// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cc_platform.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/string.h>
#include <asm/cpufeature.h>
#include <asm/io.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <asm/special_insns.h>

#include "cr3_switch_layout.h"
#include "fki_x86_64_cr3.h"

#define CR3_PROBE_SLOT  16
#define CR3_PROBE_ALIAS ((unsigned long)CR3_PROBE_SLOT << 39)
#define CR3_PROBE_CANARY 0x4352335357495443ULL

enum cr3_probe_page {
	PROBE_ROOT,
	PROBE_PDPT,
	PROBE_PD,
	PROBE_PT,
	PROBE_MARKER,
	PROBE_NR_PAGES,
};

struct cr3_switch_observation {
	unsigned long before;
	unsigned long active;
	unsigned long after;
	u64 value;
};

int x86_cr3_switch_once(unsigned long private_cr3, unsigned long alias,
			 bool abort, struct cr3_switch_observation *out);

static bool qemu_tcg(void)
{
	unsigned int vendor[3], max_leaf;

	if (!boot_cpu_has(X86_FEATURE_HYPERVISOR))
		return false;
	cpuid(0x40000000, &max_leaf, &vendor[0], &vendor[1], &vendor[2]);
	return max_leaf >= 0x40000000 &&
	       !memcmp(vendor, "TCGTCGTCGTCG", sizeof(vendor));
}

static int check_profile(void)
{
	unsigned long cr4 = __read_cr4();
	unsigned long unsupported = X86_CR4_PCIDE | X86_CR4_LA57;
	u64 efer;

#ifdef X86_CR4_LAM_SUP
	unsupported |= X86_CR4_LAM_SUP;
#endif
#ifdef X86_CR4_LASS
	unsupported |= X86_CR4_LASS;
#endif
	if (!qemu_tcg()) {
		pr_err("x86-cr3-switch: requires the QEMU TCG CPUID signature\n");
		return -EOPNOTSUPP;
	}
	if (PAGE_SIZE != FKI_X86_CR3_PAGE_SIZE || boot_cpu_has(X86_FEATURE_PTI) ||
	    cc_platform_has(CC_ATTR_MEM_ENCRYPT) || (cr4 & unsupported)) {
		pr_err("x86-cr3-switch: unsupported TCG paging profile (CR4=%#lx required-clear=%#lx PTI=%d encryption=%d)\n",
		       cr4, unsupported, boot_cpu_has(X86_FEATURE_PTI),
		       cc_platform_has(CC_ATTR_MEM_ENCRYPT));
		return -EOPNOTSUPP;
	}
	if (num_possible_cpus() != 1) {
		pr_err("x86-cr3-switch: requires exactly one possible CPU\n");
		return -EOPNOTSUPP;
	}
	rdmsrl(MSR_EFER, efer);
	if (!(efer & EFER_NX))
		return -EOPNOTSUPP;
	return 0;
}

/*
 * The single possible CPU keeps the copied Linux root stable while the
 * caller disables IRQs and preemption. All original mappings are copied,
 * including those used by asynchronous handlers. The only private leaf
 * is supervisor-only, read-only, NX and non-global. Linux receives no
 * page-table edits and keeps identical mappings for its global translations.
 */
static int run_switch(struct page **pages, bool abort)
{
	struct cr3_switch_observation obs = {};
	struct fki_x86_cr3 saved;
	unsigned long irq_flags, saved_raw, private_raw, pfn;
	u64 *linux_root, *private_root = page_address(pages[PROBE_ROOT]);
	int ret;

	preempt_disable();
	local_irq_save(irq_flags);
	saved_raw = __read_cr3();
	ret = fki_x86_decode_cr3(saved_raw, boot_cpu_data.x86_phys_bits,
				 false, false, &saved);
	if (ret)
		goto out;
	pfn = saved.root_pa >> PAGE_SHIFT;
	if (!pfn_valid(pfn)) {
		ret = -EFAULT;
		goto out;
	}
	linux_root = page_address(pfn_to_page(pfn));
	if (!linux_root || READ_ONCE(linux_root[CR3_PROBE_SLOT])) {
		ret = -EBUSY;
		goto out;
	}
	private_raw = page_to_phys(pages[PROBE_ROOT]) | saved.cache_flags;
	if (private_raw == saved_raw) {
		ret = -EINVAL;
		goto out;
	}
	memcpy(private_root, linux_root, PAGE_SIZE);
	private_root[CR3_PROBE_SLOT] = page_to_phys(pages[PROBE_PDPT]) |
				     _PAGE_PRESENT;
	ret = x86_cr3_switch_once(private_raw, CR3_PROBE_ALIAS, abort, &obs);
	/* Never release the private root or resume scheduling with it live. */
	if (unlikely(__read_cr3() != saved_raw))
		panic("x86-cr3-switch: failed to restore Linux CR3");
	if (ret != (abort ? -ECANCELED : 0) || obs.before != saved_raw ||
	    obs.active != private_raw || obs.after != saved_raw ||
	    obs.value != CR3_PROBE_CANARY ||
	    READ_ONCE(linux_root[CR3_PROBE_SLOT])) {
		ret = -EIO;
		goto out;
	}
	ret = 0;
out:
	local_irq_restore(irq_flags);
	preempt_enable();
	return ret;
}

static int __init cr3_switch_probe_init(void)
{
	struct page *pages[PROBE_NR_PAGES];
	unsigned int i;
	int ret;

	static_assert(offsetof(struct cr3_switch_observation, before) == CR3_SWITCH_BEFORE);
	static_assert(offsetof(struct cr3_switch_observation, active) == CR3_SWITCH_ACTIVE);
	static_assert(offsetof(struct cr3_switch_observation, after) == CR3_SWITCH_AFTER);
	static_assert(offsetof(struct cr3_switch_observation, value) == CR3_SWITCH_VALUE);
	ret = check_profile();
	if (ret)
		return ret;
	for (i = 0; i < PROBE_NR_PAGES; i++) {
		pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!pages[i]) {
			ret = -ENOMEM;
			goto free_pages;
		}
	}
	*(u64 *)page_address(pages[PROBE_PDPT]) =
		page_to_phys(pages[PROBE_PD]) | _PAGE_PRESENT;
	*(u64 *)page_address(pages[PROBE_PD]) =
		page_to_phys(pages[PROBE_PT]) | _PAGE_PRESENT;
	*(u64 *)page_address(pages[PROBE_PT]) =
		page_to_phys(pages[PROBE_MARKER]) | _PAGE_PRESENT | _PAGE_NX;
	*(u64 *)page_address(pages[PROBE_MARKER]) = CR3_PROBE_CANARY;

	ret = run_switch(pages, false);
	if (!ret) {
		pr_info("x86-cr3-switch: return verified\n");
		ret = run_switch(pages, true);
		if (!ret)
			pr_info("x86-cr3-switch: abort verified\n");
	}
free_pages:
	while (i)
		__free_page(pages[--i]);
	return ret;
}

static void __exit cr3_switch_probe_exit(void)
{
}

module_init(cr3_switch_probe_init);
module_exit(cr3_switch_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Disposable QEMU TCG x86 CR3 switch probe");
