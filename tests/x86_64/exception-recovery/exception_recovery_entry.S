/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>
#include "exception_recovery_asm.h"

.text

.macro ER_PUSH_GPRS
	pushq %rax
	pushq %rbx
	pushq %rcx
	pushq %rdx
	pushq %rsi
	pushq %rdi
	pushq %rbp
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
.endm

.macro ER_POP_GPRS
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r11
	popq %r10
	popq %r9
	popq %r8
	popq %rbp
	popq %rdi
	popq %rsi
	popq %rdx
	popq %rcx
	popq %rbx
	popq %rax
.endm

#define ER_TERMINAL_CODE_BASE 0x45520000

.macro ER_CHECK_SCRATCH expected, offset
	movabsq $\expected, %rax
	cmpq %rax, er_recovery_scratch+\offset(%rip)
	jne .Lterminal_recovery_state
.endm

SYM_FUNC_START(x86_er_inject)
	/* Preserve the Linux task-stack envelope before selecting any scratch GPRs. */
	pushq %rbp
	pushq %rbx
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	movq %rdi, %r12
	movq %rsp, ER_OFF_ENVELOPE_RSP(%r12)
	movq %rbx, ER_OFF_SAVED_RBX(%r12)
	movq %rbp, ER_OFF_SAVED_RBP(%r12)
	movq 24(%rsp), %rax
	movq %rax, ER_OFF_SAVED_R12(%r12)
	movq %r13, ER_OFF_SAVED_R13(%r12)
	movq %r14, ER_OFF_SAVED_R14(%r12)
	movq %r15, ER_OFF_SAVED_R15(%r12)

	/* RSP moves to preallocated, canary-checked storage before shadow LIDT. */
	movq ER_OFF_FAULT_STACK_TOP(%r12), %rsp
	movq ER_OFF_FAULT_STACK_BASE(%r12), %rax
	addq $ER_TEST_STACK_TOP_OFFSET, %rax
	cmpq %rax, %rsp
	jne .Lstack_not_ready
	movq ER_OFF_FAULT_STACK_BASE(%r12), %rax
	movabsq $0x45524c4f5743414e, %rdx
	cmpq %rdx, (%rax)
	jne .Lstack_not_ready
	addq $ER_TEST_STACK_HIGH_CANARY, %rax
	movabsq $0x4552484947484341, %rdx
	cmpq %rdx, (%rax)
	jne .Lstack_not_ready
	movq ER_OFF_RECOVERY_STACK_BASE(%r12), %rax
	addq $ER_TEST_STACK_TOP_OFFSET, %rax
	cmpq ER_OFF_RECOVERY_STACK_TOP(%r12), %rax
	jne .Lstack_not_ready
	testq $15, %rsp
	jnz .Lstack_not_ready

	/* Record exact injection and recovery labels before arming the IDT. */
	cmpq $14, ER_OFF_EXPECTED_VECTOR(%r12)
	jne .Lselect_gp
	leaq .Lpf_fault(%rip), %rax
	movq %rax, ER_OFF_EXPECTED_RIP(%r12)
	leaq x86_er_recovery_stub(%rip), %rax
	movq %rax, ER_OFF_RECOVERY_RIP(%r12)
	jmp .Larm_shadow_idt
.Lselect_gp:
	cmpq $13, ER_OFF_EXPECTED_VECTOR(%r12)
	jne .Lstack_not_ready
	leaq .Lgp_fault(%rip), %rax
	movq %rax, ER_OFF_EXPECTED_RIP(%r12)
	leaq x86_er_recovery_stub(%rip), %rax
	movq %rax, ER_OFF_RECOVERY_RIP(%r12)

