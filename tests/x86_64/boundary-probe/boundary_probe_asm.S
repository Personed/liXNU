/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>
#include "boundary_probe_asm.h"

/*
 * Switch to a probe-only stack, deliberately clobber callee-saved registers,
 * take a normal or software-abort return, then restore the Linux call frame.
 * The stub does not alter CR3/GS or call C while the sentinels are installed.
 */
SYM_FUNC_START(x86_boundary_probe_switch_stack)
	pushq %rbp
	pushq %rbx
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15

	movq %rsi, %r12
	movl %edx, %r13d
	leaq 48(%rsp), %rax
	movq %rax, BP_OFF_HOST_ENTRY_RSP(%r12)
	movq 40(%rsp), %rax
	movq %rax, BP_OFF_GPRS_ENTRY+BP_GPR_RBP(%r12)
	movq 32(%rsp), %rax
	movq %rax, BP_OFF_GPRS_ENTRY+BP_GPR_RBX(%r12)
	movq 24(%rsp), %rax
	movq %rax, BP_OFF_GPRS_ENTRY+BP_GPR_R12(%r12)
	movq 16(%rsp), %rax
	movq %rax, BP_OFF_GPRS_ENTRY+BP_GPR_R13(%r12)
	movq 8(%rsp), %rax
	movq %rax, BP_OFF_GPRS_ENTRY+BP_GPR_R14(%r12)
	movq 0(%rsp), %rax
	movq %rax, BP_OFF_GPRS_ENTRY+BP_GPR_R15(%r12)
	pushfq
	popq %rax
	movq %rax, BP_OFF_RFLAGS_ENTRY(%r12)

	movq %rdi, %r14
	movq %rsp, %r15
	andq $-16, %r14
	subq $32, %r14
	movq %r15, 0(%r14)
	movq %r12, 8(%r14)
	movq %r13, 16(%r14)
	movq %r14, BP_OFF_ALT_SWITCH_RSP(%r12)
	movq %r14, %rsp
	movq 8(%rsp), %rdi
	movq 16(%rsp), %rsi
	call x86_boundary_probe_inner

	movq 8(%rsp), %r10
	movq %rax, BP_OFF_STATUS(%r10)
	pushfq
	popq %rax
	movq %rax, BP_OFF_RFLAGS_INNER_RETURN(%r10)
	movq %rsp, BP_OFF_ALT_RETURN_RSP(%r10)
	movq 0(%rsp), %r11
	movq %r11, %rsp
	leaq 48(%rsp), %rax
	movq %rax, BP_OFF_HOST_RETURN_RSP(%r10)

	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
	movq BP_OFF_RFLAGS_ENTRY(%r10), %rax
	pushq %rax
	popfq
	pushfq
	popq %rax
	movq %rax, BP_OFF_RFLAGS_RETURN(%r10)
	movq %rbp, BP_OFF_GPRS_RETURN+BP_GPR_RBP(%r10)
	movq %rbx, BP_OFF_GPRS_RETURN+BP_GPR_RBX(%r10)
	movq %r12, BP_OFF_GPRS_RETURN+BP_GPR_R12(%r10)
	movq %r13, BP_OFF_GPRS_RETURN+BP_GPR_R13(%r10)
	movq %r14, BP_OFF_GPRS_RETURN+BP_GPR_R14(%r10)
	movq %r15, BP_OFF_GPRS_RETURN+BP_GPR_R15(%r10)
	movq BP_OFF_STATUS(%r10), %rax
	RET
SYM_FUNC_END(x86_boundary_probe_switch_stack)

SYM_FUNC_START(x86_boundary_probe_inner)
	movq %rsp, %rax
	cmpl $BP_COOP_MODE_RESUME, %esi
	jne .Lstore_alt_entry_rsp
	movq %rax, BP_OFF_COOP_RESUME_RSP(%rdi)
	cmpq BP_OFF_ALT_ENTRY_RSP(%rdi), %rax
	jne .Lcooperative_stale
