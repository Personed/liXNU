// SPDX-License-Identifier: GPL-2.0-only
#include <linux/linkage.h>
#include <asm/nospec-branch.h>

/* Run fn(arg) on the caller-provided stack and restore the original stack. */
SYM_FUNC_START(fki_run_on_stack)
	pushq %rbp
	movq %rsp, %rbp
	movq %rdx, %rsp
	andq $-16, %rsp
	movq %rdi, %rax
	movq %rsi, %rdi
	CALL_NOSPEC rax
	movq %rbp, %rsp
	popq %rbp
	RET
SYM_FUNC_END(fki_run_on_stack)