.Larm_shadow_idt:
	movabsq $ER_ARM_TOKEN, %rdx
	movq %rdx, ER_OFF_ARMED(%r12)
	lidt ER_OFF_SHADOW_IDTR(%r12)

	/* Seed distinct GPR values; RSP stays fixed through the faulting label. */
	movabsq $ER_SENTINEL_RAX, %rax
	movabsq $ER_SENTINEL_RBX, %rbx
	movabsq $ER_SENTINEL_RCX, %rcx
	movabsq $ER_SENTINEL_RDX, %rdx
	movabsq $ER_SENTINEL_RSI, %rsi
	movabsq $ER_SENTINEL_RDI, %rdi
	movabsq $ER_SENTINEL_RBP, %rbp
	movabsq $ER_SENTINEL_R8, %r8
	movabsq $ER_SENTINEL_R9, %r9
	movabsq $ER_SENTINEL_R10, %r10
	movabsq $ER_SENTINEL_R11, %r11
	movabsq $ER_SENTINEL_R12, %r12
	movabsq $ER_SENTINEL_R13, %r13
	movabsq $ER_SENTINEL_R14, %r14
	movabsq $ER_SENTINEL_R15, %r15

	/* The state pointer is reacquired to record the final, unmodified RSP. */
	movq er_active_state(%rip), %r12
	cmpq $14, ER_OFF_EXPECTED_VECTOR(%r12)
	jne .Lsetup_gp
	movq ER_OFF_FAULT_ADDRESS(%r12), %rax
	movq %rax, ER_OFF_EXPECTED_RAX(%r12)
	movq %rsp, ER_OFF_FAULT_WINDOW_RSP(%r12)
	movabsq $ER_SENTINEL_R12, %r12
.Lpf_fault:
	movq (%rax), %rax
	jmp .Lnot_faulted

.Lsetup_gp:
	movabsq $ER_SENTINEL_RAX, %rax
	movw $ER_GP_SELECTOR, %ax
	movq %rax, ER_OFF_EXPECTED_RAX(%r12)
	movq %rsp, ER_OFF_FAULT_WINDOW_RSP(%r12)
	movabsq $ER_SENTINEL_R12, %r12
.Lgp_fault:
	movw %ax, %ds
	jmp .Lnot_faulted

.Lnot_faulted:
	/* A missing fault is a not-run failure; restore IDTR before returning to C. */
	movq er_active_state(%rip), %r12
	movq $1, ER_OFF_NO_FAULT(%r12)
	movq $0, ER_OFF_ARMED(%r12)
	lidt ER_OFF_ORIGINAL_IDTR(%r12)
	sidt ER_OFF_VERIFY_IDTR(%r12)
	movzwq ER_OFF_ORIGINAL_IDTR+ER_IDTR_LIMIT_OFF(%r12), %rax
	cmpw ER_OFF_VERIFY_IDTR+ER_IDTR_LIMIT_OFF(%r12), %ax
	jne .Lterminal_idtr
	movq ER_OFF_ORIGINAL_IDTR+ER_IDTR_BASE_OFF(%r12), %rax
	cmpq ER_OFF_VERIFY_IDTR+ER_IDTR_BASE_OFF(%r12), %rax
	jne .Lterminal_idtr
	movq $3, ER_OFF_IDTR_PROOF(%r12)
	movw ER_OFF_SAVED_DS(%r12), %ax
	movw %ax, %ds
	jmp .Lreturn_not_run

.Lstack_not_ready:
	movq $2, ER_OFF_NO_FAULT(%r12)
	jmp .Lreturn_setup_failure

.Lterminal_idtr:
	movl $0x4552ff01, %eax
	jmp .Lterminal_common

.Lreturn_not_run:
	/* RSP points to the saved Linux envelope; callee GPRs are popped below. */
	movq ER_OFF_ENVELOPE_RSP(%r12), %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
	movq $-95, %rax /* -EOPNOTSUPP */
	RET

.Lreturn_setup_failure:
	/* No shadow IDT was installed, so this return is still on Linux's IDT. */
	movq ER_OFF_ENVELOPE_RSP(%r12), %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
	movq $-117, %rax /* -EUCLEAN */
	RET