.Lstore_alt_entry_rsp:
	movq %rax, BP_OFF_ALT_ENTRY_RSP(%rdi)

	movabsq $0x5555666677778888, %rbp
	movabsq $0x1111222233334444, %rbx
	movabsq $0x9999aaaabbbbcccc, %r12
	movabsq $0xddddeeeeffff0001, %r13
	movabsq $0x123456789abcdef0, %r14
	movabsq $0xfedcba9876543210, %r15
	movq %rbp, BP_OFF_GPRS_DURING+BP_GPR_RBP(%rdi)
	movq %rbx, BP_OFF_GPRS_DURING+BP_GPR_RBX(%rdi)
	movq %r12, BP_OFF_GPRS_DURING+BP_GPR_R12(%rdi)
	movq %r13, BP_OFF_GPRS_DURING+BP_GPR_R13(%rdi)
	movq %r14, BP_OFF_GPRS_DURING+BP_GPR_R14(%rdi)
	movq %r15, BP_OFF_GPRS_DURING+BP_GPR_R15(%rdi)
	std
	stc
	pushfq
	popq %rax
	movq %rax, BP_OFF_RFLAGS_DURING(%rdi)
	cld

	cmpl $BP_COOP_MODE_STOP, %esi
	je .Lcooperative_stop
	cmpl $BP_COOP_MODE_RESUME, %esi
	je .Lcooperative_resume
	cmpl $BP_RUN_MODE_SHUTDOWN_HALT, %esi
	je x86_boundary_probe_shutdown_halt_stop
	cmpl $BP_RUN_MODE_SHUTDOWN_RESTART, %esi
	je x86_boundary_probe_shutdown_restart_stop
	testl %esi, %esi
	jnz x86_boundary_probe_abort_stop
	.globl x86_boundary_probe_normal_stop
x86_boundary_probe_normal_stop:
	movq %rsp, BP_OFF_STOP_RSP(%rdi)
	leaq x86_boundary_probe_normal_stop(%rip), %r11
	movq %r11, BP_OFF_STOP_RIP(%rdi)
	xorl %eax, %eax
	jmp .Lreturn_with_changed_flags
.Lcooperative_stop:
	movq BP_OFF_COOP_GENERATION(%rdi), %rax
	movabsq $BP_COOP_TOKEN_XOR, %r11
	xorq %r11, %rax
	movq %rax, BP_OFF_COOP_TOKEN(%rdi)
	movq $0, BP_OFF_COOP_RESUME_COUNT(%rdi)
	movq $-115, %rax /* -EINPROGRESS: return to the Linux task before it blocks. */
	jmp .Lreturn_with_changed_flags
.Lcooperative_resume:
	cmpq $0, BP_OFF_COOP_RESUME_COUNT(%rdi)
	jne .Lcooperative_already_resumed
	cmpq $BP_COOP_STATE_REPLIED, BP_OFF_COOP_STATE(%rdi)
	jne .Lcooperative_stale
	movq BP_OFF_COOP_GENERATION(%rdi), %rax
	movabsq $BP_COOP_TOKEN_XOR, %r11
	xorq %r11, %rax
	cmpq BP_OFF_COOP_TOKEN(%rdi), %rax
	jne .Lcooperative_stale
	movq BP_OFF_COOP_GENERATION(%rdi), %rax
	movabsq $BP_COOP_REPLY_XOR, %r11
	xorq %r11, %rax
	cmpq BP_OFF_COOP_REPLY(%rdi), %rax
	jne .Lcooperative_stale
	incq BP_OFF_COOP_RESUME_COUNT(%rdi)
	xorl %eax, %eax
	jmp .Lreturn_with_changed_flags
.Lcooperative_already_resumed:
	movq $-114, %rax /* -EALREADY */
	jmp .Lreturn_with_changed_flags
.Lcooperative_stale:
	movq $-116, %rax /* -ESTALE */
	jmp .Lreturn_with_changed_flags
	.globl x86_boundary_probe_shutdown_halt_stop
x86_boundary_probe_shutdown_halt_stop:
	movq %rsp, BP_OFF_STOP_RSP(%rdi)
	leaq x86_boundary_probe_shutdown_halt_stop(%rip), %r11
	movq %r11, BP_OFF_STOP_RIP(%rdi)
	xorl %eax, %eax
	jmp .Lreturn_with_changed_flags
	.globl x86_boundary_probe_shutdown_restart_stop
x86_boundary_probe_shutdown_restart_stop:
	movq %rsp, BP_OFF_STOP_RSP(%rdi)
	leaq x86_boundary_probe_shutdown_restart_stop(%rip), %r11
	movq %r11, BP_OFF_STOP_RIP(%rdi)
	xorl %eax, %eax
	jmp .Lreturn_with_changed_flags
	.globl x86_boundary_probe_abort_stop
x86_boundary_probe_abort_stop:
	movq %rsp, BP_OFF_STOP_RSP(%rdi)
	leaq x86_boundary_probe_abort_stop(%rip), %r11
	movq %r11, BP_OFF_STOP_RIP(%rdi)
	movq $-125, %rax /* -ECANCELED */

.Lreturn_with_changed_flags:
	/* Make both CF and DF differ from the entry state so the outer restore is tested. */
	testq $0x400, BP_OFF_RFLAGS_ENTRY(%rdi)
	jz .Lentry_df_clear
	cld
	jmp .Lentry_df_done
.Lentry_df_clear:
	std
.Lentry_df_done:
	movq BP_OFF_RFLAGS_ENTRY(%rdi), %r8
	btq $0, %r8
	jc .Lentry_cf_set
	stc
	RET
.Lentry_cf_set:
	clc
	RET
SYM_FUNC_END(x86_boundary_probe_inner)
