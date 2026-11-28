/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>

/* Called only inside a kernel FPU section with KFPU_387 | KFPU_MXCSR. */
SYM_FUNC_START(x86_boundary_probe_clobber_xstate)
	fldz
	ldmxcsr .Lprobe_mxcsr(%rip)
	vxorps %ymm0, %ymm0, %ymm0
	vxorps %ymm1, %ymm1, %ymm1
	vxorps %ymm2, %ymm2, %ymm2
	vxorps %ymm3, %ymm3, %ymm3
	vxorps %ymm4, %ymm4, %ymm4
	vxorps %ymm5, %ymm5, %ymm5
	vxorps %ymm6, %ymm6, %ymm6
	vxorps %ymm7, %ymm7, %ymm7
	RET
SYM_FUNC_END(x86_boundary_probe_clobber_xstate)

.pushsection .rodata
.p2align 2
.Lprobe_mxcsr:
	.long 0x5f80	/* Round up with exceptions masked. */
.popsection
