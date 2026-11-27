/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>
#include <asm/ibt.h>

#define NMI_PROBE_PHASE_ARMED 2
#define NMI_PROBE_READY_BYTE 0x4e

	.text
	.p2align 4
SYM_FUNC_START(x86_nmi_probe_wait)
	ENDBR
	pushq %rbp
	movq %rsp, %rbp
	pushq %rbx
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	movq %rsp, x86_nmi_probe_expected_rsp(%rip)
	movq %rbp, x86_nmi_probe_expected_rbp(%rip)

	movq $0x000000000000004e, %rax
	movq $0x0000000011111111, %rbx
	xorl %ecx, %ecx
	movq $0x0000000022222222, %rdx
	movq $0x0000000044444444, %rdi
	movq $0x0000000033333333, %rsi
	movq $0x0000000066666666, %r8
	movq $0x0000000077777777, %r9
	movq $0x0000000012345678, %r10
	movq $0x0000000023456789, %r11
	movq $0x000000003456789a, %r12
	movq $0x00000000456789ab, %r13
	movq $0x0000000056789abc, %r14
	movq $0x000000006789abcd, %r15
	pushq $0x46
	popfq

	.globl x86_nmi_probe_window_begin
x86_nmi_probe_window_begin:
	movl $NMI_PROBE_PHASE_ARMED, x86_nmi_probe_phase(%rip)
	outb %al, $0xe9
.Lwait_for_nmi:
	movl x86_nmi_probe_done(%rip), %ecx
	jrcxz .Lspin
	jmp .Lwindow_done
.Lspin:
	pause
	jmp .Lwait_for_nmi
.Lwindow_done:
	.globl x86_nmi_probe_window_end
x86_nmi_probe_window_end:
	pushfq
	popq x86_nmi_probe_return_rflags(%rip)

	movl $0, x86_nmi_probe_return_gpr_mask(%rip)
	cmpq $0x4e, %rax
	jne .Lgpr_rax_done
	orl $0x0001, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_rax_done:
	cmpq $0x11111111, %rbx
	jne .Lgpr_rbx_done
	orl $0x0002, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_rbx_done:
	cmpq x86_nmi_probe_expected_rbp(%rip), %rbp
	jne .Lgpr_rbp_done
	orl $0x0004, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_rbp_done:
	cmpq $1, %rcx
	jne .Lgpr_rcx_done
	orl $0x0008, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_rcx_done:
	cmpq $0x22222222, %rdx
	jne .Lgpr_rdx_done
	orl $0x0010, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_rdx_done:
	cmpq $0x44444444, %rdi
	jne .Lgpr_rdi_done
	orl $0x0020, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_rdi_done:
	cmpq $0x33333333, %rsi
	jne .Lgpr_rsi_done
	orl $0x0040, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_rsi_done:
	cmpq $0x66666666, %r8
	jne .Lgpr_r8_done
	orl $0x0080, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_r8_done:
	cmpq $0x77777777, %r9
	jne .Lgpr_r9_done
	orl $0x0100, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_r9_done:
	cmpq $0x12345678, %r10
	jne .Lgpr_r10_done
	orl $0x0200, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_r10_done:
	cmpq $0x23456789, %r11
	jne .Lgpr_r11_done
	orl $0x0400, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_r11_done:
	cmpq $0x3456789a, %r12
	jne .Lgpr_r12_done
	orl $0x0800, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_r12_done:
	cmpq $0x456789ab, %r13
	jne .Lgpr_r13_done
	orl $0x1000, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_r13_done:
	cmpq $0x56789abc, %r14
	jne .Lgpr_r14_done
	orl $0x2000, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_r14_done:
	cmpq $0x6789abcd, %r15
	jne .Lgpr_r15_done
	orl $0x4000, x86_nmi_probe_return_gpr_mask(%rip)
.Lgpr_r15_done:

	cmpq x86_nmi_probe_expected_rsp(%rip), %rsp
	sete %al
	movzbl %al, %eax
	movl %eax, x86_nmi_probe_return_rsp_ok(%rip)

	cmpq $0x46, x86_nmi_probe_return_rflags(%rip)
	sete %al
	movzbl %al, %eax
	movl %eax, x86_nmi_probe_return_rflags_ok(%rip)

	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
	RET
SYM_FUNC_END(x86_nmi_probe_wait)
