// SPDX-License-Identifier: GPL-2.0-only
/* Disposable-VM-only x86_64 call/return boundary test. */
#include <linux/cpu.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/irqflags.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <asm/cpufeature.h>
#include <asm/desc.h>
#include <asm/fpu/api.h>
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <asm/processor-flags.h>
#include <asm/special_insns.h>

#include "fki.h"
#include "boundary_probe_asm.h"
#include "boundary_probe_uapi.h"

#define BP_STACK_ORDER 2
#define BP_STACK_SIZE (PAGE_SIZE << BP_STACK_ORDER)
#define BP_STACK_GUARD_BYTES sizeof(u64)
#define BP_STACK_LOW_CANARY  0x4c4f5743414e4152ULL
#define BP_STACK_HIGH_CANARY 0x4849474843414e52ULL
#define BP_RFLAGS_CONTROL (X86_EFLAGS_IF | X86_EFLAGS_DF | X86_EFLAGS_IOPL | \
			   X86_EFLAGS_NT | X86_EFLAGS_RF | X86_EFLAGS_VM | \
			   X86_EFLAGS_AC | X86_EFLAGS_VIF | X86_EFLAGS_VIP)

struct bp_asm_result {
	unsigned long host_entry_rsp;
	unsigned long host_return_rsp;
	unsigned long alt_switch_rsp;
	unsigned long alt_entry_rsp;
	unsigned long alt_return_rsp;
	unsigned long rflags_entry;
	unsigned long rflags_during;
	unsigned long rflags_inner_return;
	unsigned long rflags_return;
	long status;
	unsigned long gprs_entry[6];
	unsigned long gprs_during[6];
	unsigned long gprs_return[6];
};

struct bp_host_snapshot {
	int cpu;
	struct task_struct *task;
	unsigned long stack_page;
	unsigned long stack_end;
	unsigned long rsp;
	unsigned long rflags;
	unsigned long cr0;
	unsigned long cr3;
	unsigned long cr4;
	u64 efer;
	u64 fs_base;
	u64 gs_base;
	u64 kernel_gs_base;
	u64 xcr0;
	unsigned long gdt_base;
	unsigned long idt_base;
	unsigned int gdt_limit;
	unsigned int idt_limit;
	unsigned int ldt;
	unsigned int tr;
	unsigned int preempt_count;
	bool irq_disabled;
};

struct bp_work {
	struct x86_boundary_probe_request request;
};

static_assert(sizeof(struct bp_asm_result) == 224);
static_assert(offsetof(struct bp_asm_result, gprs_entry) == BP_OFF_GPRS_ENTRY);
static_assert(offsetof(struct bp_asm_result, gprs_during) == BP_OFF_GPRS_DURING);
static_assert(offsetof(struct bp_asm_result, gprs_return) == BP_OFF_GPRS_RETURN);
static_assert(-ECANCELED == -125);
static_assert(X86_BOUNDARY_PROBE_RUN == _IOW(0xb7, 1, struct x86_boundary_probe_request));

void x86_boundary_probe_switch_stack(void *stack_top, struct bp_asm_result *result,
				     unsigned int mode);
void x86_boundary_probe_clobber_xstate(void);

static int bp_read_xcr0(u64 *xcr0)
{
	u32 low, high;

	if (!boot_cpu_has(X86_FEATURE_XSAVE) ||
	    !boot_cpu_has(X86_FEATURE_OSXSAVE) ||
	    !boot_cpu_has(X86_FEATURE_AVX))
		return -EOPNOTSUPP;
	asm volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
	*xcr0 = ((u64)high << 32) | low;
	if ((*xcr0 & 0x6) != 0x6)
		return -EOPNOTSUPP;
	return 0;
}

static noinline int bp_snapshot(struct bp_host_snapshot *snapshot)
{
	struct desc_ptr gdt, idt;
	unsigned long flags;
	u32 low, high;
	int err;

	snapshot->cpu = raw_smp_processor_id();
	snapshot->task = current;
	snapshot->stack_page = (unsigned long)task_stack_page(current);
	snapshot->stack_end = snapshot->stack_page + THREAD_SIZE;
	asm volatile("mov %%rsp, %0\n\tpushfq\n\tpopq %1"
		     : "=r"(snapshot->rsp), "=r"(flags) :: "memory");
	snapshot->rflags = flags;
	snapshot->preempt_count = preempt_count();
	snapshot->irq_disabled = irqs_disabled();
	snapshot->cr0 = read_cr0();
	snapshot->cr3 = __read_cr3();
	snapshot->cr4 = __read_cr4();
	native_store_gdt(&gdt);
	store_idt(&idt);
	snapshot->ldt = 0;
	store_ldt(snapshot->ldt);
	store_tr(snapshot->tr);
	snapshot->gdt_base = gdt.address;
	snapshot->gdt_limit = gdt.size;
	snapshot->idt_base = idt.address;
	snapshot->idt_limit = idt.size;
	err = bp_read_xcr0(&snapshot->xcr0);
	if (err)
		return err;
	if (rdmsr_safe(MSR_FS_BASE, &low, &high))
		return -EIO;
	snapshot->fs_base = ((u64)high << 32) | low;
	if (rdmsr_safe(MSR_GS_BASE, &low, &high))
		return -EIO;
	snapshot->gs_base = ((u64)high << 32) | low;
	if (rdmsr_safe(MSR_KERNEL_GS_BASE, &low, &high))
		return -EIO;
	snapshot->kernel_gs_base = ((u64)high << 32) | low;
	if (rdmsr_safe(MSR_EFER, &low, &high))
		return -EIO;
	snapshot->efer = ((u64)high << 32) | low;
	return 0;
}

