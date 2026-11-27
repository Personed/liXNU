// SPDX-License-Identifier: GPL-2.0-only
/* Disposable-VM-only x86_64 call/return boundary test. */
#include <linux/cpu.h>
#include <linux/atomic.h>
#include <linux/build_bug.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/irqflags.h>
#include <linux/kthread.h>
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
#include "task_continuation_probe.h"

#define BP_STACK_ORDER 2
#define BP_STACK_SIZE (PAGE_SIZE << BP_STACK_ORDER)
#define BP_STACK_GUARD_BYTES sizeof(u64)
#define BP_STACK_LOW_CANARY  0x4c4f5743414e4152ULL
#define BP_STACK_HIGH_CANARY 0x4849474843414e52ULL
#define BP_RFLAGS_CONTROL (X86_EFLAGS_IF | X86_EFLAGS_DF | X86_EFLAGS_IOPL | \
			   X86_EFLAGS_NT | X86_EFLAGS_RF | X86_EFLAGS_VM | \
			   X86_EFLAGS_AC | X86_EFLAGS_VIF | X86_EFLAGS_VIP)
#define BP_COOP_REPLY_TIMEOUT_MS 2000
#define BP_COOP_CANCEL_TIMEOUT_MS 200
#define BP_COOP_TEARDOWN_TIMEOUT_MS 2000

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
	unsigned long coop_generation;
	unsigned long coop_token;
	unsigned long coop_reply;
	unsigned long coop_resume_count;
	unsigned long coop_state;
	unsigned long coop_resume_rsp;
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

struct bp_coop_context {
	struct x86_boundary_probe_request request;
	struct bp_asm_result result;
	struct page *stack_page;
	unsigned long stack_base;
	unsigned long stack_top;
	struct task_struct *owner;
	struct task_struct *helper;
	struct completion request_ready;
	struct completion reply_ready;
	struct completion owner_cancelled;
	struct completion owner_done;
	struct completion helper_done;
	atomic_t state;
	atomic_t late_reply_rejected;
	unsigned long late_reply_value;
	unsigned long owner_nvcsw_before;
	unsigned long owner_nvcsw_after;
	unsigned long helper_nvcsw_before;
	unsigned long helper_nvcsw_after;
	bool timeout_case;
	long owner_status;
	long helper_status;
};

static atomic64_t bp_coop_generation = ATOMIC64_INIT(0);

static_assert(sizeof(struct bp_asm_result) == 272);
static_assert(offsetof(struct bp_asm_result, gprs_entry) == BP_OFF_GPRS_ENTRY);
static_assert(offsetof(struct bp_asm_result, gprs_during) == BP_OFF_GPRS_DURING);
static_assert(offsetof(struct bp_asm_result, gprs_return) == BP_OFF_GPRS_RETURN);
static_assert(offsetof(struct bp_asm_result, coop_generation) == BP_OFF_COOP_GENERATION);
static_assert(offsetof(struct bp_asm_result, coop_token) == BP_OFF_COOP_TOKEN);
static_assert(offsetof(struct bp_asm_result, coop_reply) == BP_OFF_COOP_REPLY);
static_assert(offsetof(struct bp_asm_result, coop_resume_count) == BP_OFF_COOP_RESUME_COUNT);
static_assert(offsetof(struct bp_asm_result, coop_state) == BP_OFF_COOP_STATE);
static_assert(offsetof(struct bp_asm_result, coop_resume_rsp) == BP_OFF_COOP_RESUME_RSP);
static_assert(-ECANCELED == -125);
static_assert(X86_BOUNDARY_PROBE_RUN == _IOW(0xb7, 1, struct x86_boundary_probe_request));
static_assert(X86_BOUNDARY_PROBE_COOPERATIVE ==
	      _IOW(0xb7, 2, struct x86_boundary_probe_request));
static_assert(X86_BOUNDARY_PROBE_TASK_CONTINUATION ==
	      _IOW(0xb7, 3, struct x86_boundary_continuation_request));

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

