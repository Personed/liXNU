// SPDX-License-Identifier: GPL-2.0-only
/* Disposable-VM-only model of a Linux-owned continuation slot. */
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/irqflags.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/processor.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/wait.h>

#include "task_continuation_probe.h"

#define BP_CONT_STACK_ORDER 2
#define BP_CONT_STACK_SIZE (PAGE_SIZE << BP_CONT_STACK_ORDER)
#define BP_CONT_LOW_CANARY  0x434f4e544c4f5743ULL
#define BP_CONT_HIGH_CANARY 0x434f4e5448494748ULL
#define BP_CONT_REPLY_SEED  0x9c638a57d4e21b0fULL
#define BP_CONT_WATCHDOG_MS 5000
#define BP_CONT_TIMEOUT_MS 150

enum bp_cont_state {
	BP_CONT_FREE,
	BP_CONT_RUNNING,
	BP_CONT_WAITING,
	BP_CONT_RESUME_READY,
	BP_CONT_COMPLETE,
	BP_CONT_CANCELLED,
};

enum bp_cont_phase {
	BP_CONT_FIRST_ENTRY,
	BP_CONT_RESUME_ENTRY,
};

enum bp_cont_case {
	BP_CONT_IMMEDIATE_REPLY,
	BP_CONT_DELAYED_REPLY,
	BP_CONT_TIMEOUT_REPLY,
	BP_CONT_CANCEL_REPLY,
};

struct bp_continuation_test;

struct bp_continuation_slot {
	spinlock_t lock;
	bool lock_initialized;
	enum bp_cont_state state;
	struct task_struct *owner;
	struct bp_continuation_test *request;
	u64 generation;
	u64 request_id;
	struct page *stack_pages;
	unsigned long stack_base;
	unsigned long stack_top;
};

struct bp_continuation_test {
	struct completion request_published;
	struct completion owner_pre_wait_check;
	struct completion reply_checked;
	struct completion reply_ready;
	struct completion contender_checked;
	struct completion cancel_ready;
	struct completion cancel_acknowledged;
	struct completion owner_done;
	struct completion responder_done;
	wait_queue_head_t release_wait;
	struct bp_continuation_slot *slot;
	struct task_struct *owner_task;
	struct task_struct *responder_task;
	enum bp_cont_case test_case;
	enum bp_cont_phase phase;
	unsigned int owner_cpu;
	unsigned int responder_cpu;
	u64 generation;
	u64 request_id;
	u64 request_payload;
	u64 expected_reply;
	u64 reply_payload;
	unsigned long callback_rsp[2];
	struct task_struct *callback_task[2];
	unsigned long owner_rsp_first;
	unsigned long owner_rsp_resume;
	unsigned long responder_rsp;
	unsigned long owner_nvcsw_before;
	unsigned long owner_nvcsw_after;
	unsigned int callback_entries;
	unsigned int continuation_resumes;
	int owner_status;
	int responder_status;
	int second_entry_status;
	int wrong_generation_status;
	int wrong_request_id_status;
	int accepted_reply_status;
	int duplicate_reply_status;
	int late_reply_status;
	bool owner_wait_started;
	bool owner_first_wait_attempted;
	bool reply_before_first_wait;
	bool timeout_observed;
	bool cancellation_observed;
	bool same_cpu_contender;
	bool reply_buffer_released_after_ack;
	int callback_error;
};

static DEFINE_PER_CPU(struct bp_continuation_slot, bp_continuation_slots);
static DEFINE_MUTEX(bp_continuation_run_lock);
static atomic64_t bp_continuation_request_ids = ATOMIC64_INIT(0);

static unsigned int bp_task_continuation_current_cpu(void)
{
	unsigned int cpu = get_cpu();

	put_cpu();
	return cpu;
}

static noinline unsigned long bp_task_continuation_read_rsp(void)
{
	unsigned long rsp;

	asm volatile("mov %%rsp, %0" : "=r"(rsp));
	return rsp;
}

static bool bp_task_continuation_rsp_in_task(struct task_struct *task,
					    unsigned long rsp)
{
	unsigned long base = (unsigned long)task_stack_page(task);

	return rsp >= base && rsp - base < THREAD_SIZE;
}

static bool bp_task_continuation_rsp_in_slot(struct bp_continuation_slot *slot,
					    unsigned long rsp)
{
	return rsp >= slot->stack_base + sizeof(u64) && rsp < slot->stack_top;
}

