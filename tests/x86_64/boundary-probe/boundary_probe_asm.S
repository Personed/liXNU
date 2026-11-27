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

	cmpl $BP_MODE_EXCEPTION_PF, %esi
	jae .Lexception_window
	testl %esi, %esi
	jnz .Lsoftware_abort
	xorl %eax, %eax
	jmp .Lreturn_with_changed_flags
.Lsoftware_abort:
	movq $-125, %rax /* -ECANCELED */
	jmp .Lreturn_with_changed_flags

.Lexception_window:
	leaq x86_boundary_probe_exception_control(%rip), %rax
	movq %rsp, BPX_OFF_ENVELOPE_RSP(%rax)
	leaq .Lpost_exception_injection(%rip), %rcx
	movq %rcx, BPX_OFF_POST_INJECTION_RIP(%rax)
	cmpl $BP_MODE_EXCEPTION_GP, %esi
	je .Lexception_setup_gp
	leaq .Lpf_fault_instruction(%rip), %rcx
	movq %rcx, BPX_OFF_EXPECTED_RIP(%rax)
	movq $14, BPX_OFF_EXPECTED_VECTOR(%rax)
	movq $BPX_STATE_ARMED, BPX_OFF_STATE(%rax)
	lidt BPX_OFF_SHADOW_DESC(%rax)
	xorl %eax, %eax
.Lpf_fault_instruction:
	movb (%rax), %al
	jmp .Lpost_exception_injection

.Lexception_setup_gp:
	leaq .Lgp_fault_instruction(%rip), %rcx
	movq %rcx, BPX_OFF_EXPECTED_RIP(%rax)
	movq $13, BPX_OFF_EXPECTED_VECTOR(%rax)
	movq $BPX_STATE_ARMED, BPX_OFF_STATE(%rax)
	movq $0, BPX_OFF_FAULT_SCRATCH_RAX(%rax)
	movabs $0xdeadbeefdeadbeef, %rcx
	movq %rcx, BPX_OFF_FAULT_SCRATCH_RAX+2(%rax)
	lidt BPX_OFF_SHADOW_DESC(%rax)
.Lgp_fault_instruction:
	lidt BPX_OFF_FAULT_SCRATCH_RAX(%rax)

.Lpost_exception_injection:
	leaq x86_boundary_probe_exception_control(%rip), %rax
	lidt BPX_OFF_ORIGINAL_DESC(%rax)
	sidt BPX_OFF_READBACK_DESC(%rax)
	movzwl BPX_OFF_ORIGINAL_DESC(%rax), %ecx
	movzwl BPX_OFF_READBACK_DESC(%rax), %edx
	cmpl %edx, %ecx
	jne .Lterminal_post
	movq BPX_OFF_ORIGINAL_DESC+2(%rax), %rcx
	cmpq BPX_OFF_READBACK_DESC+2(%rax), %rcx
	jne .Lterminal_post
	orq $BPX_PROOF_IDTR, BPX_OFF_PROOF_FLAGS(%rax)
	movq BPX_OFF_ENVELOPE_RSP(%rax), %rcx
	cmpq %rsp, %rcx
	jne .Lterminal_post
	movq %rsp, BPX_OFF_FINAL_RSP(%rax)
	orq $BPX_PROOF_RSP, BPX_OFF_PROOF_FLAGS(%rax)
	cmpq $1, BPX_OFF_RECOVERY_COUNT(%rax)
	je .Lexception_recovered
	cmpq $0, BPX_OFF_RECOVERY_COUNT(%rax)
	je .Lexception_done
	jmp .Lterminal_post
.Lexception_recovered:
	cmpq $BPX_STATE_DISARMED, BPX_OFF_STATE(%rax)
	jne .Lterminal_post
.Lexception_done:
	xorl %eax, %eax
	jmp .Lreturn_with_changed_flags

.Lterminal_post:
	movl $0x43, %eax
	jmp x86_boundary_probe_terminal

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

/* Raw #GP/#PF entries for the disposable shadow-IDT test. */
SYM_FUNC_START(x86_boundary_probe_gp_handler)
	movq %rax, x86_boundary_probe_exception_control+BPX_OFF_FAULT_SCRATCH_RAX(%rip)
	movl $13, %eax
	jmp x86_boundary_probe_exception_common
SYM_FUNC_END(x86_boundary_probe_gp_handler)

SYM_FUNC_START(x86_boundary_probe_pf_handler)
	movq %rax, x86_boundary_probe_exception_control+BPX_OFF_FAULT_SCRATCH_RAX(%rip)
	movl $14, %eax
	jmp x86_boundary_probe_exception_common
SYM_FUNC_END(x86_boundary_probe_pf_handler)