.Lpost_injection:
	/* The recovery stub restored the task-stack RSP before this label. */
	movq er_active_state(%rip), %rax
	testq %rax, %rax
	jz .Lterminal_restore_failure
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
	cmpq ER_OFF_SAVED_RBX(%rax), %rbx
	jne .Lterminal_restore_failure
	cmpq ER_OFF_SAVED_RBP(%rax), %rbp
	jne .Lterminal_restore_failure
	cmpq ER_OFF_SAVED_R12(%rax), %r12
	jne .Lterminal_restore_failure
	cmpq ER_OFF_SAVED_R13(%rax), %r13
	jne .Lterminal_restore_failure
	cmpq ER_OFF_SAVED_R14(%rax), %r14
	jne .Lterminal_restore_failure
	cmpq ER_OFF_SAVED_R15(%rax), %r15
	jne .Lterminal_restore_failure
	leaq -48(%rsp), %rdx
	cmpq ER_OFF_ENVELOPE_RSP(%rax), %rdx
	jne .Lterminal_restore_failure
	xorl %eax, %eax
	RET
.Lterminal_restore_failure:
	movl $0x4552ff03, %eax
	jmp .Lterminal_common
SYM_FUNC_END(x86_er_inject)

/* The prevalidated fault stack saves GPRs before CR2 is read. */
SYM_CODE_START(x86_er_pf_entry)
	ER_PUSH_GPRS
	movq %cr2, %r11
	movq er_active_state(%rip), %r15
	testq %r15, %r15
	jz .Lterminal_pf_unarmed
	leaq 120(%rsp), %r14
	movq %r11, ER_OFF_CR2(%r15)
	movq $14, ER_OFF_CAPTURED_VECTOR(%r15)
	movq 0(%r14), %rax
	movq %rax, ER_OFF_ERROR_CODE(%r15)
	movq 8(%r14), %rax
	movq %rax, ER_OFF_FAULT_RIP(%r15)
	movq 16(%r14), %rax
	movq %rax, ER_OFF_FAULT_CS(%r15)
	movq 24(%r14), %rax
	movq %rax, ER_OFF_FAULT_RFLAGS(%r15)
	movq 32(%r14), %rax
	movq %rax, ER_OFF_FAULT_RSP(%r15)
	movq 40(%r14), %rax
	movq %rax, ER_OFF_FAULT_SS(%r15)
	movabsq $ER_ARM_TOKEN, %rax
	cmpq %rax, ER_OFF_ARMED(%r15)
	jne .Lterminal_pf_unclassified
	cmpq $14, ER_OFF_EXPECTED_VECTOR(%r15)
	jne .Lterminal_pf_unclassified
	movq ER_OFF_FAULT_RIP(%r15), %rax
	cmpq ER_OFF_EXPECTED_RIP(%r15), %rax
	jne .Lterminal_pf_unclassified
	cmpq $0, ER_OFF_RECOVERY_COUNT(%r15)
	jne .Lterminal_pf_nested
	/* Consume the arm token before rewriting the frame; reentry is terminal. */
	movabsq $ER_HANDOFF_TOKEN, %rax
	movq %rax, ER_OFF_ARMED(%r15)
	leaq x86_er_recovery_stub(%rip), %rax
	movq %rax, 8(%r14)
	movq ER_OFF_RECOVERY_STACK_TOP(%r15), %rax
	movq %rax, 32(%r14)
	ER_POP_GPRS
	addq $8, %rsp /* Skip the error-code slot before IRETQ. */
	iretq

.Lterminal_pf_unarmed:
	movl $0x4552d00e, %eax
	jmp .Lterminal_common
.Lterminal_pf_unclassified:
	movl $0x4552e00e, %eax
	jmp .Lterminal_common
.Lterminal_pf_nested:
	movl $0x4552f00e, %eax
	jmp .Lterminal_common
SYM_CODE_END(x86_er_pf_entry)