/* Called only on the reserved test stack. It never schedules or takes a lock. */
noinline notrace void bp_task_continuation_body(struct bp_continuation_test *test)
{
	unsigned long rsp = bp_task_continuation_read_rsp();
	unsigned int index = READ_ONCE(test->callback_entries);

	if (index >= ARRAY_SIZE(test->callback_rsp)) {
		WRITE_ONCE(test->callback_error, -EOVERFLOW);
		return;
	}
	WRITE_ONCE(test->callback_entries, index + 1);
	WRITE_ONCE(test->callback_rsp[index], rsp);
	WRITE_ONCE(test->callback_task[index], current);
	if (current != READ_ONCE(test->owner_task) ||
	    !bp_task_continuation_rsp_in_slot(test->slot, rsp) ||
	    !irqs_disabled() ||
	    (IS_ENABLED(CONFIG_PREEMPT_COUNT) && !preempt_count()))
		WRITE_ONCE(test->callback_error, -EUCLEAN);

	if (READ_ONCE(test->phase) == BP_CONT_RESUME_ENTRY) {
		WRITE_ONCE(test->continuation_resumes,
			   READ_ONCE(test->continuation_resumes) + 1);
		if (READ_ONCE(test->generation) == 0 ||
		    READ_ONCE(test->request_id) == 0 ||
		    READ_ONCE(test->reply_payload) != READ_ONCE(test->expected_reply))
			WRITE_ONCE(test->callback_error, -EPROTO);
	}
}

static void bp_task_continuation_slot_initialize(struct bp_continuation_slot *slot)
{
	if (slot->lock_initialized)
		return;
	spin_lock_init(&slot->lock);
	slot->state = BP_CONT_FREE;
	slot->owner = NULL;
	slot->request = NULL;
	slot->generation = 0;
	slot->request_id = 0;
	slot->lock_initialized = true;
}

static int bp_task_continuation_stack_allocate(struct bp_continuation_slot *slot)
{
	u64 *low_canary;
	u64 *high_canary;

	slot->stack_pages = alloc_pages(GFP_KERNEL | __GFP_ZERO,
				       BP_CONT_STACK_ORDER);
	if (!slot->stack_pages)
		return -ENOMEM;
	slot->stack_base = (unsigned long)page_address(slot->stack_pages);
	slot->stack_top = (slot->stack_base + BP_CONT_STACK_SIZE - sizeof(u64)) &
		  ~0xfUL;
	low_canary = (u64 *)slot->stack_base;
	high_canary = (u64 *)(slot->stack_base + BP_CONT_STACK_SIZE - sizeof(u64));
	WRITE_ONCE(*low_canary, BP_CONT_LOW_CANARY);
	WRITE_ONCE(*high_canary, BP_CONT_HIGH_CANARY);
	return 0;
}

static bool bp_task_continuation_canaries_ok(struct bp_continuation_slot *slot)
{
	const u64 *low_canary = (const u64 *)slot->stack_base;
	const u64 *high_canary = (const u64 *)(slot->stack_base +
					       BP_CONT_STACK_SIZE - sizeof(u64));

	return READ_ONCE(*low_canary) == BP_CONT_LOW_CANARY &&
	       READ_ONCE(*high_canary) == BP_CONT_HIGH_CANARY;
}

static int bp_task_continuation_claim_initial(struct bp_continuation_test *test)
{
	struct bp_continuation_slot *slot = test->slot;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&slot->lock, flags);
	if (slot->state != BP_CONT_FREE) {
		ret = -EBUSY;
	} else {
		slot->generation++;
		if (!slot->generation)
			slot->generation++;
		slot->owner = current;
		slot->request = test;
		slot->request_id = test->request_id;
		slot->state = BP_CONT_RUNNING;
		test->generation = slot->generation;
		test->expected_reply = BP_CONT_REPLY_SEED ^ test->generation ^
				       test->request_id ^ test->request_payload;
	}
	spin_unlock_irqrestore(&slot->lock, flags);
	return ret;
}

static int bp_task_continuation_claim_resume(struct bp_continuation_test *test)
{
	struct bp_continuation_slot *slot = test->slot;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&slot->lock, flags);
	if (slot->state != BP_CONT_RESUME_READY || slot->owner != current ||
	    slot->request != test || slot->generation != test->generation ||
	    slot->request_id != test->request_id ||
	    test->reply_payload != test->expected_reply) {
		ret = -ESTALE;
	} else {
		slot->state = BP_CONT_RUNNING;
	}
	spin_unlock_irqrestore(&slot->lock, flags);
	return ret;
}

static int bp_task_continuation_enter(struct bp_continuation_test *test,
				      enum bp_cont_phase phase)
{
	unsigned int preempt_before = preempt_count();
	unsigned long irq_flags;
	bool irqs_disabled_before = irqs_disabled();
	unsigned long rsp;
	int ret;