static bool bp_snapshots_match(const struct bp_host_snapshot *before,
			      const struct bp_host_snapshot *after)
{
	const unsigned long page_table_address_mask = ~((1UL << 12) - 1);
	const unsigned long pcid_mask = (1UL << 12) - 1;

	return before->cpu == after->cpu && before->task == after->task &&
	       before->stack_page == after->stack_page &&
	       before->stack_end == after->stack_end && before->rsp == after->rsp &&
	       before->preempt_count == after->preempt_count &&
	       before->irq_disabled == after->irq_disabled &&
	       before->xcr0 == after->xcr0 &&
	       before->cr0 == after->cr0 && before->cr4 == after->cr4 &&
	       before->efer == after->efer &&
	       before->gdt_base == after->gdt_base &&
	       before->gdt_limit == after->gdt_limit &&
	       before->idt_base == after->idt_base &&
	       before->idt_limit == after->idt_limit &&
	       before->ldt == after->ldt && before->tr == after->tr &&
	       ((before->rflags ^ after->rflags) & BP_RFLAGS_CONTROL) == 0 &&
	       before->cr3 == after->cr3 &&
	       (before->cr3 & page_table_address_mask) ==
		(after->cr3 & page_table_address_mask) &&
	       (before->cr3 & pcid_mask) == (after->cr3 & pcid_mask) &&
	       before->fs_base == after->fs_base && before->gs_base == after->gs_base &&
	       before->kernel_gs_base == after->kernel_gs_base;
}

static bool bp_gprs_match(const unsigned long *left, const unsigned long *right)
{
	int i;

	for (i = 0; i < 6; i++) {
		if (left[i] != right[i])
			return false;
	}
	return true;
}

static bool bp_gprs_have_sentinels(const unsigned long *gprs)
{
	static const unsigned long sentinels[6] = {
		0x5555666677778888UL, 0x1111222233334444UL,
		0x9999aaaabbbbccccUL, 0xddddeeeeffff0001UL,
		0x123456789abcdef0UL, 0xfedcba9876543210UL,
	};

	return bp_gprs_match(gprs, sentinels);
}

static long bp_run_on_cpu(void *arg)
{
	struct bp_work *work = arg;
	struct bp_host_snapshot before, after;
	struct bp_asm_result result = {};
	unsigned long irq_flags;
	unsigned long stack_base, stack_limit;
	struct page *stack_page;
	unsigned long stack_top;
	u64 *low_canary, *high_canary;
	long ret = 0;

	if (fki_caps() != 0)
		return -EOPNOTSUPP;

	stack_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, BP_STACK_ORDER);
	if (!stack_page)
		return -ENOMEM;
	stack_base = (unsigned long)page_address(stack_page);
	stack_limit = stack_base + BP_STACK_SIZE;
	low_canary = (u64 *)stack_base;
	high_canary = (u64 *)(stack_limit - BP_STACK_GUARD_BYTES);
	WRITE_ONCE(*low_canary, BP_STACK_LOW_CANARY);
	WRITE_ONCE(*high_canary, BP_STACK_HIGH_CANARY);
	stack_top = (unsigned long)high_canary;

	preempt_disable();
	local_irq_save(irq_flags);
	if (raw_smp_processor_id() != work->request.cpu) {
		ret = -EXDEV;
		goto out_restore;
	}
	ret = bp_snapshot(&before);
	if (ret)
		goto out_restore;
	if (!before.irq_disabled ||
	    before.rsp < before.stack_page || before.rsp >= before.stack_end) {
		ret = -EUCLEAN;
		goto out_restore;
	}
#ifdef CONFIG_PREEMPT_COUNT
	if (!before.preempt_count) {
		ret = -EUCLEAN;
		goto out_restore;
	}