/* #GP: out-of-limit GDT selector load; SGDT preflight is in the C wrapper. */
SYM_CODE_START(x86_er_gp_entry)
	ER_PUSH_GPRS
	movq er_active_state(%rip), %r15
	testq %r15, %r15
	jz .Lterminal_gp_unarmed
	leaq 120(%rsp), %r14
	movq $0, ER_OFF_CR2(%r15)
	movq $13, ER_OFF_CAPTURED_VECTOR(%r15)
	movq 0(%r14), %rax
	movq %rax, ER_OFF_ERROR_CODE(%r15)
	movq 8(%r14), %rax
	movq %rax, ER_OFF_FAULT_RIP(%r15)
	movq 16(%r14), %rax
	movq %rax, ER_OFF_FAULT_CS(%r15)
	movq 24(%r14), %rax
	movq %rax, ER_OFF_FAULT_RFLAGS(%r15)
	movq 32(%r14), %rax
	movq %rax, ER_OFF_FAULT_RSP(%r15)
	movq 40(%r14), %rax
	movq %rax, ER_OFF_FAULT_SS(%r15)
	movabsq $ER_ARM_TOKEN, %rax
	cmpq %rax, ER_OFF_ARMED(%r15)
	jne .Lterminal_gp_unclassified
	cmpq $13, ER_OFF_EXPECTED_VECTOR(%r15)
	jne .Lterminal_gp_unclassified
	movq ER_OFF_FAULT_RIP(%r15), %rax
	cmpq ER_OFF_EXPECTED_RIP(%r15), %rax
	jne .Lterminal_gp_unclassified
	cmpq $0, ER_OFF_RECOVERY_COUNT(%r15)
	jne .Lterminal_gp_nested
	/* Consume the arm token before rewriting the frame; reentry is terminal. */
	movabsq $ER_HANDOFF_TOKEN, %rax
	movq %rax, ER_OFF_ARMED(%r15)
	leaq x86_er_recovery_stub(%rip), %rax
	movq %rax, 8(%r14)
	movq ER_OFF_RECOVERY_STACK_TOP(%r15), %rax
	movq %rax, 32(%r14)
	ER_POP_GPRS
	addq $8, %rsp
	iretq

.Lterminal_gp_unarmed:
	movl $0x4552d00d, %eax
	jmp .Lterminal_common
.Lterminal_gp_unclassified:
	movl $0x4552e00d, %eax
	jmp .Lterminal_common
.Lterminal_gp_nested:
	movl $0x4552f00d, %eax
	jmp .Lterminal_common
SYM_CODE_END(x86_er_gp_entry)