	ret = phase == BP_CONT_FIRST_ENTRY ?
		bp_task_continuation_claim_initial(test) :
		bp_task_continuation_claim_resume(test);
	if (ret)
		return ret;
	if (bp_task_continuation_current_cpu() != test->owner_cpu)
		return -EXDEV;

	WRITE_ONCE(test->phase, phase);
	/* Keep maskable interrupts and preemption off on the alternate stack. */
	preempt_disable();
	local_irq_save(irq_flags);
	bp_task_continuation_switch_stack(test->slot->stack_top, test);
	local_irq_restore(irq_flags);
	preempt_enable();
	if (preempt_count() != preempt_before ||
	    irqs_disabled() != irqs_disabled_before)
		return -EUCLEAN;
	if (current != test->owner_task)
		return -EUCLEAN;
	rsp = bp_task_continuation_read_rsp();
	if (!bp_task_continuation_rsp_in_task(current, rsp))
		return -EUCLEAN;
	if (phase == BP_CONT_FIRST_ENTRY)
		WRITE_ONCE(test->owner_rsp_first, rsp);
	else
		WRITE_ONCE(test->owner_rsp_resume, rsp);
	return READ_ONCE(test->callback_error);
}

static int bp_task_continuation_publish_wait(struct bp_continuation_test *test)
{
	struct bp_continuation_slot *slot = test->slot;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&slot->lock, flags);
	if (slot->state != BP_CONT_RUNNING || slot->owner != current ||
	    slot->request != test || slot->generation != test->generation ||
	    slot->request_id != test->request_id) {
		ret = -EUCLEAN;
	} else {
		/* The request becomes visible only after WAITING is published. */
		slot->state = BP_CONT_WAITING;
	}
	spin_unlock_irqrestore(&slot->lock, flags);
	if (!ret)
		complete(&test->request_published);
	return ret;
}

static int bp_task_continuation_reply(struct bp_continuation_test *test,
				      u64 generation, u64 request_id, u64 payload)
{
	struct bp_continuation_slot *slot = test->slot;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&slot->lock, flags);
	if (slot->state == BP_CONT_RESUME_READY) {
		ret = -EALREADY;
	} else if (slot->state != BP_CONT_WAITING ||
		   slot->generation != generation ||
		   slot->request_id != request_id || slot->request != test) {
		ret = -ESTALE;
	} else {
		/* Store the complete payload before publishing RESUME_READY. */
		WRITE_ONCE(test->reply_payload, payload);
		WRITE_ONCE(test->reply_before_first_wait,
			   !READ_ONCE(test->owner_first_wait_attempted));
		smp_wmb();
		slot->state = BP_CONT_RESUME_READY;
	}
	spin_unlock_irqrestore(&slot->lock, flags);
	if (!ret)
		complete(&test->reply_ready);
	return ret;
}

static int bp_task_continuation_cancel_pending(struct bp_continuation_test *test)
{
	struct bp_continuation_slot *slot = test->slot;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&slot->lock, flags);
	if (slot->state != BP_CONT_WAITING || slot->owner != test->owner_task ||
	    slot->request != test || slot->generation != test->generation ||
	    slot->request_id != test->request_id) {
		ret = -ESTALE;
	} else {
		/* Cancellation and reply acceptance share the same serialization lock. */
		slot->state = BP_CONT_CANCELLED;
	}
	spin_unlock_irqrestore(&slot->lock, flags);
	if (!ret)
		complete(&test->cancel_ready);
	return ret;
}

static int bp_task_continuation_set_terminal(struct bp_continuation_test *test,
					     enum bp_cont_state terminal)
{
	struct bp_continuation_slot *slot = test->slot;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&slot->lock, flags);
	if (slot->owner != current || slot->request != test ||
	    slot->generation != test->generation ||
	    (slot->state != BP_CONT_RUNNING && slot->state != BP_CONT_WAITING)) {
		ret = -EUCLEAN;
	} else {
		slot->state = terminal;
		slot->owner = NULL;
	}
	spin_unlock_irqrestore(&slot->lock, flags);
	return ret;
}

static bool bp_task_continuation_owner_blocked(struct bp_continuation_test *test)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(BP_CONT_WATCHDOG_MS);

	for (;;) {
		if (READ_ONCE(test->owner_wait_started) &&
		    READ_ONCE(test->owner_task->nvcsw) >
			    READ_ONCE(test->owner_nvcsw_before))
			return true;
		if (kthread_should_stop() || time_after_eq(jiffies, deadline))
			return false;
		/* Let the same-CPU owner reach and enter its actual wait. */
		schedule_timeout_uninterruptible(1);
	}
}