static bool bp_snapshots_match_internal(const struct bp_host_snapshot *before,
					const struct bp_host_snapshot *after,
					bool compare_rsp)
{
	const unsigned long page_table_address_mask = ~((1UL << 12) - 1);
	const unsigned long pcid_mask = (1UL << 12) - 1;

	return before->cpu == after->cpu && before->task == after->task &&
	       before->stack_page == after->stack_page &&
	       before->stack_end == after->stack_end &&
	       (!compare_rsp || before->rsp == after->rsp) &&
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

static bool bp_snapshots_match(const struct bp_host_snapshot *before,
			      const struct bp_host_snapshot *after)
{
	return bp_snapshots_match_internal(before, after, true);
}

static bool bp_task_identity_match(const struct bp_host_snapshot *before,
				   const struct bp_host_snapshot *after)
{
	return before->cpu == after->cpu && before->task == after->task &&
	       before->stack_page == after->stack_page &&
	       before->stack_end == after->stack_end &&
	       before->preempt_count == after->preempt_count &&
	       !before->irq_disabled && !after->irq_disabled;
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

static int bp_task_snapshot(struct bp_coop_context *ctx, struct task_struct *task,
			    struct bp_host_snapshot *snapshot)
{
	unsigned long stack_marker;
	int ret;

	preempt_disable();
	if (raw_smp_processor_id() != ctx->request.cpu || current != task ||
	    irqs_disabled()) {
		ret = -EXDEV;
	} else if ((unsigned long)&stack_marker <
		   (unsigned long)task_stack_page(current) ||
		   (unsigned long)&stack_marker >=
		   (unsigned long)task_stack_page(current) + THREAD_SIZE) {
		ret = -EUCLEAN;
	} else {
		ret = bp_snapshot(snapshot);
		if (!ret && (snapshot->cpu != ctx->request.cpu ||
			     snapshot->task != task ||
			     snapshot->rsp < snapshot->stack_page ||
			     snapshot->rsp >= snapshot->stack_end))
			ret = -EUCLEAN;
	}
	preempt_enable();
	return ret;
}

/* The shim returns to this Linux task before waiting; it never blocks on its probe stack. */
static int bp_coop_run_shim(struct bp_coop_context *ctx, unsigned int mode,
			    long expected_status)
{
	struct bp_host_snapshot before, after;
	unsigned long irq_flags;
	unsigned long stack_marker;
	int ret = 0;

	preempt_disable();
	local_irq_save(irq_flags);
	if (raw_smp_processor_id() != ctx->request.cpu || current != ctx->owner ||
	    !irqs_disabled() ||
	    (unsigned long)&stack_marker <
		(unsigned long)task_stack_page(current) ||
	    (unsigned long)&stack_marker >=
		(unsigned long)task_stack_page(current) + THREAD_SIZE) {
		ret = -EXDEV;
		goto out_restore;
	}
	ret = bp_snapshot(&before);
	if (ret)
		goto out_restore;
	x86_boundary_probe_switch_stack((void *)ctx->stack_top, &ctx->result, mode);
	ret = bp_snapshot(&after);
	if (ret)
		goto out_restore;
	if (!bp_snapshots_match(&before, &after) ||
	    ctx->result.host_entry_rsp != ctx->result.host_return_rsp ||
	    ctx->result.rflags_entry != ctx->result.rflags_return ||
	    !bp_gprs_match(ctx->result.gprs_entry, ctx->result.gprs_return) ||
	    (mode == BP_COOP_MODE_RESUME &&
	     ctx->result.coop_resume_rsp != ctx->result.alt_entry_rsp)) {
		ret = -EUCLEAN;
		goto out_restore;
	}
	if (ctx->result.status != expected_status)
		ret = ctx->result.status ? ctx->result.status : -EUCLEAN;
out_restore:
	local_irq_restore(irq_flags);
	preempt_enable();
	return ret;
}

static int bp_wait_for_owner_switch(struct bp_coop_context *ctx)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(BP_COOP_TEARDOWN_TIMEOUT_MS);

	while (time_before(jiffies, deadline)) {
		if (READ_ONCE(ctx->owner->nvcsw) > ctx->owner_nvcsw_before)
			return 0;
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(1);
	}
	__set_current_state(TASK_RUNNING);
	return -ETIMEDOUT;
}

static int bp_coop_helper_thread(void *arg)
{
	struct bp_coop_context *ctx = arg;
	struct bp_host_snapshot before, after;
	unsigned long waited;
	unsigned long helper_nvcsw_before;
	int old, ret;

	if (kthread_should_stop())
		return 0;
	waited = wait_for_completion_timeout(&ctx->request_ready,
					    msecs_to_jiffies(BP_COOP_TEARDOWN_TIMEOUT_MS));
	if (!waited) {
		ctx->helper_status = -ETIMEDOUT;
		complete(&ctx->reply_ready);
		complete(&ctx->helper_done);
		return 0;
	}
	ret = bp_task_snapshot(ctx, ctx->helper, &before);
	if (ret) {
		ctx->helper_status = ret;
		complete(&ctx->reply_ready);
		complete(&ctx->helper_done);
		return 0;
	}
	helper_nvcsw_before = READ_ONCE(current->nvcsw);
	ctx->helper_nvcsw_before = helper_nvcsw_before;
	if (ctx->timeout_case) {
		waited = wait_for_completion_timeout(&ctx->owner_cancelled,
						     msecs_to_jiffies(BP_COOP_TEARDOWN_TIMEOUT_MS));
		if (!waited) {
			ctx->helper_status = -ETIMEDOUT;
			complete(&ctx->helper_done);
			return 0;
		}
		ret = bp_task_snapshot(ctx, ctx->helper, &after);
		ctx->helper_nvcsw_after = READ_ONCE(current->nvcsw);
		if (ret || !bp_task_identity_match(&before, &after)) {
			ctx->helper_status = ret ? ret : -EUCLEAN;
			complete(&ctx->helper_done);
			return 0;
		}
		WRITE_ONCE(ctx->late_reply_value,
			   ctx->result.coop_generation ^ BP_COOP_REPLY_XOR);
		old = atomic_cmpxchg(&ctx->state, BP_COOP_STATE_PENDING,
				     BP_COOP_STATE_REPLYING);
		if (old != BP_COOP_STATE_CANCELLED) {
			ctx->helper_status = -EUCLEAN;
		} else {
			/* The attempted late transition must not publish a reply. */
			atomic_set(&ctx->late_reply_rejected, 1);
			ctx->helper_status = 0;
		}
		complete(&ctx->helper_done);
		return 0;
	}

	ret = bp_wait_for_owner_switch(ctx);
	if (ret) {
		ctx->helper_status = ret;
		complete(&ctx->reply_ready);
		complete(&ctx->helper_done);
		return 0;
	}
	ret = bp_task_snapshot(ctx, ctx->helper, &after);
	ctx->helper_nvcsw_after = READ_ONCE(current->nvcsw);
	if (ret || !bp_task_identity_match(&before, &after)) {
		/* The owner must have blocked; the helper itself must remain task-owned. */
		ctx->helper_status = ret ? ret : -EUCLEAN;
		complete(&ctx->reply_ready);
		complete(&ctx->helper_done);
		return 0;
	}
	old = atomic_cmpxchg(&ctx->state, BP_COOP_STATE_PENDING,
			     BP_COOP_STATE_REPLYING);
	if (old != BP_COOP_STATE_PENDING) {
		ctx->helper_status = -EUCLEAN;
		complete(&ctx->reply_ready);
		complete(&ctx->helper_done);
		return 0;
	}
	WRITE_ONCE(ctx->result.coop_reply,
		   ctx->result.coop_generation ^ BP_COOP_REPLY_XOR);
	WRITE_ONCE(ctx->result.coop_state, BP_COOP_STATE_REPLIED);
	atomic_set(&ctx->state, BP_COOP_STATE_REPLIED);
	ctx->helper_status = 0;
	complete(&ctx->reply_ready);
	complete(&ctx->helper_done);
	return 0;
}

static int bp_coop_owner_thread(void *arg)
{
	struct bp_coop_context *ctx = arg;
	struct bp_host_snapshot before, after;
	unsigned long waited;
	long ret;
	int old, err;

	if (kthread_should_stop())
		return 0;
	ctx->result.coop_generation = atomic64_inc_return(&bp_coop_generation);
	if (!ctx->result.coop_generation)
		ctx->result.coop_generation = atomic64_inc_return(&bp_coop_generation);
	ctx->result.coop_state = BP_COOP_STATE_PENDING;
	atomic_set(&ctx->state, BP_COOP_STATE_PENDING);
	if (raw_smp_processor_id() != ctx->request.cpu || current != ctx->owner) {
		ctx->owner_status = -EXDEV;
		goto out;
	}
	err = bp_coop_run_shim(ctx, BP_COOP_MODE_STOP, -EINPROGRESS);
	if (err ||
	    ctx->result.coop_token !=
		(ctx->result.coop_generation ^ BP_COOP_TOKEN_XOR) ||
	    ctx->result.alt_entry_rsp < ctx->stack_base + BP_STACK_GUARD_BYTES ||
	    ctx->result.alt_entry_rsp >= ctx->stack_top) {
		ctx->owner_status = err ? err : -EUCLEAN;
		goto out;
	}
	err = bp_task_snapshot(ctx, current, &before);
	if (err) {
		ctx->owner_status = err;
		goto out;
	}
	/* The helper must observe a real voluntary switch after this point. */
	ctx->owner_nvcsw_before = READ_ONCE(current->nvcsw);
	complete(&ctx->request_ready);
	waited = wait_for_completion_timeout(&ctx->reply_ready,
					    msecs_to_jiffies(ctx->timeout_case ?
						BP_COOP_CANCEL_TIMEOUT_MS :
						BP_COOP_REPLY_TIMEOUT_MS));
	if (ctx->timeout_case) {
		if (waited) {
			ctx->owner_status = -EUCLEAN;
			goto out;
		}
		old = atomic_cmpxchg(&ctx->state, BP_COOP_STATE_PENDING,
				     BP_COOP_STATE_CANCELLED);
		if (old != BP_COOP_STATE_PENDING) {
			ctx->owner_status = -EUCLEAN;
			goto out;
		}
		WRITE_ONCE(ctx->result.coop_state, BP_COOP_STATE_CANCELLED);
		complete(&ctx->owner_cancelled);
		ret = 0;
	} else {
		if (!waited || ctx->helper_status) {
			ctx->owner_status = ctx->helper_status ? ctx->helper_status : -ETIMEDOUT;
			goto out;
		}
		if (atomic_read(&ctx->state) != BP_COOP_STATE_REPLIED ||
		    READ_ONCE(ctx->result.coop_state) != BP_COOP_STATE_REPLIED ||
		    READ_ONCE(ctx->result.coop_reply) !=
			(ctx->result.coop_generation ^ BP_COOP_REPLY_XOR)) {
			ctx->owner_status = -EUCLEAN;
			goto out;
		}
		ret = 0;
	}
	err = bp_task_snapshot(ctx, current, &after);
	ctx->owner_nvcsw_after = READ_ONCE(current->nvcsw);
	if (err || !bp_task_identity_match(&before, &after) ||
	    ctx->owner_nvcsw_after <= ctx->owner_nvcsw_before) {
		ctx->owner_status = err ? err : -EUCLEAN;
		goto out;
	}
	if (ctx->timeout_case) {
		err = bp_coop_run_shim(ctx, BP_COOP_MODE_RESUME, -ESTALE);
		if (err ||
		    ctx->result.coop_resume_count != 0) {
			ctx->owner_status = -EUCLEAN;
			goto out;
		}
	} else {
		err = bp_coop_run_shim(ctx, BP_COOP_MODE_RESUME, 0);
		if (err ||
		    ctx->result.coop_resume_count != 1) {
			ctx->owner_status = err ? err : -EUCLEAN;
			goto out;
		}
		old = atomic_cmpxchg(&ctx->state, BP_COOP_STATE_REPLIED,
				     BP_COOP_STATE_RESUMED);
		if (old != BP_COOP_STATE_REPLIED) {
			ctx->owner_status = -EUCLEAN;
			goto out;
		}
		WRITE_ONCE(ctx->result.coop_state, BP_COOP_STATE_RESUMED);
		err = bp_coop_run_shim(ctx, BP_COOP_MODE_RESUME, -EALREADY);
		if (err ||
		    ctx->result.coop_resume_count != 1 ||
		    atomic_read(&ctx->state) != BP_COOP_STATE_RESUMED ||
		    READ_ONCE(ctx->result.coop_state) != BP_COOP_STATE_RESUMED) {
			ctx->owner_status = err ? err : -EUCLEAN;
			goto out;
		}
	}
	ctx->owner_status = ret;
out:
	complete(&ctx->owner_done);
	return 0;
}

static long bp_run_cooperative(struct x86_boundary_probe_request *request)
{
	struct bp_coop_context ctx = {
		.request = *request,
		.timeout_case = request->mode == X86_BOUNDARY_PROBE_COOP_TIMEOUT,
	};
	struct task_struct *owner = NULL, *helper = NULL;
	u64 *low_canary, *high_canary;
	unsigned long waited;
	long ret = 0;

	if (fki_caps() != 0)
		return -EOPNOTSUPP;
	if (request->cpu >= nr_cpu_ids || !cpu_online(request->cpu))
		return -EINVAL;
	init_completion(&ctx.request_ready);
	init_completion(&ctx.reply_ready);
	init_completion(&ctx.owner_cancelled);
	init_completion(&ctx.owner_done);
	init_completion(&ctx.helper_done);
	atomic_set(&ctx.state, 0);
	atomic_set(&ctx.late_reply_rejected, 0);
	ctx.stack_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, BP_STACK_ORDER);
	if (!ctx.stack_page)
		return -ENOMEM;
	ctx.stack_base = (unsigned long)page_address(ctx.stack_page);
	ctx.stack_top = ctx.stack_base + BP_STACK_SIZE - BP_STACK_GUARD_BYTES;
	low_canary = (u64 *)ctx.stack_base;
	high_canary = (u64 *)ctx.stack_top;
	WRITE_ONCE(*low_canary, BP_STACK_LOW_CANARY);
	WRITE_ONCE(*high_canary, BP_STACK_HIGH_CANARY);

	cpus_read_lock();
	if (!cpu_online(request->cpu)) {
		ret = -EINVAL;
		goto out_unlock;
	}
	owner = kthread_create(bp_coop_owner_thread, &ctx, "bp-coop-owner/%u",
			       request->cpu);
	if (IS_ERR(owner)) {
		ret = PTR_ERR(owner);
		owner = NULL;
		goto out_unlock;
	}
	helper = kthread_create(bp_coop_helper_thread, &ctx, "bp-coop-helper/%u",
				request->cpu);
	if (IS_ERR(helper)) {
		ret = PTR_ERR(helper);
		helper = NULL;
		goto out_stop;
	}
	get_task_struct(owner);
	get_task_struct(helper);
	ctx.owner = owner;
	ctx.helper = helper;
	kthread_bind(owner, request->cpu);
	kthread_bind(helper, request->cpu);
	wake_up_process(helper);
	wake_up_process(owner);
	waited = wait_for_completion_timeout(&ctx.owner_done,
					    msecs_to_jiffies(BP_COOP_TEARDOWN_TIMEOUT_MS));
	if (!waited) {
		ret = -ETIMEDOUT;
		complete_all(&ctx.request_ready);
		complete_all(&ctx.reply_ready);
		complete_all(&ctx.owner_cancelled);
	}
	waited = wait_for_completion_timeout(&ctx.helper_done,
					    msecs_to_jiffies(BP_COOP_TEARDOWN_TIMEOUT_MS));
	if (!waited) {
		if (!ret)
			ret = -ETIMEDOUT;
		complete_all(&ctx.request_ready);
		complete_all(&ctx.owner_cancelled);
	}
	if (owner)
		kthread_stop(owner);
	if (helper)
		kthread_stop(helper);
	put_task_struct(helper);
	put_task_struct(owner);
	if (!ret && ctx.owner_status)
		ret = ctx.owner_status;
	if (!ret && ctx.helper_status)
		ret = ctx.helper_status;
	if (!ret && ctx.timeout_case &&
	    (!atomic_read(&ctx.late_reply_rejected) ||
	     READ_ONCE(ctx.late_reply_value) !=
		(ctx.result.coop_generation ^ BP_COOP_REPLY_XOR) ||
	     READ_ONCE(ctx.result.coop_reply) != 0 ||
	     atomic_read(&ctx.state) != BP_COOP_STATE_CANCELLED ||
	     READ_ONCE(ctx.result.coop_state) != BP_COOP_STATE_CANCELLED ||
	     ctx.result.coop_resume_count != 0))
		ret = -EUCLEAN;
	if (!ret && (READ_ONCE(*low_canary) != BP_STACK_LOW_CANARY ||
		     READ_ONCE(*high_canary) != BP_STACK_HIGH_CANARY))
		ret = -EUCLEAN;
	if (!ret)
		pr_info("x86-boundary-probe: cooperative cpu %u %s passed; same_cpu=1 task_stack=1 owner_nvcsw=%lu helper_nvcsw=%lu resume_count=%lu late_reply_rejected=%u\n",
			request->cpu, ctx.timeout_case ? "timeout" : "reply",
			ctx.owner_nvcsw_after - ctx.owner_nvcsw_before,
			ctx.helper_nvcsw_after - ctx.helper_nvcsw_before,
			ctx.result.coop_resume_count,
			atomic_read(&ctx.late_reply_rejected));
	goto out_unlock;

out_stop:
	complete_all(&ctx.request_ready);
	complete_all(&ctx.reply_ready);
	complete_all(&ctx.owner_cancelled);
	if (owner)
		kthread_stop(owner);
	if (helper)
		kthread_stop(helper);
out_unlock:
	cpus_read_unlock();
	__free_pages(ctx.stack_page, BP_STACK_ORDER);
	return ret;
}

static long bp_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
	struct x86_boundary_probe_request request;
	struct x86_boundary_continuation_request continuation_request;
	struct bp_work work;
	long ret;

	if (command == X86_BOUNDARY_PROBE_TASK_CONTINUATION) {
		if (copy_from_user(&continuation_request, (void __user *)arg,
				   sizeof(continuation_request)))
			return -EFAULT;
		return bp_task_continuation_probe_run(continuation_request.owner_cpu,
						      continuation_request.responder_cpu);
	}
	if (command != X86_BOUNDARY_PROBE_RUN &&
	    command != X86_BOUNDARY_PROBE_COOPERATIVE)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (command == X86_BOUNDARY_PROBE_COOPERATIVE) {
		if (request.mode != X86_BOUNDARY_PROBE_COOP_REPLY &&
		    request.mode != X86_BOUNDARY_PROBE_COOP_TIMEOUT)
			return -EINVAL;
		return bp_run_cooperative(&request);
	}
	if (request.mode != X86_BOUNDARY_PROBE_MODE_RETURN &&
	    request.mode != X86_BOUNDARY_PROBE_MODE_ABORT)
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
