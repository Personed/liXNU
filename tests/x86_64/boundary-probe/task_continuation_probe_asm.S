/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>

SYM_FUNC_START(bp_task_continuation_switch_stack)
	pushq %rbp
	/* Keep the caller frame anchored to the task stack across the switch. */
	movq %rsp, %rbp
	movq %rdi, %rsp
	andq $-16, %rsp
	movq %rsi, %rdi
	call bp_task_continuation_body
	movq %rbp, %rsp
	popq %rbp
	RET
SYM_FUNC_END(bp_task_continuation_switch_stack)
