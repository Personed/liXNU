/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>

/* Called only between kernel_fpu_begin() and kernel_fpu_end(). */
SYM_FUNC_START(x86_boundary_probe_clobber_xstate)
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
