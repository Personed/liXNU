#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "../../fki-ko/fki_x86_64_exception_ownership.h"

#define STEP(o,e,c,v) fki_x86_exception_owner_step((o),(e),(c),(v))

static const struct fki_x86_exception_owner_context ctx = {
	.cpu = 3,
	.task_cookie = 0x1111,
	.idtr_cookie = 0x2222,
	.stack_cookie = 0x3333,
};

static void init(struct fki_x86_exception_owner *o)
{
	fki_x86_exception_owner_init(o, &ctx);
}

static void arm_and_enter(struct fki_x86_exception_owner *o)
{
	assert(STEP(o, FKI_X86_EXC_OWNER_ARM_CONTROL, &ctx, 0) == 0);
	assert(STEP(o, FKI_X86_EXC_OWNER_INSTALL_IDT, &ctx, 0) == 0);
	assert(STEP(o, FKI_X86_EXC_OWNER_ENTER_FOREIGN, &ctx, 0) == 0);
	assert(o->state == FKI_X86_EXC_OWNER_FOREIGN);
}

static void teardown_complete(struct fki_x86_exception_owner *o)
{
	assert(STEP(o, FKI_X86_EXC_OWNER_NORMAL_EXIT, &ctx, 0) == 0);
	assert(STEP(o, FKI_X86_EXC_OWNER_RESTORE_IDT, &ctx, 0) == 0);
	assert(STEP(o, FKI_X86_EXC_OWNER_CLEAR_CONTROL, &ctx, 0) == 0);
	assert(o->state == FKI_X86_EXC_OWNER_DONE);
	assert(o->outcome == FKI_X86_EXC_OWNER_OUTCOME_COMPLETE);
}

int main(void)
{
	struct fki_x86_exception_owner o;
	struct fki_x86_exception_owner_context wrong = ctx;

	init(&o);
	arm_and_enter(&o);
	assert(STEP(&o, FKI_X86_EXC_OWNER_SYNC_FAULT, &ctx, 14) == 0);
	assert(o.state == FKI_X86_EXC_OWNER_RECOVERABLE_FAULT);
	assert(STEP(&o, FKI_X86_EXC_OWNER_ACCEPT_RECOVERY, &ctx, 0) == 0);
	assert(o.recovery_count == 1);
	teardown_complete(&o);

	init(&o);
	arm_and_enter(&o);
	assert(STEP(&o, FKI_X86_EXC_OWNER_SYNC_FAULT, &ctx, 13) == 0);
	assert(STEP(&o, FKI_X86_EXC_OWNER_SYNC_FAULT, &ctx, 14) == 0);
	assert(o.state == FKI_X86_EXC_OWNER_FATAL_FAULT);
	assert(STEP(&o, FKI_X86_EXC_OWNER_ABORT, &ctx, 0) == 0);
	assert(STEP(&o, FKI_X86_EXC_OWNER_CLEAR_CONTROL, &ctx, 0) == -EINVAL);
	assert(STEP(&o, FKI_X86_EXC_OWNER_RESTORE_IDT, &ctx, 0) == 0);
	assert(STEP(&o, FKI_X86_EXC_OWNER_RESTORE_IDT, &ctx, 0) == -EINVAL);
	assert(STEP(&o, FKI_X86_EXC_OWNER_CLEAR_CONTROL, &ctx, 0) == 0);
	assert(o.state == FKI_X86_EXC_OWNER_DONE);
	assert(o.outcome == FKI_X86_EXC_OWNER_OUTCOME_ABORTED);

	init(&o);
	assert(STEP(&o, FKI_X86_EXC_OWNER_ARM_CONTROL, &ctx, 0) == 0);
	assert(STEP(&o, FKI_X86_EXC_OWNER_ABORT, &ctx, 0) == 0);
	assert(STEP(&o, FKI_X86_EXC_OWNER_CLEAR_CONTROL, &ctx, 0) == 0);
	assert(o.outcome == FKI_X86_EXC_OWNER_OUTCOME_ABORTED);

	init(&o);
	arm_and_enter(&o);
	assert(STEP(&o, FKI_X86_EXC_OWNER_SYNC_FAULT, &ctx, 8) == 0);
	assert(o.state == FKI_X86_EXC_OWNER_FATAL_FAULT);
	assert(STEP(&o, FKI_X86_EXC_OWNER_ACCEPT_RECOVERY, &ctx, 0) == -EINVAL);
	assert(STEP(&o, FKI_X86_EXC_OWNER_ABORT, &ctx, 0) == 0);
	assert(STEP(&o, FKI_X86_EXC_OWNER_RESTORE_IDT, &ctx, 0) == 0);
	assert(STEP(&o, FKI_X86_EXC_OWNER_CLEAR_CONTROL, &ctx, 0) == 0);

	init(&o);
	arm_and_enter(&o);
	assert(STEP(&o, FKI_X86_EXC_OWNER_SYNC_FAULT, &ctx, 2) == -EOPNOTSUPP);
	assert(STEP(&o, FKI_X86_EXC_OWNER_SYNC_FAULT, &ctx, 32) == -EOPNOTSUPP);
	wrong.cpu = 2;
	assert(STEP(&o, FKI_X86_EXC_OWNER_NORMAL_EXIT, &wrong, 0) == -EUCLEAN);
	wrong = ctx;
	wrong.idtr_cookie = 1;
	assert(STEP(&o, FKI_X86_EXC_OWNER_NORMAL_EXIT, &wrong, 0) == -EUCLEAN);

	puts("x86 exception ownership model fixtures passed");
	return 0;
}
