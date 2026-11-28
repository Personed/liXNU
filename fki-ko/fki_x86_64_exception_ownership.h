/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FKI_X86_64_EXCEPTION_OWNERSHIP_H
#include <linux/string.h>
#define FKI_X86_64_EXCEPTION_OWNERSHIP_H

#include <linux/errno.h>
#include <stdbool.h>
#include <linux/types.h>

/*
 * Validation-only synchronous-exception ownership model. It is a design oracle
 * for a future production boundary, not an IDT, XNU, runtime, or ownership
 * implementation. CPU/task/IDTR/stack cookies are abstract equality tokens.
 */

enum fki_x86_exception_owner_state {
	FKI_X86_EXC_OWNER_UNARMED,
	FKI_X86_EXC_OWNER_CONTROL_ARMED,
	FKI_X86_EXC_OWNER_IDT_ARMED,
	FKI_X86_EXC_OWNER_FOREIGN,
	FKI_X86_EXC_OWNER_RECOVERABLE_FAULT,
	FKI_X86_EXC_OWNER_FATAL_FAULT,
	FKI_X86_EXC_OWNER_EXITING,
	FKI_X86_EXC_OWNER_DONE,
};

enum fki_x86_exception_owner_event {
	FKI_X86_EXC_OWNER_ARM_CONTROL,
	FKI_X86_EXC_OWNER_INSTALL_IDT,
	FKI_X86_EXC_OWNER_ENTER_FOREIGN,
	FKI_X86_EXC_OWNER_SYNC_FAULT,
	FKI_X86_EXC_OWNER_ACCEPT_RECOVERY,
	FKI_X86_EXC_OWNER_NORMAL_EXIT,
	FKI_X86_EXC_OWNER_ABORT,
	FKI_X86_EXC_OWNER_RESTORE_IDT,
	FKI_X86_EXC_OWNER_CLEAR_CONTROL,
};

enum fki_x86_exception_owner_outcome {
	FKI_X86_EXC_OWNER_OUTCOME_NONE,
	FKI_X86_EXC_OWNER_OUTCOME_COMPLETE,
	FKI_X86_EXC_OWNER_OUTCOME_ABORTED,
};

struct fki_x86_exception_owner_context {
	unsigned int cpu;
	unsigned long task_cookie;
	unsigned long idtr_cookie;
	unsigned long stack_cookie;
};

struct fki_x86_exception_owner {
	enum fki_x86_exception_owner_state state;
	enum fki_x86_exception_owner_outcome outcome;
	struct fki_x86_exception_owner_context context;
	bool control_armed;
	bool idt_installed;
	bool fault_pending;
	unsigned int fault_vector;
	unsigned int fault_count;
	unsigned int recovery_count;
};

static inline void
fki_x86_exception_owner_init(struct fki_x86_exception_owner *owner,
				     const struct fki_x86_exception_owner_context *context)
{
	memset(owner, 0, sizeof(*owner));
	owner->state = FKI_X86_EXC_OWNER_UNARMED;
	owner->outcome = FKI_X86_EXC_OWNER_OUTCOME_NONE;
	owner->context = *context;
}

static inline int
fki_x86_exception_owner_context_valid(const struct fki_x86_exception_owner *owner,
					      const struct fki_x86_exception_owner_context *context)
{
	if (!context || context->cpu != owner->context.cpu ||
	    context->task_cookie != owner->context.task_cookie ||
	    context->idtr_cookie != owner->context.idtr_cookie ||
	    context->stack_cookie != owner->context.stack_cookie ||
	    !context->task_cookie || !context->idtr_cookie || !context->stack_cookie)
		return -EUCLEAN;
	return 0;
}