#endif
	x86_boundary_probe_switch_stack((void *)stack_top, &result,
					work->request.mode);
	ret = bp_snapshot(&after);
	if (ret)
		goto out_restore;

	if (!bp_snapshots_match(&before, &after) ||
	    result.host_entry_rsp != result.host_return_rsp ||
	    result.host_entry_rsp < before.stack_page ||
	    result.host_entry_rsp >= before.stack_end ||
	    (result.host_entry_rsp & 0x7) != 0 ||
	    result.alt_switch_rsp < stack_base + BP_STACK_GUARD_BYTES ||
	    result.alt_switch_rsp >= stack_top ||
	    result.alt_entry_rsp < stack_base + BP_STACK_GUARD_BYTES ||
	    result.alt_entry_rsp >= stack_top ||
	    (result.alt_entry_rsp & 0xf) != 8 ||
	    (result.alt_switch_rsp & 0xf) != 0 ||
	    result.alt_return_rsp < stack_base + BP_STACK_GUARD_BYTES ||
	    result.alt_return_rsp >= stack_top ||
	    READ_ONCE(*low_canary) != BP_STACK_LOW_CANARY ||
	    READ_ONCE(*high_canary) != BP_STACK_HIGH_CANARY ||
	    result.rflags_entry != result.rflags_return ||
	    ((result.rflags_entry ^ result.rflags_inner_return) &
	     (X86_EFLAGS_DF | X86_EFLAGS_CF)) != (X86_EFLAGS_DF | X86_EFLAGS_CF) ||
	    !(result.rflags_during & X86_EFLAGS_DF) ||
	    !(result.rflags_during & X86_EFLAGS_CF) ||
	    !bp_gprs_match(result.gprs_entry, result.gprs_return) ||
	    !bp_gprs_have_sentinels(result.gprs_during)) {
		pr_err("x86-boundary-probe: state mismatch on cpu %u mode %u\n",
		       work->request.cpu, work->request.mode);
		ret = -EUCLEAN;
	} else if (work->request.mode == X86_BOUNDARY_PROBE_MODE_RETURN) {
		if (result.status != 0)
			ret = -EUCLEAN;
	} else if (result.status != -ECANCELED) {
		ret = -EUCLEAN;
	}

out_restore:
	local_irq_restore(irq_flags);
	preempt_enable();
	__free_pages(stack_page, BP_STACK_ORDER);
	if (ret)
		return ret;
	pr_info("x86-boundary-probe: cpu %u mode %u restored host state "
		"shim_entry_rsp_mod16=%lu preempt_count=%u "
		"irqs_disabled=%u xcr0=%#llx stack_canaries=ok\n",
		work->request.cpu, work->request.mode, result.alt_entry_rsp & 0xf,
		before.preempt_count, before.irq_disabled,
		(unsigned long long)before.xcr0);
	return result.status;
}

static long bp_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
	struct x86_boundary_probe_request request;
	struct bp_work work;
	long ret;

	if (command != X86_BOUNDARY_PROBE_RUN)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (request.mode != X86_BOUNDARY_PROBE_MODE_RETURN &&
	    request.mode != X86_BOUNDARY_PROBE_MODE_ABORT)
		return -EINVAL;
	if (request.cpu >= nr_cpu_ids || !cpu_online(request.cpu))
		return -EINVAL;
	if (fki_caps() != 0)
		return -EOPNOTSUPP;

	/* Exercise kernel XSAVE/SIMD save/restore in this ioctl's calling task. */
	kernel_fpu_begin();
	x86_boundary_probe_clobber_xstate();
	kernel_fpu_end();

	work.request = request;
	ret = work_on_cpu(request.cpu, bp_run_on_cpu, &work);
	return ret;
}

static const struct file_operations bp_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = bp_ioctl,
};

static struct miscdevice bp_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "x86-boundary-probe",
	.fops = &bp_fops,
};

static int __init bp_init(void)
{
	u64 xcr0;
	int err;

	if (fki_caps() != 0) {
		pr_err("x86-boundary-probe: refusing nonzero FKI capabilities\n");
		return -EOPNOTSUPP;
	}
	err = bp_read_xcr0(&xcr0);
	if (err) {
		pr_err("x86-boundary-probe: XSAVE/OSXSAVE/AVX and XMM/YMM XCR0 state are required\n");
		return err;
	}
	pr_info("x86-boundary-probe: test-only xcr0=%#llx; fki_caps=0\n",
		(unsigned long long)xcr0);
	return misc_register(&bp_device);
}

static void __exit bp_exit(void)
{
	misc_deregister(&bp_device);
}

module_init(bp_init);
module_exit(bp_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Disposable test-only x86_64 host-state boundary probe");
