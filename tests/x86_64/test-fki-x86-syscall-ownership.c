// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include "../../fki-ko/fki_x86_64_syscall_ownership.h"

#define STEP(o,e) fki_x86_syscall_owner_step((o),(e),&ctx)

static const struct fki_x86_syscall_owner_context ctx = {
	.cpu = 1,
	.linux_task_cookie = 0xaaaa,
	.foreign_task_cookie = 0xbbbb,
};

static void init_owner(struct fki_x86_syscall_owner *o)
{
	fki_x86_syscall_owner_init(o, &ctx);
	assert(STEP(o, FKI_X86_SYSCALL_OWNER_REGISTER_FOREIGN) == 0);
}

int main(void)
{
	struct fki_x86_syscall_owner o;
	struct fki_x86_syscall_owner_context wrong = ctx;

	init_owner(&o);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_LINUX_NATIVE_SYSCALL) == 0);
	assert(o.linux_native_count == 1);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_FOREIGN_SYSCALL) == 0);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_REPLY_HANDOFF) == -EINVAL);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_ACCEPT_HANDOFF) == 0);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_ACCEPT_HANDOFF) == -EINVAL);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_REPLY_HANDOFF) == 0);
	assert(o.handoff_count == 1 && o.accept_count == 1 && o.reply_count == 1);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_FOREIGN_SYSCALL) == 0);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_ACCEPT_HANDOFF) == 0);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_REJECT_HANDOFF) == 0);
	assert(o.reject_count == 1);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_UNREGISTER_FOREIGN) == 0);

	init_owner(&o);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_FOREIGN_SYSCALL) == 0);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_FOREIGN_SYSCALL) == 0);
	assert(o.state == FKI_X86_SYSCALL_OWNER_ABORTED);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_REPLY_HANDOFF) == -EINVAL);

	init_owner(&o);
	assert(STEP(&o, FKI_X86_SYSCALL_OWNER_UNKNOWN_OWNER_SYSCALL) == 0);
	assert(o.state == FKI_X86_SYSCALL_OWNER_ABORTED);

	init_owner(&o);
	wrong.cpu = 2;
	assert(fki_x86_syscall_owner_step(&o, FKI_X86_SYSCALL_OWNER_FOREIGN_SYSCALL, &wrong) == -EUCLEAN);
	wrong = ctx;
	wrong.linux_task_cookie = wrong.foreign_task_cookie;
	assert(fki_x86_syscall_owner_step(&o, FKI_X86_SYSCALL_OWNER_FOREIGN_SYSCALL, &wrong) == -EUCLEAN);

	puts("x86 syscall ownership model fixtures passed");
	return 0;
}
