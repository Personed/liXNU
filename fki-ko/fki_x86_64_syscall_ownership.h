/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FKI_X86_64_SYSCALL_OWNERSHIP_H
#define FKI_X86_64_SYSCALL_OWNERSHIP_H

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <stdbool.h>

/* Validation-only syscall ownership/routing oracle; not a real syscall mechanism. */

enum fki_x86_syscall_owner_state {
	FKI_X86_SYSCALL_OWNER_INACTIVE,
	FKI_X86_SYSCALL_OWNER_FOREIGN_ACTIVE,
	FKI_X86_SYSCALL_OWNER_HANDOFF_PENDING,
	FKI_X86_SYSCALL_OWNER_DONE,
	FKI_X86_SYSCALL_OWNER_ABORTED,
};

enum fki_x86_syscall_owner_event {
	FKI_X86_SYSCALL_OWNER_REGISTER_FOREIGN,
	FKI_X86_SYSCALL_OWNER_LINUX_NATIVE_SYSCALL,
	FKI_X86_SYSCALL_OWNER_FOREIGN_SYSCALL,
	FKI_X86_SYSCALL_OWNER_UNKNOWN_OWNER_SYSCALL,
	FKI_X86_SYSCALL_OWNER_ACCEPT_HANDOFF,
	FKI_X86_SYSCALL_OWNER_REPLY_HANDOFF,
	FKI_X86_SYSCALL_OWNER_REJECT_HANDOFF,
	FKI_X86_SYSCALL_OWNER_UNREGISTER_FOREIGN,
	FKI_X86_SYSCALL_OWNER_ABORT,
};

struct fki_x86_syscall_owner_context {
	unsigned int cpu;
	unsigned long linux_task_cookie;
	unsigned long foreign_task_cookie;
};

struct fki_x86_syscall_owner {
	enum fki_x86_syscall_owner_state state;
	struct fki_x86_syscall_owner_context context;
	bool handoff_accepted;
	unsigned int handoff_count;
	unsigned int accept_count;
	unsigned int reply_count;
	unsigned int reject_count;
	unsigned int linux_native_count;
};

static inline void
fki_x86_syscall_owner_init(struct fki_x86_syscall_owner *owner,
				   const struct fki_x86_syscall_owner_context *context)
{
	memset(owner, 0, sizeof(*owner));
	owner->state = FKI_X86_SYSCALL_OWNER_INACTIVE;
	owner->context = *context;
}

static inline int
fki_x86_syscall_owner_context_valid(const struct fki_x86_syscall_owner *owner,
				    const struct fki_x86_syscall_owner_context *context)
{
	if (!context || context->cpu != owner->context.cpu ||
	    context->linux_task_cookie != owner->context.linux_task_cookie ||
	    context->foreign_task_cookie != owner->context.foreign_task_cookie ||
	    !context->linux_task_cookie || !context->foreign_task_cookie ||
	    context->linux_task_cookie == context->foreign_task_cookie)
		return -EUCLEAN;
	return 0;
}

static inline int
fki_x86_syscall_owner_step(struct fki_x86_syscall_owner *owner,
			   enum fki_x86_syscall_owner_event event,
			   const struct fki_x86_syscall_owner_context *context)
{
	int ret = fki_x86_syscall_owner_context_valid(owner, context);

	if (ret)
		return ret;
	if (owner->state == FKI_X86_SYSCALL_OWNER_ABORTED)
		return -EINVAL;

	switch (event) {
	case FKI_X86_SYSCALL_OWNER_REGISTER_FOREIGN:
		if (owner->state != FKI_X86_SYSCALL_OWNER_INACTIVE)
			return -EINVAL;
		owner->state = FKI_X86_SYSCALL_OWNER_FOREIGN_ACTIVE;
		return 0;
	case FKI_X86_SYSCALL_OWNER_LINUX_NATIVE_SYSCALL:
		owner->linux_native_count++;
		return 0;
	case FKI_X86_SYSCALL_OWNER_FOREIGN_SYSCALL:
		if (owner->state == FKI_X86_SYSCALL_OWNER_HANDOFF_PENDING) {
			owner->state = FKI_X86_SYSCALL_OWNER_ABORTED;
			return 0;
		}
		if (owner->state != FKI_X86_SYSCALL_OWNER_FOREIGN_ACTIVE)
			return -EINVAL;
		owner->handoff_count++;
		owner->handoff_accepted = false;
		owner->state = FKI_X86_SYSCALL_OWNER_HANDOFF_PENDING;
		return 0;
	case FKI_X86_SYSCALL_OWNER_UNKNOWN_OWNER_SYSCALL:
		owner->state = FKI_X86_SYSCALL_OWNER_ABORTED;
		return 0;
	case FKI_X86_SYSCALL_OWNER_ACCEPT_HANDOFF:
		if (owner->state != FKI_X86_SYSCALL_OWNER_HANDOFF_PENDING ||
		    owner->handoff_accepted)
			return -EINVAL;
		owner->handoff_accepted = true;
		owner->accept_count++;
		return 0;
	case FKI_X86_SYSCALL_OWNER_REPLY_HANDOFF:
	case FKI_X86_SYSCALL_OWNER_REJECT_HANDOFF:
		if (owner->state != FKI_X86_SYSCALL_OWNER_HANDOFF_PENDING ||
		    !owner->handoff_accepted)
			return -EINVAL;
		if (event == FKI_X86_SYSCALL_OWNER_REPLY_HANDOFF)
			owner->reply_count++;
		else
			owner->reject_count++;
		owner->handoff_accepted = false;
		owner->state = FKI_X86_SYSCALL_OWNER_FOREIGN_ACTIVE;
		return 0;
	case FKI_X86_SYSCALL_OWNER_UNREGISTER_FOREIGN:
		if (owner->state != FKI_X86_SYSCALL_OWNER_FOREIGN_ACTIVE)
			return -EINVAL;
		owner->state = FKI_X86_SYSCALL_OWNER_DONE;
		return 0;
	case FKI_X86_SYSCALL_OWNER_ABORT:
		if (owner->state == FKI_X86_SYSCALL_OWNER_DONE)
			return -EINVAL;
		owner->state = FKI_X86_SYSCALL_OWNER_ABORTED;
		return 0;
	default:
		return -EINVAL;
	}
}

#endif
