/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef X86_NMI_BOUNDARY_UAPI_H
#define X86_NMI_BOUNDARY_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define X86_NMI_BOUNDARY_VERSION 1U
#define X86_NMI_BOUNDARY_GPR_ALL 0x7fffU
#define X86_NMI_BOUNDARY_TYPE_LOCAL 0U
#define X86_NMI_BOUNDARY_EXPECTED_RFLAGS 0x46ULL
#define X86_NMI_BOUNDARY_RFLAGS_MASK 0x24ffffULL

#define X86_NMI_BOUNDARY_FAIL_TYPE          (1U << 0)
#define X86_NMI_BOUNDARY_FAIL_CPU           (1U << 1)
#define X86_NMI_BOUNDARY_FAIL_RIP           (1U << 2)
#define X86_NMI_BOUNDARY_FAIL_GPRS          (1U << 3)
#define X86_NMI_BOUNDARY_FAIL_RSP           (1U << 4)
#define X86_NMI_BOUNDARY_FAIL_RFLAGS        (1U << 5)
#define X86_NMI_BOUNDARY_FAIL_IDTR          (1U << 6)
#define X86_NMI_BOUNDARY_FAIL_COUNT         (1U << 7)
#define X86_NMI_BOUNDARY_FAIL_DUPLICATE     (1U << 8)
#define X86_NMI_BOUNDARY_FAIL_UNEXPECTED    (1U << 9)
#define X86_NMI_BOUNDARY_FAIL_SYSCALL       (1U << 10)
#define X86_NMI_BOUNDARY_FAIL_FRED          (1U << 11)
#define X86_NMI_BOUNDARY_FAIL_ENVIRONMENT   (1U << 12)
#define X86_NMI_BOUNDARY_FAIL_HOST_STATE    (1U << 13)

struct x86_nmi_boundary_result {
	__u32 size;
	__u32 version;
	__u32 failure_flags;
	__u32 nmi_type;
	__s32 cpu_before;
	__s32 cpu_nmi;
	__s32 cpu_after;
	__s32 post_syscall_cpu;
	__s32 post_syscall_pid;
	__s32 task_pid_before;
	__s32 task_pid_nmi;
	__s32 task_pid_after;
	__u32 nmi_count;
	__u32 duplicate_count;
	__u32 unexpected_count;
	__u32 callback_gpr_mask;
	__u32 return_gpr_mask;
	__u32 return_rsp_ok;
	__u32 return_rflags_ok;
	__u64 nmi_ip;
	__u64 nmi_rsp;
	__u64 nmi_rflags;
	__u64 expected_rsp;
	__u64 idtr_before_base;
	__u64 idtr_nmi_base;
	__u64 idtr_after_base;
	__u16 idtr_before_limit;
	__u16 idtr_nmi_limit;
	__u16 idtr_after_limit;
	__u16 reserved;
	__u64 cr3_before;
	__u64 cr3_nmi;
	__u64 cr3_after;
};

#define X86_NMI_BOUNDARY_IOC_MAGIC 0xb7
#define X86_NMI_BOUNDARY_IOC_RUN \
	_IOR(X86_NMI_BOUNDARY_IOC_MAGIC, 1, struct x86_nmi_boundary_result)
#define X86_NMI_BOUNDARY_IOC_FINALIZE \
	_IOWR(X86_NMI_BOUNDARY_IOC_MAGIC, 2, struct x86_nmi_boundary_result)

#endif /* X86_NMI_BOUNDARY_UAPI_H */