static int bp_task_continuation_owner_thread(void *data)
{
	struct bp_continuation_test *test = data;
	unsigned long deadline;
	unsigned long waited;
	int ret;

	WRITE_ONCE(test->owner_task, current);
	if (bp_task_continuation_current_cpu() != test->owner_cpu) {
		ret = -EXDEV;
		goto out;
	}
	test->owner_nvcsw_before = current->nvcsw;
	ret = bp_task_continuation_enter(test, BP_CONT_FIRST_ENTRY);
	if (ret)
		goto out;
	if (READ_ONCE(test->callback_entries) != 1 ||
	    READ_ONCE(test->callback_task[0]) != current) {
		ret = -EUCLEAN;
		goto out;
	}
	ret = bp_task_continuation_publish_wait(test);
	if (ret)
		goto out;

	if (test->test_case == BP_CONT_IMMEDIATE_REPLY) {
		/* Force the reply into the check-to-wait window without sleeping. */
		if (completion_done(&test->reply_ready)) {
			ret = -EUCLEAN;
			goto out;
		}
		complete(&test->owner_pre_wait_check);
		deadline = jiffies + msecs_to_jiffies(BP_CONT_WATCHDOG_MS);
		while (!completion_done(&test->reply_checked) &&
		       time_before(jiffies, deadline))
			cpu_relax();
		if (!completion_done(&test->reply_checked)) {
			ret = -ETIMEDOUT;
			goto out;
		}
		if (!completion_done(&test->reply_ready) ||
		    !READ_ONCE(test->reply_before_first_wait) ||
		    READ_ONCE(test->owner_first_wait_attempted)) {
			ret = -EUCLEAN;
			goto out;
		}
		WRITE_ONCE(test->owner_first_wait_attempted, true);
		waited = wait_for_completion_timeout(&test->reply_ready,
						     msecs_to_jiffies(BP_CONT_WATCHDOG_MS));
		if (!waited) {
			ret = -ETIMEDOUT;
			goto out;
		}
		if (current->nvcsw != test->owner_nvcsw_before) {
			ret = -EUCLEAN;
			goto out;
		}
		test->owner_nvcsw_after = current->nvcsw;
	} else {
		WRITE_ONCE(test->owner_wait_started, true);
		WRITE_ONCE(test->owner_first_wait_attempted, true);
		if (test->test_case == BP_CONT_TIMEOUT_REPLY) {
			/* Time the reply only after the contention check. */
			if (!wait_for_completion_timeout(&test->contender_checked,
							 msecs_to_jiffies(BP_CONT_WATCHDOG_MS))) {
				ret = -ETIMEDOUT;
				goto out;
			}
			waited = wait_for_completion_timeout(&test->reply_ready,
							 msecs_to_jiffies(BP_CONT_TIMEOUT_MS));
			if (waited) {
				ret = -EUCLEAN;
				goto out;
			}
			test->owner_nvcsw_after = current->nvcsw;
			if (test->owner_nvcsw_after <= test->owner_nvcsw_before) {
				ret = -EUCLEAN;
				goto out;
			}
			ret = bp_task_continuation_set_terminal(test, BP_CONT_CANCELLED);
			if (ret)
				goto out;
			WRITE_ONCE(test->timeout_observed, true);
			ret = 0;
			goto out;
		}
		if (test->test_case == BP_CONT_CANCEL_REPLY) {
			unsigned long flags;
			bool cancelled;

			waited = wait_for_completion_timeout(&test->cancel_ready,
							     msecs_to_jiffies(BP_CONT_WATCHDOG_MS));
			if (!waited) {
				ret = -ETIMEDOUT;
				goto out;
			}
			spin_lock_irqsave(&test->slot->lock, flags);
			cancelled = test->slot->state == BP_CONT_CANCELLED &&
				test->slot->owner == current &&
				test->slot->request == test &&
				test->slot->generation == test->generation &&
				test->slot->request_id == test->request_id;
			if (cancelled)
				test->slot->owner = NULL;
			spin_unlock_irqrestore(&test->slot->lock, flags);
			if (!cancelled) {
				ret = -EUCLEAN;
				goto out;
			}
			test->owner_nvcsw_after = current->nvcsw;
			if (test->owner_nvcsw_after <= test->owner_nvcsw_before) {
				ret = -EUCLEAN;
				goto out;
			}
			WRITE_ONCE(test->cancellation_observed, true);
			complete(&test->cancel_acknowledged);
			ret = 0;
			goto out;
		}
		waited = wait_for_completion_timeout(&test->reply_ready,
						     msecs_to_jiffies(BP_CONT_WATCHDOG_MS));
		if (!waited) {
			ret = -ETIMEDOUT;
			goto out;
		}
		test->owner_nvcsw_after = current->nvcsw;
		if (test->owner_nvcsw_after <= test->owner_nvcsw_before) {
			ret = -EUCLEAN;
			goto out;
		}
		if (!wait_for_completion_timeout(&test->reply_checked,
						 msecs_to_jiffies(BP_CONT_WATCHDOG_MS))) {
			ret = -ETIMEDOUT;
			goto out;
		}
	}

	ret = bp_task_continuation_enter(test, BP_CONT_RESUME_ENTRY);
	if (ret)
		goto out;
	if (READ_ONCE(test->continuation_resumes) != 1 ||
	    READ_ONCE(test->callback_entries) != 2 ||
	    READ_ONCE(test->callback_task[1]) != current) {
		ret = -EUCLEAN;
		goto out;
	}
	ret = bp_task_continuation_set_terminal(test, BP_CONT_COMPLETE);

out:
	if (!completion_done(&test->request_published))
		complete(&test->request_published);
	if (ret) {
		int cancel_ret;

		complete_all(&test->reply_ready);
		complete_all(&test->cancel_ready);
		complete_all(&test->reply_checked);
		complete_all(&test->owner_pre_wait_check);
		cancel_ret = bp_task_continuation_set_terminal(test,
								   BP_CONT_CANCELLED);
		if (cancel_ret && cancel_ret != -EUCLEAN)
			ret = cancel_ret;
	}
	WRITE_ONCE(test->owner_status, ret);
	complete_all(&test->owner_done);
	wait_event(test->release_wait, kthread_should_stop());
	return ret;
}