/* The first recovery-stack instructions save scratch state before using GPRs. */
SYM_CODE_START(x86_er_recovery_stub)
	pushfq
	popq er_recovery_scratch+ER_SCRATCH_FLAGS(%rip)
	movq %rax, er_recovery_scratch+ER_SCRATCH_RAX(%rip)
	movq %rbx, er_recovery_scratch+ER_SCRATCH_RBX(%rip)
	movq %rcx, er_recovery_scratch+ER_SCRATCH_RCX(%rip)
	movq %rdx, er_recovery_scratch+ER_SCRATCH_RDX(%rip)
	movq %rsi, er_recovery_scratch+ER_SCRATCH_RSI(%rip)
	movq %rdi, er_recovery_scratch+ER_SCRATCH_RDI(%rip)
	movq %rbp, er_recovery_scratch+ER_SCRATCH_RBP(%rip)
	movq %r8, er_recovery_scratch+ER_SCRATCH_R8(%rip)
	movq %r9, er_recovery_scratch+ER_SCRATCH_R9(%rip)
	movq %r10, er_recovery_scratch+ER_SCRATCH_R10(%rip)
	movq %r11, er_recovery_scratch+ER_SCRATCH_R11(%rip)
	movq %r12, er_recovery_scratch+ER_SCRATCH_R12(%rip)
	movq %r13, er_recovery_scratch+ER_SCRATCH_R13(%rip)
	movq %r14, er_recovery_scratch+ER_SCRATCH_R14(%rip)
	movq %r15, er_recovery_scratch+ER_SCRATCH_R15(%rip)

	movq er_active_state(%rip), %r15
	testq %r15, %r15
	jz .Lterminal_recovery_state
	cmpq ER_OFF_RECOVERY_STACK_TOP(%r15), %rsp
	jne .Lterminal_recovery_state
	movabsq $ER_HANDOFF_TOKEN, %rax
	cmpq %rax, ER_OFF_ARMED(%r15)
	jne .Lterminal_recovery_state
	movq ER_OFF_EXPECTED_VECTOR(%r15), %rax
	cmpq ER_OFF_CAPTURED_VECTOR(%r15), %rax
	jne .Lterminal_recovery_state
	movq ER_OFF_EXPECTED_RIP(%r15), %rax
	cmpq ER_OFF_FAULT_RIP(%r15), %rax
	jne .Lterminal_recovery_state
	cmpq $0, ER_OFF_RECOVERY_COUNT(%r15)
	jne .Lterminal_recovery_state
	movq ER_OFF_EXPECTED_RAX(%r15), %rax
	cmpq %rax, er_recovery_scratch+ER_SCRATCH_RAX(%rip)
	jne .Lterminal_recovery_state
	ER_CHECK_SCRATCH ER_SENTINEL_RBX, ER_SCRATCH_RBX
	ER_CHECK_SCRATCH ER_SENTINEL_RCX, ER_SCRATCH_RCX
	ER_CHECK_SCRATCH ER_SENTINEL_RDX, ER_SCRATCH_RDX
	ER_CHECK_SCRATCH ER_SENTINEL_RSI, ER_SCRATCH_RSI
	ER_CHECK_SCRATCH ER_SENTINEL_RDI, ER_SCRATCH_RDI
	ER_CHECK_SCRATCH ER_SENTINEL_RBP, ER_SCRATCH_RBP
	ER_CHECK_SCRATCH ER_SENTINEL_R8, ER_SCRATCH_R8
	ER_CHECK_SCRATCH ER_SENTINEL_R9, ER_SCRATCH_R9
	ER_CHECK_SCRATCH ER_SENTINEL_R10, ER_SCRATCH_R10
	ER_CHECK_SCRATCH ER_SENTINEL_R11, ER_SCRATCH_R11
	ER_CHECK_SCRATCH ER_SENTINEL_R12, ER_SCRATCH_R12
	ER_CHECK_SCRATCH ER_SENTINEL_R13, ER_SCRATCH_R13
	ER_CHECK_SCRATCH ER_SENTINEL_R14, ER_SCRATCH_R14
	ER_CHECK_SCRATCH ER_SENTINEL_R15, ER_SCRATCH_R15
	cmpq $14, ER_OFF_EXPECTED_VECTOR(%r15)
	jne .Lrecovery_not_pf
	movq ER_OFF_FAULT_ADDRESS(%r15), %rax
	cmpq ER_OFF_CR2(%r15), %rax
	jne .Lterminal_recovery_state
	jmp .Lrecovery_metadata_checked
.Lrecovery_not_pf:
	cmpq $0, ER_OFF_CR2(%r15)
	jne .Lterminal_recovery_state
