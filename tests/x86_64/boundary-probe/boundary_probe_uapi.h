/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef X86_BOUNDARY_PROBE_UAPI_H
#define X86_BOUNDARY_PROBE_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct x86_boundary_probe_request {
	__u32 cpu;
	__u32 mode;
};

struct x86_boundary_continuation_request {
	__u32 owner_cpu;
	__u32 responder_cpu;
};

#define X86_BOUNDARY_PROBE_MODE_RETURN 0
#define X86_BOUNDARY_PROBE_MODE_ABORT  1
#define X86_BOUNDARY_PROBE_RUN _IOW(0xb7, 1, struct x86_boundary_probe_request)
#define X86_BOUNDARY_PROBE_TASK_CONTINUATION \
	_IOW(0xb7, 2, struct x86_boundary_continuation_request)

#endif