static int bp_task_continuation_responder_thread(void *data)
{
	struct bp_continuation_test *test = data;
	u64 wrong_generation;
	u64 wrong_request_id;
	unsigned long deadline;
	int ret;

	WRITE_ONCE(test->responder_task, current);
	if (!wait_for_completion_timeout(&test->request_published,
					msecs_to_jiffies(BP_CONT_WATCHDOG_MS))) {
		ret = -ETIMEDOUT;
		goto out;
	}
	if (bp_task_continuation_current_cpu() != test->responder_cpu ||
	    !bp_task_continuation_rsp_in_task(current,
					      bp_task_continuation_read_rsp())) {
		ret = -EXDEV;
		goto out;
	}
	test->responder_rsp = bp_task_continuation_read_rsp();
	if (test->test_case != BP_CONT_IMMEDIATE_REPLY) {
		unsigned long flags;
		bool waiting;

		if (!bp_task_continuation_owner_blocked(test)) {
			ret = -ETIMEDOUT;
			goto out;
		}
		spin_lock_irqsave(&test->slot->lock, flags);
		waiting = test->slot->state == BP_CONT_WAITING &&
			test->slot->owner == test->owner_task &&
			test->slot->request == test &&
			test->slot->generation == test->generation;
		spin_unlock_irqrestore(&test->slot->lock, flags);
		if (!waiting || test->responder_task == test->owner_task) {
			ret = -EUCLEAN;
			goto out;
		}
		test->second_entry_status = bp_task_continuation_enter(
			test, BP_CONT_FIRST_ENTRY);
		if (test->second_entry_status != -EBUSY ||
		    READ_ONCE(test->callback_entries) != 1 ||
		    !bp_task_continuation_canaries_ok(test->slot)) {
			ret = -EUCLEAN;
			goto out;
		}
		WRITE_ONCE(test->same_cpu_contender,
			   bp_task_continuation_current_cpu() == test->owner_cpu);
		if (!test->same_cpu_contender ||
		    READ_ONCE(test->owner_task->nvcsw) <= test->owner_nvcsw_before) {
			ret = -EUCLEAN;
			goto out;
		}
	} else if (!wait_for_completion_timeout(&test->owner_pre_wait_check,
						msecs_to_jiffies(BP_CONT_WATCHDOG_MS)) ||
		   completion_done(&test->reply_ready)) {
		ret = -EUCLEAN;
		goto out;
	}

	if (test->test_case == BP_CONT_TIMEOUT_REPLY) {
		complete(&test->contender_checked);
		if (!wait_for_completion_timeout(&test->owner_done,
						msecs_to_jiffies(BP_CONT_WATCHDOG_MS))) {
			ret = -ETIMEDOUT;
			goto out;
		}
		test->late_reply_status = bp_task_continuation_reply(
			test, test->generation, test->request_id, test->expected_reply);
		if (test->late_reply_status != -ESTALE ||
		    READ_ONCE(test->owner_status) != 0 ||
		    !READ_ONCE(test->timeout_observed)) {
			ret = -EUCLEAN;
			goto out;
		}
		WRITE_ONCE(test->reply_buffer_released_after_ack, true);
		ret = 0;
		goto out;
	}
	if (test->test_case == BP_CONT_CANCEL_REPLY) {
		ret = bp_task_continuation_cancel_pending(test);
		if (ret)
			goto out;
		if (!wait_for_completion_timeout(&test->owner_done,
						msecs_to_jiffies(BP_CONT_WATCHDOG_MS))) {
			ret = -ETIMEDOUT;
			goto out;
		}
		test->late_reply_status = bp_task_continuation_reply(
			test, test->generation, test->request_id, test->expected_reply);
		if (test->late_reply_status != -ESTALE ||
		    READ_ONCE(test->owner_status) != 0 ||
		    !completion_done(&test->cancel_acknowledged) ||
		    !READ_ONCE(test->cancellation_observed)) {
			ret = -EUCLEAN;
			goto out;
		}
		WRITE_ONCE(test->reply_buffer_released_after_ack, true);
		ret = 0;
		goto out;
	}

	wrong_generation = test->generation + 1;
	wrong_request_id = test->request_id + 1;
	test->wrong_generation_status = bp_task_continuation_reply(
		test, wrong_generation, test->request_id, test->expected_reply);
	test->wrong_request_id_status = bp_task_continuation_reply(
		test, test->generation, wrong_request_id, test->expected_reply);
	if (test->wrong_generation_status != -ESTALE ||
	    test->wrong_request_id_status != -ESTALE) {
		ret = -EUCLEAN;
		goto out;
	}
	test->accepted_reply_status = bp_task_continuation_reply(
		test, test->generation, test->request_id, test->expected_reply);
	if (test->accepted_reply_status) {
		ret = test->accepted_reply_status;
		goto out;
	}
	test->duplicate_reply_status = bp_task_continuation_reply(
		test, test->generation, test->request_id, test->expected_reply);
	if (test->duplicate_reply_status != -EALREADY) {
		ret = -EUCLEAN;
		goto out;
	}
	if (test->test_case == BP_CONT_IMMEDIATE_REPLY &&
	    (!READ_ONCE(test->reply_before_first_wait) ||
	     READ_ONCE(test->owner_wait_started) ||
	     READ_ONCE(test->owner_first_wait_attempted))) {
		ret = -EUCLEAN;
		goto out;
	}
	/* The owner may consume RESUME_READY after the duplicate is checked. */
	complete(&test->reply_checked);
	if (!wait_for_completion_timeout(&test->owner_done,
					msecs_to_jiffies(BP_CONT_WATCHDOG_MS))) {
		ret = -ETIMEDOUT;
		goto out;
	}
	test->late_reply_status = bp_task_continuation_reply(
		test, test->generation, test->request_id, test->expected_reply);
	if (test->late_reply_status != -ESTALE ||
	    READ_ONCE(test->owner_status) != 0) {
		ret = -EUCLEAN;
		goto out;
	}
	WRITE_ONCE(test->reply_buffer_released_after_ack, true);
	ret = 0;

out:
	WRITE_ONCE(test->responder_status, ret);
	complete_all(&test->responder_done);
	wait_event(test->release_wait, kthread_should_stop());
	return ret;
}