.Lrecovery_metadata_checked:

	/* Keep the task-stack return envelope in dedicated scratch before IDT work. */
	movq ER_OFF_ENVELOPE_RSP(%r15), %rax
	movq %rax, er_recovery_scratch+ER_SCRATCH_ENVELOPE_RSP(%rip)
	movq %rax, %rdx
	cmpq %rdx, ER_OFF_ENVELOPE_RSP(%r15)
	jne .Lterminal_recovery_state

	/* Check both stacks before disarming or leaving the shadow IDT active. */
	movq ER_OFF_FAULT_STACK_BASE(%r15), %rsi
	movabsq $0x45524c4f5743414e, %rax
	cmpq %rax, (%rsi)
	jne .Lterminal_recovery_state
	addq $ER_TEST_STACK_HIGH_CANARY, %rsi
	movabsq $0x4552484947484341, %rax
	cmpq %rax, (%rsi)
	jne .Lterminal_recovery_state
	movq ER_OFF_RECOVERY_STACK_BASE(%r15), %rsi
	movabsq $0x45524c4f5743414e, %rax
	cmpq %rax, (%rsi)
	jne .Lterminal_recovery_state
	addq $ER_TEST_STACK_HIGH_CANARY, %rsi
	movabsq $0x4552484947484341, %rax
	cmpq %rax, (%rsi)
	jne .Lterminal_recovery_state

	/* Compare live post-IRETQ RFLAGS before publishing any result. */
	movq ER_OFF_FAULT_RFLAGS(%r15), %rax
	xorq er_recovery_scratch+ER_SCRATCH_FLAGS(%rip), %rax
	movabsq $ER_RFLAGS_RETURNED_MASK, %rdx
	andq %rdx, %rax
	jnz .Lterminal_recovery_state

	/* Count only after the recovery entry and preserved state validate. */
	incq ER_OFF_RECOVERY_COUNT(%r15)

	/* Publish the captured architectural record before the single disarm write. */
	movq ER_OFF_CAPTURED_VECTOR(%r15), %rax
	movq %rax, ER_OFF_RESULT_VECTOR(%r15)
	movq ER_OFF_ERROR_CODE(%r15), %rax
	movq %rax, ER_OFF_RESULT_ERROR_CODE(%r15)
	movq ER_OFF_CR2(%r15), %rax
	movq %rax, ER_OFF_RESULT_CR2(%r15)
	movq ER_OFF_FAULT_RIP(%r15), %rax
	movq %rax, ER_OFF_RESULT_RIP(%r15)
	movq ER_OFF_FAULT_CS(%r15), %rax
	movq %rax, ER_OFF_RESULT_CS(%r15)
	movq er_recovery_scratch+ER_SCRATCH_FLAGS(%rip), %rax
	movq %rax, ER_OFF_RESULT_RFLAGS(%r15)
	movq ER_OFF_FAULT_RSP(%r15), %rax
	movq %rax, ER_OFF_RESULT_RSP(%r15)
	movq ER_OFF_FAULT_SS(%r15), %rax
	movq %rax, ER_OFF_RESULT_SS(%r15)
	movq $0, ER_OFF_ARMED(%r15)

	lidt ER_OFF_ORIGINAL_IDTR(%r15)
	sidt ER_OFF_VERIFY_IDTR(%r15)
	movzwq ER_OFF_ORIGINAL_IDTR+ER_IDTR_LIMIT_OFF(%r15), %rax
	cmpw ER_OFF_VERIFY_IDTR+ER_IDTR_LIMIT_OFF(%r15), %ax
	jne .Lterminal_recovery_idtr
	movq ER_OFF_ORIGINAL_IDTR+ER_IDTR_BASE_OFF(%r15), %rax
	cmpq ER_OFF_VERIFY_IDTR+ER_IDTR_BASE_OFF(%r15), %rax
	jne .Lterminal_recovery_idtr
	movq $3, ER_OFF_IDTR_PROOF(%r15)
	movq $0x7d, ER_OFF_RESULT_FLAGS(%r15)
	cmpq $14, ER_OFF_EXPECTED_VECTOR(%r15)
	jne .Lresult_flags_ready
	orq $0x2, ER_OFF_RESULT_FLAGS(%r15)
