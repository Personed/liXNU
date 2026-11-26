/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/linkage.h>

/* Called only between kernel_fpu_begin() and kernel_fpu_end(). */
SYM_FUNC_START(x86_boundary_probe_clobber_xstate)
	vpcmpeqd %ymm0, %ymm0, %ymm0
	vpcmpeqd %ymm1, %ymm1, %ymm1
	vpcmpeqd %ymm2, %ymm2, %ymm2
	vpcmpeqd %ymm3, %ymm3, %ymm3
	vpcmpeqd %ymm4, %ymm4, %ymm4
	vpcmpeqd %ymm5, %ymm5, %ymm5
	vpcmpeqd %ymm6, %ymm6, %ymm6
	vpcmpeqd %ymm7, %ymm7, %ymm7
	RET
SYM_FUNC_END(x86_boundary_probe_clobber_xstate)