static inline int
fki_x86_exception_owner_step(struct fki_x86_exception_owner *owner,
			     enum fki_x86_exception_owner_event event,
			     const struct fki_x86_exception_owner_context *context,
			     unsigned int vector)
{
	int ret = fki_x86_exception_owner_context_valid(owner, context);

	if (ret)
		return ret;

	if (event == FKI_X86_EXC_OWNER_SYNC_FAULT &&
	    (vector == 2 || vector >= 32))
		return -EOPNOTSUPP;

	switch (event) {
	case FKI_X86_EXC_OWNER_ARM_CONTROL:
		if (owner->state != FKI_X86_EXC_OWNER_UNARMED || owner->control_armed)
			return -EINVAL;
		owner->control_armed = true;
		owner->state = FKI_X86_EXC_OWNER_CONTROL_ARMED;
		return 0;
	case FKI_X86_EXC_OWNER_INSTALL_IDT:
		if (owner->state != FKI_X86_EXC_OWNER_CONTROL_ARMED || owner->idt_installed)
			return -EINVAL;
		owner->idt_installed = true;
		owner->state = FKI_X86_EXC_OWNER_IDT_ARMED;
		return 0;
	case FKI_X86_EXC_OWNER_ENTER_FOREIGN:
		if (owner->state != FKI_X86_EXC_OWNER_IDT_ARMED)
			return -EINVAL;
		owner->state = FKI_X86_EXC_OWNER_FOREIGN;
		return 0;
	case FKI_X86_EXC_OWNER_SYNC_FAULT:
		if (owner->state == FKI_X86_EXC_OWNER_RECOVERABLE_FAULT ||
		    owner->state == FKI_X86_EXC_OWNER_FATAL_FAULT) {
			owner->state = FKI_X86_EXC_OWNER_FATAL_FAULT;
			owner->fault_pending = true;
			return 0;
		}
		if (owner->state != FKI_X86_EXC_OWNER_FOREIGN)
			return -EINVAL;
		owner->fault_count++;
		owner->fault_vector = vector;
		owner->fault_pending = true;
		if (vector == 13 || vector == 14)
			owner->state = FKI_X86_EXC_OWNER_RECOVERABLE_FAULT;
		else
			owner->state = FKI_X86_EXC_OWNER_FATAL_FAULT;
		return 0;
	case FKI_X86_EXC_OWNER_ACCEPT_RECOVERY:
		if (owner->state != FKI_X86_EXC_OWNER_RECOVERABLE_FAULT ||
		    !owner->fault_pending)
			return -EINVAL;
		owner->fault_pending = false;
		owner->recovery_count++;
		owner->state = FKI_X86_EXC_OWNER_FOREIGN;
		return 0;
	case FKI_X86_EXC_OWNER_NORMAL_EXIT:
		if (owner->state != FKI_X86_EXC_OWNER_FOREIGN || owner->fault_pending)
			return -EINVAL;
		owner->outcome = FKI_X86_EXC_OWNER_OUTCOME_COMPLETE;
		owner->state = FKI_X86_EXC_OWNER_EXITING;
		return 0;
	case FKI_X86_EXC_OWNER_ABORT:
		if (owner->state == FKI_X86_EXC_OWNER_UNARMED ||
		    owner->state == FKI_X86_EXC_OWNER_EXITING ||
		    owner->state == FKI_X86_EXC_OWNER_DONE)
			return -EINVAL;
		owner->outcome = FKI_X86_EXC_OWNER_OUTCOME_ABORTED;
		owner->state = FKI_X86_EXC_OWNER_EXITING;
		return 0;
	case FKI_X86_EXC_OWNER_RESTORE_IDT:
		if (owner->state != FKI_X86_EXC_OWNER_EXITING || !owner->idt_installed)
			return -EINVAL;
		owner->idt_installed = false;
		return 0;
	case FKI_X86_EXC_OWNER_CLEAR_CONTROL:
		if (owner->state != FKI_X86_EXC_OWNER_EXITING || owner->idt_installed ||
		    !owner->control_armed)
			return -EINVAL;
		owner->control_armed = false;
		owner->state = FKI_X86_EXC_OWNER_DONE;
		return 0;
	default:
		return -EINVAL;
	}
}

#endif