static int bp_task_continuation_run_case(unsigned int owner_cpu,
					 unsigned int responder_cpu,
					 enum bp_cont_case test_case)
{
	struct bp_continuation_slot *slot = per_cpu_ptr(&bp_continuation_slots,
								 owner_cpu);
	struct bp_continuation_test *test;
	struct task_struct *owner;
	struct task_struct *responder;
	unsigned long flags;
	unsigned long timeout = msecs_to_jiffies(BP_CONT_WATCHDOG_MS * 3);
	int ret = 0;

	test = kzalloc(sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	init_completion(&test->request_published);
	init_completion(&test->owner_pre_wait_check);
	init_completion(&test->reply_checked);
	init_completion(&test->reply_ready);
	init_completion(&test->contender_checked);
	init_completion(&test->cancel_ready);
	init_completion(&test->cancel_acknowledged);
	init_completion(&test->owner_done);
	init_completion(&test->responder_done);
	init_waitqueue_head(&test->release_wait);
	test->slot = slot;
	test->test_case = test_case;
	test->owner_cpu = owner_cpu;
	test->responder_cpu = responder_cpu;
	test->request_id = atomic64_inc_return(&bp_continuation_request_ids);
	if (!test->request_id)
		test->request_id = atomic64_inc_return(&bp_continuation_request_ids);
	test->request_payload = 0x7b8d5ea1c342906fULL ^ test->request_id;
	test->second_entry_status = -EINPROGRESS;
	test->wrong_generation_status = -EINPROGRESS;
	test->wrong_request_id_status = -EINPROGRESS;
	test->accepted_reply_status = -EINPROGRESS;
	test->duplicate_reply_status = -EINPROGRESS;
	test->late_reply_status = -EINPROGRESS;
	test->owner_status = -EINPROGRESS;
	test->responder_status = -EINPROGRESS;

	spin_lock_irqsave(&slot->lock, flags);
	if (slot->state != BP_CONT_FREE || slot->stack_pages) {
		spin_unlock_irqrestore(&slot->lock, flags);
		ret = -EBUSY;
		goto out_free_test;
	}
	spin_unlock_irqrestore(&slot->lock, flags);
	ret = bp_task_continuation_stack_allocate(slot);
	if (ret)
		goto out_free_test;

	responder = kthread_create(bp_task_continuation_responder_thread, test,
				   "x86-cont-responder/%u", responder_cpu);
	if (IS_ERR(responder)) {
		ret = PTR_ERR(responder);
		goto out_free_stack;
	}
	owner = kthread_create(bp_task_continuation_owner_thread, test,
			       "x86-cont-owner/%u", owner_cpu);
	if (IS_ERR(owner)) {
		ret = PTR_ERR(owner);
		WRITE_ONCE(test->owner_status, ret);
		complete(&test->request_published);
		complete_all(&test->owner_pre_wait_check);
		complete_all(&test->reply_ready);
		kthread_stop(responder);
		goto out_free_stack;
	}
	WRITE_ONCE(test->owner_task, owner);
	WRITE_ONCE(test->responder_task, responder);
	kthread_bind(responder, responder_cpu);
	kthread_bind(owner, owner_cpu);
	wake_up_process(responder);
	wake_up_process(owner);

	if (!wait_for_completion_timeout(&test->owner_done, timeout)) {
		ret = -ETIMEDOUT;
		complete_all(&test->reply_ready);
		complete_all(&test->cancel_ready);
		complete_all(&test->reply_checked);
		complete_all(&test->owner_pre_wait_check);
		complete_all(&test->request_published);
	} else {
		ret = READ_ONCE(test->owner_status);
	}
	if (!wait_for_completion_timeout(&test->responder_done, timeout)) {
		if (!ret)
			ret = -ETIMEDOUT;
		complete_all(&test->reply_ready);
		complete_all(&test->cancel_ready);
		complete_all(&test->reply_checked);
		complete_all(&test->owner_pre_wait_check);
		complete_all(&test->request_published);
	} else if (!ret) {
		ret = READ_ONCE(test->responder_status);
	}
	if (ret)
		goto out_stop_threads;

	if (!bp_task_continuation_canaries_ok(slot) ||
	    !bp_task_continuation_rsp_in_task(test->owner_task,
					      test->owner_rsp_first) ||
	    !bp_task_continuation_rsp_in_task(test->responder_task,
					      test->responder_rsp) ||
	    !bp_task_continuation_rsp_in_slot(slot, test->callback_rsp[0]) ||
	    test->callback_task[0] != test->owner_task ||
	    test->responder_task == test->owner_task ||
	    test->request_payload != (0x7b8d5ea1c342906fULL ^ test->request_id) ||
	    test->expected_reply != (BP_CONT_REPLY_SEED ^ test->generation ^
				      test->request_id ^ test->request_payload) ||
	    test->slot->generation != test->generation ||
	    test->slot->request != test ||
	    test->reply_buffer_released_after_ack != true) {
		ret = -EUCLEAN;
		goto out_stop_threads;
	}
	if (test_case == BP_CONT_IMMEDIATE_REPLY) {
		if (!test->reply_before_first_wait || test->owner_wait_started ||
		    !test->owner_first_wait_attempted ||
		    test->owner_nvcsw_after != test->owner_nvcsw_before ||
		    test->wrong_generation_status != -ESTALE ||
		    test->wrong_request_id_status != -ESTALE ||
		    test->accepted_reply_status != 0 ||
		    test->duplicate_reply_status != -EALREADY ||
		    test->late_reply_status != -ESTALE || test->callback_entries != 2 ||
		    test->continuation_resumes != 1 ||
		    !bp_task_continuation_rsp_in_task(test->owner_task,
						      test->owner_rsp_resume) ||
		    !bp_task_continuation_rsp_in_slot(slot, test->callback_rsp[1]) ||
		    test->callback_task[1] != test->owner_task) {
			ret = -EUCLEAN;
			goto out_stop_threads;
		}
	} else if (test_case == BP_CONT_DELAYED_REPLY) {
		if (!test->same_cpu_contender || test->reply_before_first_wait ||
		    test->second_entry_status != -EBUSY ||
		    test->wrong_generation_status != -ESTALE ||
		    test->wrong_request_id_status != -ESTALE ||
		    test->accepted_reply_status ||
		    test->duplicate_reply_status != -EALREADY ||
		    test->late_reply_status != -ESTALE || test->callback_entries != 2 ||
		    test->continuation_resumes != 1 ||
		    test->owner_nvcsw_after <= test->owner_nvcsw_before ||
		    !bp_task_continuation_rsp_in_task(test->owner_task,
						      test->owner_rsp_resume) ||
		    !bp_task_continuation_rsp_in_slot(slot, test->callback_rsp[1])) {
			ret = -EUCLEAN;
			goto out_stop_threads;
		}
	} else if (test_case == BP_CONT_TIMEOUT_REPLY) {
		if (!test->timeout_observed || !test->same_cpu_contender ||
		    test->second_entry_status != -EBUSY ||
		    test->late_reply_status != -ESTALE || test->callback_entries != 1 ||
		    test->continuation_resumes != 0 ||
		    test->owner_nvcsw_after <= test->owner_nvcsw_before) {
			ret = -EUCLEAN;
			goto out_stop_threads;
		}
	} else if (!test->cancellation_observed || !test->same_cpu_contender ||
		   test->second_entry_status != -EBUSY ||
		   test->late_reply_status != -ESTALE || test->callback_entries != 1 ||
		   test->continuation_resumes != 0 || test->timeout_observed ||
		   test->owner_nvcsw_after <= test->owner_nvcsw_before) {
		ret = -EUCLEAN;
		goto out_stop_threads;
	}

out_stop_threads:
	if (ret)
		pr_err("x86-boundary-probe: task-continuation case %u failed ret=%d owner=%d responder=%d entries=%u resumes=%u\n",
		       test_case, ret, test->owner_status, test->responder_status,
		       test->callback_entries, test->continuation_resumes);
	kthread_stop(owner);
	kthread_stop(responder);
	spin_lock_irqsave(&slot->lock, flags);
	if (slot->state != BP_CONT_COMPLETE && slot->state != BP_CONT_CANCELLED &&
	    !ret)
		ret = -EUCLEAN;
	slot->state = BP_CONT_FREE;
	slot->owner = NULL;
	slot->request = NULL;
	slot->request_id = 0;
	spin_unlock_irqrestore(&slot->lock, flags);

out_free_stack:
	if (slot->stack_pages) {
		__free_pages(slot->stack_pages, BP_CONT_STACK_ORDER);
		slot->stack_pages = NULL;
		slot->stack_base = 0;
		slot->stack_top = 0;
	}
out_free_test:
	kfree(test);
	return ret;
}

int bp_task_continuation_probe_run(unsigned int owner_cpu,
				   unsigned int immediate_responder_cpu)
{
	int ret;

	if (owner_cpu == immediate_responder_cpu ||
	    owner_cpu >= nr_cpu_ids || immediate_responder_cpu >= nr_cpu_ids)
		return -EINVAL;

	cpus_read_lock();
	if (!cpu_online(owner_cpu) || !cpu_online(immediate_responder_cpu) ||
	    num_online_cpus() < 2) {
		cpus_read_unlock();
		return -EINVAL;
	}
	mutex_lock(&bp_continuation_run_lock);
	{
		struct bp_continuation_slot *slot = per_cpu_ptr(
			&bp_continuation_slots, owner_cpu);

		bp_task_continuation_slot_initialize(slot);
	}
	ret = bp_task_continuation_run_case(owner_cpu, immediate_responder_cpu,
					   BP_CONT_IMMEDIATE_REPLY);
	if (!ret)
		ret = bp_task_continuation_run_case(owner_cpu, owner_cpu,
						   BP_CONT_DELAYED_REPLY);
	if (!ret)
		ret = bp_task_continuation_run_case(owner_cpu, owner_cpu,
						   BP_CONT_TIMEOUT_REPLY);
	if (!ret)
		ret = bp_task_continuation_run_case(owner_cpu, owner_cpu,
						   BP_CONT_CANCEL_REPLY);
	mutex_unlock(&bp_continuation_run_lock);
	cpus_read_unlock();
	if (!ret)
		pr_info("x86-boundary-probe: task-stack continuation immediate/delayed/timeout/cancel passed; same-CPU contention rejected; reply generation and request IDs checked\n");
	return ret;
}