SYM_FUNC_START(x86_boundary_probe_exception_common)
	movq %rbx, x86_boundary_probe_exception_control+BPX_OFF_FAULT_SCRATCH_RBX(%rip)
	movq %rcx, x86_boundary_probe_exception_control+BPX_OFF_FAULT_SCRATCH_RCX(%rip)
	movq %rdx, x86_boundary_probe_exception_control+BPX_OFF_FAULT_SCRATCH_RDX(%rip)
	leaq x86_boundary_probe_exception_control(%rip), %rbx
	mov %cr2, %rcx
	movq %rcx, BPX_OFF_FAULT_CR2(%rbx)
	movabs $BPX_MAGIC, %rcx
	cmpq %rcx, BPX_OFF_MAGIC(%rbx)
	jne .Lterminal_raw
	cmpq $BPX_STATE_ARMED, BPX_OFF_STATE(%rbx)
	jne .Lterminal_raw
	cmpq BPX_OFF_EXPECTED_VECTOR(%rbx), %rax
	jne .Lterminal_raw
	cmpq $0, BPX_OFF_RECOVERY_COUNT(%rbx)
	jne .Lterminal_raw
	movq 8(%rsp), %rdx
	cmpq BPX_OFF_EXPECTED_RIP(%rbx), %rdx
	jne .Lterminal_raw
	movq %rax, BPX_OFF_FAULT_VECTOR(%rbx)
	movq 0(%rsp), %rcx
	movq %rcx, BPX_OFF_FAULT_ERROR(%rbx)
	movq %rdx, BPX_OFF_FAULT_RIP(%rbx)
	movq 24(%rsp), %rcx
	movq %rcx, BPX_OFF_FAULT_RFLAGS(%rbx)
	movq 32(%rsp), %rcx
	movq %rcx, BPX_OFF_FAULT_RSP(%rbx)
	movq 16(%rsp), %rcx
	movq %rcx, BPX_OFF_FAULT_CS(%rbx)
	movq 40(%rsp), %rcx
	movq %rcx, BPX_OFF_FAULT_SS(%rbx)
	movq $1, BPX_OFF_RECOVERY_COUNT(%rbx)
	leaq x86_boundary_probe_recovery(%rip), %rcx
	movq %rcx, 8(%rsp)
	movq BPX_OFF_RECOVERY_STACK_TOP(%rbx), %rcx
	movq %rcx, 32(%rsp)
	movq BPX_OFF_FAULT_SCRATCH_RDX(%rbx), %rdx
	movq BPX_OFF_FAULT_SCRATCH_RCX(%rbx), %rcx
	movq BPX_OFF_FAULT_SCRATCH_RBX(%rbx), %rbx
	movq x86_boundary_probe_exception_control+BPX_OFF_FAULT_SCRATCH_RAX(%rip), %rax
	addq $8, %rsp
	iretq

.Lterminal_raw:
	movl $0x41, %eax
	jmp x86_boundary_probe_terminal
SYM_FUNC_END(x86_boundary_probe_exception_common)

SYM_FUNC_START(x86_boundary_probe_recovery)
	movq %rax, x86_boundary_probe_exception_control+BPX_OFF_REC_SCRATCH_RAX(%rip)
	movq %rbx, x86_boundary_probe_exception_control+BPX_OFF_REC_SCRATCH_RBX(%rip)
	movq %rcx, x86_boundary_probe_exception_control+BPX_OFF_REC_SCRATCH_RCX(%rip)
	movq %rdx, x86_boundary_probe_exception_control+BPX_OFF_REC_SCRATCH_RDX(%rip)
	leaq x86_boundary_probe_exception_control(%rip), %rbx
	movabs $BPX_MAGIC, %rcx
	cmpq %rcx, BPX_OFF_MAGIC(%rbx)
	jne .Lterminal_recovery
	cmpq $1, BPX_OFF_RECOVERY_COUNT(%rbx)
	jne .Lterminal_recovery
	cmpq $BPX_STATE_ARMED, BPX_OFF_STATE(%rbx)
	jne .Lterminal_recovery
	movq BPX_OFF_FAULT_VECTOR(%rbx), %rax
	cmpq BPX_OFF_EXPECTED_VECTOR(%rbx), %rax
	jne .Lterminal_recovery
	movq BPX_OFF_FAULT_RIP(%rbx), %rax
	cmpq BPX_OFF_EXPECTED_RIP(%rbx), %rax
	jne .Lterminal_recovery
	movq BPX_OFF_RECOVERY_LOW_CANARY_PTR(%rbx), %rax
	movabs $BPX_RECOVERY_LOW_CANARY, %rcx
	cmpq %rcx, (%rax)
	jne .Lterminal_recovery
	movq BPX_OFF_RECOVERY_HIGH_CANARY_PTR(%rbx), %rax
	movabs $BPX_RECOVERY_HIGH_CANARY, %rcx
	cmpq %rcx, (%rax)
	jne .Lterminal_recovery
	movq $BPX_STATE_DISARMED, BPX_OFF_STATE(%rbx)
	lidt BPX_OFF_ORIGINAL_DESC(%rbx)
	sidt BPX_OFF_READBACK_DESC(%rbx)
	movzwl BPX_OFF_ORIGINAL_DESC(%rbx), %eax
	movzwl BPX_OFF_READBACK_DESC(%rbx), %ecx
	cmpl %ecx, %eax
	jne .Lterminal_recovery
	movq BPX_OFF_ORIGINAL_DESC+2(%rbx), %rax
	cmpq BPX_OFF_READBACK_DESC+2(%rbx), %rax
	jne .Lterminal_recovery
	orq $(BPX_PROOF_CANARIES | BPX_PROOF_IDTR), BPX_OFF_PROOF_FLAGS(%rbx)
	movq x86_boundary_probe_exception_control+BPX_OFF_REC_SCRATCH_RDX(%rip), %rdx
	movq x86_boundary_probe_exception_control+BPX_OFF_REC_SCRATCH_RCX(%rip), %rcx
	movq x86_boundary_probe_exception_control+BPX_OFF_REC_SCRATCH_RBX(%rip), %rbx
	movq x86_boundary_probe_exception_control+BPX_OFF_REC_SCRATCH_RAX(%rip), %rax
	movq x86_boundary_probe_exception_control+BPX_OFF_ENVELOPE_RSP(%rip), %rsp
	jmp .Lpost_exception_injection

