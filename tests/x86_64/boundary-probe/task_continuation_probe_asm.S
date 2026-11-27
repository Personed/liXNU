/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>

SYM_FUNC_START(bp_task_continuation_switch_stack)
	pushq %rbp
	pushq %rbx
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15

	movq %rsp, %r12
	movq %rsi, %r13
	movq %rdi, %r14
	andq $-16, %r14
	subq $16, %r14
	movq %r12, 0(%r14)
	movq %r13, 8(%r14)
	movq %r14, %rsp
	movq 8(%rsp), %rdi
	call bp_task_continuation_body
	movq 0(%rsp), %rsp

	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
	RET
SYM_FUNC_END(bp_task_continuation_switch_stack)
