/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
#ifndef X86_EXCEPTION_RECOVERY_UAPI_H
#define X86_EXCEPTION_RECOVERY_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define X86_EXCEPTION_RECOVERY_PF 14U
#define X86_EXCEPTION_RECOVERY_GP 13U

#define X86_EXCEPTION_RECOVERY_RIP_MATCHED       (1U << 0)
#define X86_EXCEPTION_RECOVERY_CR2_MATCHED       (1U << 1)
#define X86_EXCEPTION_RECOVERY_IDTR_RESTORED     (1U << 2)
#define X86_EXCEPTION_RECOVERY_ONCE              (1U << 3)
#define X86_EXCEPTION_RECOVERY_STACKS_INTACT     (1U << 4)
#define X86_EXCEPTION_RECOVERY_CPU_MATCHED       (1U << 5)

struct x86_exception_recovery_result {
	__u32 cpu;
	__u32 vector;
	__u32 flags;
	__u32 recovery_count;
	__u64 error_code;
	__u64 fault_rip;
	__u64 cr2;
};

struct x86_exception_recovery_request {
	__u32 cpu;
	__u32 vector;
	struct x86_exception_recovery_result result;
};

#define X86_EXCEPTION_RECOVERY_RUN \
	_IOWR(0xb8, 1, struct x86_exception_recovery_request)

#endif