.Lterminal_recovery:
	movl $0x42, %eax
	jmp x86_boundary_probe_terminal
SYM_FUNC_END(x86_boundary_probe_recovery)

SYM_FUNC_START(x86_boundary_probe_terminal)
	movq %rax, x86_boundary_probe_exception_control+BPX_OFF_TERMINAL_CODE(%rip)
	movl $0xf4, %edx
	outb %al, %dx
	cli
.Lterminal_halt:
	hlt
	jmp .Lterminal_halt
SYM_FUNC_END(x86_boundary_probe_terminal)

/* Vector-identifying terminal entries for unclassified synchronous exceptions. */
.macro BPX_TERMINAL vector
	.globl x86_boundary_probe_terminal_\vector
	.type x86_boundary_probe_terminal_\vector, @function
x86_boundary_probe_terminal_\vector:
	movq $\vector, x86_boundary_probe_exception_control+BPX_OFF_FAULT_VECTOR(%rip)
	movl $0x44, %eax
	jmp x86_boundary_probe_terminal
	.size x86_boundary_probe_terminal_\vector, .-x86_boundary_probe_terminal_\vector
.endm

BPX_TERMINAL 0
BPX_TERMINAL 1
BPX_TERMINAL 2
BPX_TERMINAL 3
BPX_TERMINAL 4
BPX_TERMINAL 5
BPX_TERMINAL 6
BPX_TERMINAL 7
BPX_TERMINAL 8
BPX_TERMINAL 9
BPX_TERMINAL 10
BPX_TERMINAL 11
BPX_TERMINAL 12
BPX_TERMINAL 13
BPX_TERMINAL 14
BPX_TERMINAL 15
BPX_TERMINAL 16
BPX_TERMINAL 17
BPX_TERMINAL 18
BPX_TERMINAL 19
BPX_TERMINAL 20
BPX_TERMINAL 21
BPX_TERMINAL 22
BPX_TERMINAL 23
BPX_TERMINAL 24
BPX_TERMINAL 25
BPX_TERMINAL 26
BPX_TERMINAL 27
BPX_TERMINAL 28
BPX_TERMINAL 29
BPX_TERMINAL 30
BPX_TERMINAL 31

.pushsection .rodata,"a",@progbits
	.globl x86_boundary_probe_terminal_vectors
	.align 8
x86_boundary_probe_terminal_vectors:
	.quad x86_boundary_probe_terminal_0
	.quad x86_boundary_probe_terminal_1
	.quad x86_boundary_probe_terminal_2
	.quad x86_boundary_probe_terminal_3
	.quad x86_boundary_probe_terminal_4
	.quad x86_boundary_probe_terminal_5
	.quad x86_boundary_probe_terminal_6
	.quad x86_boundary_probe_terminal_7
	.quad x86_boundary_probe_terminal_8
	.quad x86_boundary_probe_terminal_9
	.quad x86_boundary_probe_terminal_10
	.quad x86_boundary_probe_terminal_11
	.quad x86_boundary_probe_terminal_12
	.quad x86_boundary_probe_terminal_13
	.quad x86_boundary_probe_terminal_14
	.quad x86_boundary_probe_terminal_15
	.quad x86_boundary_probe_terminal_16
	.quad x86_boundary_probe_terminal_17
	.quad x86_boundary_probe_terminal_18
	.quad x86_boundary_probe_terminal_19
	.quad x86_boundary_probe_terminal_20
	.quad x86_boundary_probe_terminal_21
	.quad x86_boundary_probe_terminal_22
	.quad x86_boundary_probe_terminal_23
	.quad x86_boundary_probe_terminal_24
	.quad x86_boundary_probe_terminal_25
	.quad x86_boundary_probe_terminal_26
	.quad x86_boundary_probe_terminal_27
	.quad x86_boundary_probe_terminal_28
	.quad x86_boundary_probe_terminal_29
	.quad x86_boundary_probe_terminal_30
	.quad x86_boundary_probe_terminal_31
.popsection
