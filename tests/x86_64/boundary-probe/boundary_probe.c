// SPDX-License-Identifier: GPL-2.0-only
/* Disposable-VM-only x86_64 call/return boundary test. */
#include <linux/cpu.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
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
#define BP_RFLAGS_CONTROL (X86_EFLAGS_IF | X86_EFLAGS_DF | X86_EFLAGS_IOPL | \
			   X86_EFLAGS_NT | X86_EFLAGS_RF | X86_EFLAGS_VM | \
			   X86_EFLAGS_AC | X86_EFLAGS_VIF | X86_EFLAGS_VIP)

struct bp_asm_result {
	unsigned long host_entry_rsp;
	unsigned long host_return_rsp;
	unsigned long alt_switch_rsp;
	unsigned long alt_inner_rsp;
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
	unsigned long rsp;
	unsigned long rflags;
	unsigned long cr3;
	u64 fs_base;
	u64 gs_base;
	u64 kernel_gs_base;
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

static noinline int bp_snapshot(struct bp_host_snapshot *snapshot)
{
	unsigned long flags;
	u32 low, high;

	snapshot->cpu = raw_smp_processor_id();
	snapshot->task = current;
	snapshot->stack_page = (unsigned long)task_stack_page(current);
	asm volatile("mov %%rsp, %0\n\tpushfq\n\tpopq %1"
		     : "=r"(snapshot->rsp), "=r"(flags) :: "memory");
	snapshot->rflags = flags;
	snapshot->cr3 = __read_cr3();
	if (rdmsr_safe(MSR_FS_BASE, &low, &high))
		return -EIO;
	snapshot->fs_base = ((u64)high << 32) | low;
	if (rdmsr_safe(MSR_GS_BASE, &low, &high))
		return -EIO;
	snapshot->gs_base = ((u64)high << 32) | low;
	if (rdmsr_safe(MSR_KERNEL_GS_BASE, &low, &high))
		return -EIO;
	snapshot->kernel_gs_base = ((u64)high << 32) | low;
	return 0;
}

static bool bp_snapshots_match(const struct bp_host_snapshot *before,
			      const struct bp_host_snapshot *after)
{
	const unsigned long page_table_address_mask = ~((1UL << 12) - 1);
	const unsigned long pcid_mask = (1UL << 12) - 1;

	return before->cpu == after->cpu && before->task == after->task &&
	       before->stack_page == after->stack_page && before->rsp == after->rsp &&
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
	struct page *stack_page;
	unsigned long stack_top;
	long ret = 0;

	if (fki_caps() != 0)
		return -EOPNOTSUPP;

	stack_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, BP_STACK_ORDER);
	if (!stack_page)
		return -ENOMEM;
	stack_top = (unsigned long)page_address(stack_page) + BP_STACK_SIZE;

	preempt_disable();
	local_irq_save(irq_flags);
	if (raw_smp_processor_id() != work->request.cpu) {
		ret = -EXDEV;
		goto out_restore;
	}
	ret = bp_snapshot(&before);
	if (ret)
		goto out_restore;
	x86_boundary_probe_switch_stack((void *)stack_top, &result,
					work->request.mode);
	ret = bp_snapshot(&after);
	if (ret)
		goto out_restore;

	if (!bp_snapshots_match(&before, &after) ||
	    result.host_entry_rsp != result.host_return_rsp ||
	    result.alt_switch_rsp < (unsigned long)page_address(stack_page) ||
	    result.alt_switch_rsp >= stack_top ||
	    result.alt_inner_rsp < (unsigned long)page_address(stack_page) ||
	    result.alt_inner_rsp >= stack_top ||
	    result.alt_return_rsp < (unsigned long)page_address(stack_page) ||
	    result.alt_return_rsp >= stack_top ||
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
	pr_info("x86-boundary-probe: cpu %u mode %u restored host state\n",
		work->request.cpu, work->request.mode);
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
	.llseek = noop_llseek,
};

static struct miscdevice bp_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "x86-boundary-probe",
	.fops = &bp_fops,
};

static int __init bp_init(void)
{
	if (fki_caps() != 0) {
		pr_err("x86-boundary-probe: refusing nonzero FKI capabilities\n");
		return -EOPNOTSUPP;
	}
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
