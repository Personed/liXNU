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
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <asm/cpufeature.h>
#include <asm/desc.h>
#include <asm/fpu/api.h>
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <asm/processor-flags.h>
#include <asm/special_insns.h>
#include <asm/trapnr.h>

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

struct bp_exception_control {
	u64 magic;
	u64 state;
	u64 expected_vector;
	u64 expected_rip;
	u64 post_injection_rip;
	u64 recovery_stack_top;
	u64 recovery_low_canary_ptr;
	u64 recovery_high_canary_ptr;
	u64 envelope_rsp;
	u64 final_rsp;
	struct desc_ptr original_desc;
	struct desc_ptr shadow_desc;
	struct desc_ptr readback_desc;
	u64 terminal_code;
	u64 recovery_count;
	u64 fault_vector;
	u64 fault_error;
	u64 fault_rip;
	u64 fault_rflags;
	u64 fault_rsp;
	u64 fault_cs;
	u64 fault_ss;
	u64 fault_cr2;
	u64 proof_flags;
	u64 fault_scratch[4];
	u64 recovery_scratch[4];
	u64 fault_address;
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
static_assert(sizeof(struct bp_exception_control) == BPX_SIZE);
static_assert(offsetof(struct bp_exception_control, original_desc) == BPX_OFF_ORIGINAL_DESC);
static_assert(offsetof(struct bp_exception_control, shadow_desc) == BPX_OFF_SHADOW_DESC);
static_assert(offsetof(struct bp_exception_control, readback_desc) == BPX_OFF_READBACK_DESC);
static_assert(offsetof(struct bp_exception_control, terminal_code) == BPX_OFF_TERMINAL_CODE);
static_assert(offsetof(struct bp_exception_control, fault_scratch) == BPX_OFF_FAULT_SCRATCH_RAX);
static_assert(offsetof(struct bp_exception_control, recovery_scratch) == BPX_OFF_REC_SCRATCH_RAX);
static_assert(offsetof(struct bp_exception_control, fault_address) == BPX_OFF_FAULT_ADDRESS);
static_assert(BP_MODE_EXCEPTION_PF == X86_BOUNDARY_PROBE_MODE_EXCEPTION_PF);
static_assert(BP_MODE_EXCEPTION_GP == X86_BOUNDARY_PROBE_MODE_EXCEPTION_GP);
static_assert(BPX_RECOVERY_LOW_CANARY == BP_STACK_LOW_CANARY);
static_assert(BPX_RECOVERY_HIGH_CANARY == BP_STACK_HIGH_CANARY);

void x86_boundary_probe_switch_stack(void *stack_top, struct bp_asm_result *result,
				     unsigned int mode);
void x86_boundary_probe_clobber_xstate(void);
void x86_boundary_probe_gp_handler(void);
void x86_boundary_probe_pf_handler(void);
extern const unsigned long x86_boundary_probe_terminal_vectors[32];

struct bp_exception_control x86_boundary_probe_exception_control;
static DEFINE_MUTEX(bp_ioctl_mutex);

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

static bool bp_mode_exception(unsigned int mode)
{
	return mode == X86_BOUNDARY_PROBE_MODE_EXCEPTION_PF ||
	       mode == X86_BOUNDARY_PROBE_MODE_EXCEPTION_GP;
}

static int bp_prepare_exception(unsigned int mode,
				const struct bp_host_snapshot *before,
				void *shadow_idt, unsigned long recovery_base)
{
	struct bp_exception_control *control = &x86_boundary_probe_exception_control;
	gate_desc *gates = shadow_idt;
	size_t idt_bytes = before->idt_limit + 1;
	u64 *low_canary = (u64 *)recovery_base;
	u64 *high_canary = (u64 *)(recovery_base + PAGE_SIZE - sizeof(u64));

	if (!idt_bytes || idt_bytes > PAGE_SIZE)
		return -EOPNOTSUPP;
	memcpy(shadow_idt, (const void *)before->idt_base, idt_bytes);
	if (!gates[X86_TRAP_DF].bits.ist)
		return -EOPNOTSUPP;
	for (int vector = 0; vector < 32; vector++) {
		if (vector == X86_TRAP_NMI || vector == X86_TRAP_PF || vector == X86_TRAP_GP)
			continue;
		pack_gate(&gates[vector], GATE_INTERRUPT,
			  x86_boundary_probe_terminal_vectors[vector], 0,
			  gates[vector].bits.ist, 0);
	}
	pack_gate(&gates[X86_TRAP_PF], GATE_INTERRUPT,
		  (unsigned long)x86_boundary_probe_pf_handler, 0, 0, 0);
	pack_gate(&gates[X86_TRAP_GP], GATE_INTERRUPT,
		  (unsigned long)x86_boundary_probe_gp_handler, 0, 0, 0);

	WRITE_ONCE(*low_canary, BPX_RECOVERY_LOW_CANARY);
	WRITE_ONCE(*high_canary, BPX_RECOVERY_HIGH_CANARY);
	{
		u8 probe_byte;
		void *fault_address = (void *)(recovery_base + PAGE_SIZE);

		if (!copy_from_kernel_nofault(&probe_byte, fault_address, sizeof(probe_byte)))
			return -EOPNOTSUPP;
		memset(control, 0, sizeof(*control));
		control->fault_address = (unsigned long)fault_address;
	}
	control->magic = BPX_MAGIC;
	control->state = BPX_STATE_DISARMED;
	control->expected_vector = mode == X86_BOUNDARY_PROBE_MODE_EXCEPTION_PF ?
		X86_TRAP_PF : X86_TRAP_GP;
	control->original_desc.address = before->idt_base;
	control->original_desc.size = before->idt_limit;
	control->shadow_desc.address = (unsigned long)shadow_idt;
	control->shadow_desc.size = before->idt_limit;
	control->recovery_stack_top = recovery_base + PAGE_SIZE - 2 * sizeof(u64);
	control->recovery_low_canary_ptr = recovery_base;
	control->recovery_high_canary_ptr = recovery_base + PAGE_SIZE - sizeof(u64);
	return 0;
}

static bool bp_replaced_gate_byte(size_t byte)
{
	size_t vector = byte / sizeof(gate_desc);
	size_t offset = byte % sizeof(gate_desc);

	return vector < 32 && vector != X86_TRAP_NMI && offset < sizeof(gate_desc);
}

static bool bp_shadow_idt_matches(const struct bp_host_snapshot *before,
				  const void *shadow_idt)
{
	const u8 *original = (const u8 *)before->idt_base;
	const u8 *shadow = shadow_idt;
	size_t byte, bytes = before->idt_limit + 1;

	for (byte = 0; byte < bytes; byte++) {
		if (!bp_replaced_gate_byte(byte) && original[byte] != shadow[byte])
			return false;
	}
	return true;
}

static bool bp_shadow_gates_valid(const struct bp_host_snapshot *before,
				  const void *shadow_idt)
{
	const gate_desc *original = (const gate_desc *)before->idt_base;
	const gate_desc *shadow = shadow_idt;

	for (int vector = 0; vector < 32; vector++) {
		if (vector == X86_TRAP_NMI)
			continue;
		if (vector == X86_TRAP_PF) {
			if (gate_offset(&shadow[vector]) != (unsigned long)x86_boundary_probe_pf_handler ||
			    shadow[vector].bits.ist)
				return false;
			continue;
		}
		if (vector == X86_TRAP_GP) {
			if (gate_offset(&shadow[vector]) != (unsigned long)x86_boundary_probe_gp_handler ||
			    shadow[vector].bits.ist)
				return false;
			continue;
		}
		if (gate_offset(&shadow[vector]) != x86_boundary_probe_terminal_vectors[vector] ||
		    shadow[vector].bits.ist != original[vector].bits.ist)
			return false;
	}
	return true;
}

static bool bp_exception_valid(const struct bp_exception_control *control,
			       const struct bp_host_snapshot *before,
			       const void *shadow_idt)
{
	u64 expected_vector = control->expected_vector;
	const u64 *low = (const u64 *)(unsigned long)control->recovery_low_canary_ptr;
	const u64 *high = (const u64 *)(unsigned long)control->recovery_high_canary_ptr;
	u64 expected_proofs = BPX_PROOF_CANARIES | BPX_PROOF_IDTR | BPX_PROOF_RSP;

	if (!bp_shadow_idt_matches(before, shadow_idt) ||
	    !bp_shadow_gates_valid(before, shadow_idt) ||
	    control->terminal_code || control->magic != BPX_MAGIC ||
	    control->state != BPX_STATE_DISARMED || control->recovery_count != 1 ||
	    control->proof_flags != expected_proofs ||
	    !control->expected_rip || control->fault_rip != control->expected_rip ||
	    control->fault_vector != expected_vector ||
	    control->fault_rsp != control->envelope_rsp ||
	    control->final_rsp != control->envelope_rsp || !control->fault_cs ||
	    READ_ONCE(*low) != BPX_RECOVERY_LOW_CANARY ||
	    READ_ONCE(*high) != BPX_RECOVERY_HIGH_CANARY)
		return false;
	if (expected_vector == X86_TRAP_PF)
		return control->fault_cr2 == control->fault_address &&
	       (control->fault_error & 0x7) == 0;
	return control->fault_error == 0;
}

static long bp_run_on_cpu(void *arg)
{
	struct bp_work *work = arg;
	struct bp_host_snapshot before, after;
	struct bp_asm_result result = {};
	unsigned long irq_flags;
	unsigned long stack_base, stack_limit;
	struct page *stack_page, *shadow_page = NULL;
	void *recovery_area = NULL;
	unsigned long stack_top;
	unsigned long recovery_base = 0;
	void *shadow_idt = NULL;
	u64 *low_canary, *high_canary;
	long ret = 0;

	if (fki_caps() != 0)
		return -EOPNOTSUPP;

	stack_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, BP_STACK_ORDER);
	if (!stack_page)
		return -ENOMEM;
	shadow_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	recovery_area = vmalloc(PAGE_SIZE);
	if (!shadow_page || !recovery_area) {
		ret = -ENOMEM;
		goto out_free;
	}
	shadow_idt = page_address(shadow_page);
	recovery_base = (unsigned long)recovery_area;
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
	if (bp_mode_exception(work->request.mode)) {
		ret = bp_prepare_exception(work->request.mode, &before, shadow_idt,
					   recovery_base);
		if (ret)
			goto out_restore;
	}
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
	} else if (work->request.mode == X86_BOUNDARY_PROBE_MODE_RETURN ||
		   bp_mode_exception(work->request.mode)) {
		if (result.status != 0)
			ret = -EUCLEAN;
	} else if (result.status != -ECANCELED) {
		ret = -EUCLEAN;
	}
	if (!ret && bp_mode_exception(work->request.mode) &&
	    !bp_exception_valid(&x86_boundary_probe_exception_control, &before,
				 shadow_idt)) {
		pr_err("x86-boundary-probe: exception recovery mismatch on cpu %u mode %u "
		       "count=%llu state=%llu proofs=%#llx terminal=%#llx\n",
		       work->request.cpu, work->request.mode,
		       x86_boundary_probe_exception_control.recovery_count,
		       x86_boundary_probe_exception_control.state,
		       x86_boundary_probe_exception_control.proof_flags,
		       x86_boundary_probe_exception_control.terminal_code);
		ret = -EUCLEAN;
	}

