/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/ibt.h>

#include "cr3_switch_layout.h"

.text
/* rdi: private CR3, rsi: private alias, edx: abort, rcx: result buffer */
SYM_FUNC_START(x86_cr3_switch_once)
	ENDBR
	/* Discard any local translation of the otherwise absent alias. */
	invlpg (%rsi)
	movq %cr3, %r8
	movq %rdi, %cr3
	movq %cr3, %r9

	/* During the switch touch only the private, prevalidated alias. */
	xorl %r10d, %r10d
	movl $-EIO, %eax
	cmpq %rdi, %r9
	jne .Lrestore
	movq (%rsi), %r10
	xorl %eax, %eax
	testb %dl, %dl
	je .Lrestore
	movl $-ECANCELED, %eax
.Lrestore:
	movq %r8, %cr3
	movq %cr3, %r11
	movq %r8, CR3_SWITCH_BEFORE(%rcx)
	movq %r9, CR3_SWITCH_ACTIVE(%rcx)
	movq %r11, CR3_SWITCH_AFTER(%rcx)
	movq %r10, CR3_SWITCH_VALUE(%rcx)
	RET
SYM_FUNC_END(x86_cr3_switch_once)