.Lresult_flags_ready:
	movq ER_OFF_ENVELOPE_RSP(%r15), %rax
	movq %rax, er_recovery_scratch+ER_SCRATCH_ENVELOPE_RSP(%rip)

	/* Restore fault-time RFLAGS/GPRs, then the saved Linux envelope RSP. */
	pushq er_recovery_scratch+ER_SCRATCH_FLAGS(%rip)
	popfq
	movq er_recovery_scratch+ER_SCRATCH_RAX(%rip), %rax
	movq er_recovery_scratch+ER_SCRATCH_RBX(%rip), %rbx
	movq er_recovery_scratch+ER_SCRATCH_RCX(%rip), %rcx
	movq er_recovery_scratch+ER_SCRATCH_RDX(%rip), %rdx
	movq er_recovery_scratch+ER_SCRATCH_RSI(%rip), %rsi
	movq er_recovery_scratch+ER_SCRATCH_RDI(%rip), %rdi
	movq er_recovery_scratch+ER_SCRATCH_RBP(%rip), %rbp
	movq er_recovery_scratch+ER_SCRATCH_R8(%rip), %r8
	movq er_recovery_scratch+ER_SCRATCH_R9(%rip), %r9
	movq er_recovery_scratch+ER_SCRATCH_R10(%rip), %r10
	movq er_recovery_scratch+ER_SCRATCH_R11(%rip), %r11
	movq er_recovery_scratch+ER_SCRATCH_R12(%rip), %r12
	movq er_recovery_scratch+ER_SCRATCH_R13(%rip), %r13
	movq er_recovery_scratch+ER_SCRATCH_R14(%rip), %r14
	movq er_recovery_scratch+ER_SCRATCH_R15(%rip), %r15
	movq er_recovery_scratch+ER_SCRATCH_ENVELOPE_RSP(%rip), %rsp
	jmp .Lpost_injection

.Lterminal_recovery_state:
	movl $0x4552f001, %eax
	jmp .Lterminal_common
.Lterminal_recovery_idtr:
	movl $0x4552ff02, %eax
	jmp .Lterminal_common
SYM_CODE_END(x86_er_recovery_stub)

/* Terminal path for handler mismatches/failures while the shadow IDT is live. */
.Lterminal_common:
	cli
	movq er_active_state(%rip), %r11
	testq %r11, %r11
	jz 1f
	movq $0, ER_OFF_RECOVERY_COUNT(%r11)
	movq %rax, ER_OFF_TERMINAL_CODE(%r11)
1:
	movw $0xf4, %dx
	outl %eax, %dx
2:
	hlt
	jmp 2b

.macro ER_TERMINAL_VECTOR vector
SYM_CODE_START(x86_er_terminal_\vector)
	movl $(ER_TERMINAL_CODE_BASE + \vector), %eax
	jmp .Lterminal_common
SYM_CODE_END(x86_er_terminal_\vector)
.endm

ER_TERMINAL_VECTOR 0
ER_TERMINAL_VECTOR 1
ER_TERMINAL_VECTOR 3
ER_TERMINAL_VECTOR 4
ER_TERMINAL_VECTOR 5
ER_TERMINAL_VECTOR 6
ER_TERMINAL_VECTOR 7
ER_TERMINAL_VECTOR 8
ER_TERMINAL_VECTOR 9
ER_TERMINAL_VECTOR 10
ER_TERMINAL_VECTOR 11
ER_TERMINAL_VECTOR 12
ER_TERMINAL_VECTOR 15
ER_TERMINAL_VECTOR 16
ER_TERMINAL_VECTOR 17
ER_TERMINAL_VECTOR 18
ER_TERMINAL_VECTOR 19
ER_TERMINAL_VECTOR 20
ER_TERMINAL_VECTOR 21
ER_TERMINAL_VECTOR 22
ER_TERMINAL_VECTOR 23
ER_TERMINAL_VECTOR 24
ER_TERMINAL_VECTOR 25
ER_TERMINAL_VECTOR 26
ER_TERMINAL_VECTOR 27
ER_TERMINAL_VECTOR 28
ER_TERMINAL_VECTOR 29
ER_TERMINAL_VECTOR 30
ER_TERMINAL_VECTOR 31

.section .bss
.balign 8
.globl er_recovery_scratch
er_recovery_scratch:
	.zero 136