out_restore:
	local_irq_restore(irq_flags);
	preempt_enable();
out_free:
	if (shadow_page)
		__free_page(shadow_page);
	if (recovery_area)
		vfree(recovery_area);
	__free_pages(stack_page, BP_STACK_ORDER);
	if (ret)
		return ret;
	if (bp_mode_exception(work->request.mode))
		pr_info("x86-boundary-probe: cpu %u mode %u exception vector=%llu rip=%#llx "
			"error=%#llx cr2=%#llx recoveries=%llu idtr/rsp/canary proofs=%#llx\n",
			work->request.cpu, work->request.mode,
			x86_boundary_probe_exception_control.fault_vector,
			x86_boundary_probe_exception_control.fault_rip,
			x86_boundary_probe_exception_control.fault_error,
			x86_boundary_probe_exception_control.fault_cr2,
			x86_boundary_probe_exception_control.recovery_count,
			x86_boundary_probe_exception_control.proof_flags);
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
	    request.mode != X86_BOUNDARY_PROBE_MODE_ABORT &&
	    request.mode != X86_BOUNDARY_PROBE_MODE_EXCEPTION_PF &&
	    request.mode != X86_BOUNDARY_PROBE_MODE_EXCEPTION_GP)
		return -EINVAL;
	if (request.cpu >= nr_cpu_ids || !cpu_online(request.cpu))
		return -EINVAL;
	if (fki_caps() != 0)
		return -EOPNOTSUPP;

	/* Exercise x87/MXCSR/SIMD save and restore in the ioctl's calling task. */
	kernel_fpu_begin_mask(KFPU_387 | KFPU_MXCSR);
	x86_boundary_probe_clobber_xstate();
	kernel_fpu_end();

	work.request = request;
	mutex_lock(&bp_ioctl_mutex);
	ret = work_on_cpu(request.cpu, bp_run_on_cpu, &work);
	mutex_unlock(&bp_ioctl_mutex);
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
