// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "nmi_boundary_uapi.h"

#define DEVICE_PATH "/dev/x86-nmi-boundary"

static int report_failure(const char *message)
{
	fprintf(stderr, "x86-nmi-boundary: %s\n", message);
	return EXIT_FAILURE;
}

static void print_result(const struct x86_nmi_boundary_result *result)
{
	printf("NMI_BOUNDARY_RESULT flags=0x%x type=%u count=%u duplicate=%u unexpected=%u "
	       "cpu=%d/%d/%d ip=0x%llx rsp=0x%llx rflags=0x%llx "
	       "gpr=0x%x/0x%x idtr=0x%llx:%u/0x%llx:%u/0x%llx:%u "
	       "task=%d/%d/%d cr3=0x%llx/0x%llx/0x%llx syscall=%d@%d\n",
	       result->failure_flags, result->nmi_type, result->nmi_count,
	       result->duplicate_count, result->unexpected_count,
	       result->cpu_before, result->cpu_nmi, result->cpu_after,
	       (unsigned long long)result->nmi_ip,
	       (unsigned long long)result->nmi_rsp,
	       (unsigned long long)result->nmi_rflags,
	       result->callback_gpr_mask, result->return_gpr_mask,
	       (unsigned long long)result->idtr_before_base,
	       result->idtr_before_limit,
	       (unsigned long long)result->idtr_nmi_base,
	       result->idtr_nmi_limit,
	       (unsigned long long)result->idtr_after_base,
	       result->idtr_after_limit,
	       result->task_pid_before, result->task_pid_nmi, result->task_pid_after,
	       (unsigned long long)result->cr3_before,
	       (unsigned long long)result->cr3_nmi,
	       (unsigned long long)result->cr3_after,
	       result->post_syscall_pid, result->post_syscall_cpu);
}

static int validate_result(const struct x86_nmi_boundary_result *result,
			   int check_syscall)
{
	if (result->size != sizeof(*result) ||
	    result->version != X86_NMI_BOUNDARY_VERSION)
		return report_failure("kernel/user ABI size or version mismatch");
	if (result->failure_flags)
		return report_failure("kernel reported a failed NMI boundary oracle");
	if (result->nmi_type != X86_NMI_BOUNDARY_TYPE_LOCAL ||
	    result->nmi_count != 1 || result->duplicate_count != 0 ||
	    result->unexpected_count != 0)
		return report_failure("NMI_LOCAL callback count or type was unexpected");
	if (result->cpu_before != 0 || result->cpu_nmi != 0 || result->cpu_after != 0)
		return report_failure("NMI left the single-vCPU CPU 0 profile");
	if (result->task_pid_before <= 0 ||
	    result->task_pid_nmi != result->task_pid_before ||
	    result->task_pid_after != result->task_pid_before ||
	    result->cr3_before != result->cr3_nmi ||
	    result->cr3_before != result->cr3_after)
		return report_failure("Linux task or CR3 host state changed across NMI return");
	if (!result->nmi_ip || !result->expected_rsp ||
	    result->nmi_rsp != result->expected_rsp)
		return report_failure("NMI RIP/RSP did not match the active shim window");
	if (result->callback_gpr_mask != X86_NMI_BOUNDARY_GPR_ALL ||
	    result->return_gpr_mask != X86_NMI_BOUNDARY_GPR_ALL)
		return report_failure("GPR canaries were not preserved through NMI return");
	if ((result->nmi_rflags & X86_NMI_BOUNDARY_RFLAGS_MASK) !=
	    X86_NMI_BOUNDARY_EXPECTED_RFLAGS ||
	    !result->return_rflags_ok || !result->return_rsp_ok)
		return report_failure("RFLAGS/RSP were not preserved through NMI return");
	if (result->idtr_before_base != result->idtr_nmi_base ||
	    result->idtr_before_base != result->idtr_after_base ||
	    result->idtr_before_limit != result->idtr_nmi_limit ||
	    result->idtr_before_limit != result->idtr_after_limit)
		return report_failure("Linux IDTR changed during the NMI test");
	if (check_syscall &&
	    (result->post_syscall_pid <= 0 || result->post_syscall_cpu != 0))
		return report_failure("post-NMI syscall/CPU check failed");
	return 0;
}

int main(void)
{
	cpu_set_t cpus;
	struct x86_nmi_boundary_result result;
	int fd;
	int failed = 0;
	long pid;
	int cpu;
	int ret;

	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	if (sched_setaffinity(0, sizeof(cpus), &cpus) != 0)
		return report_failure("could not pin the test task to CPU 0");
	if (sched_getcpu() != 0)
		return report_failure("task did not start on CPU 0");

	fd = open(DEVICE_PATH, O_RDONLY);
	if (fd < 0) {
		perror("open " DEVICE_PATH);
		return EXIT_FAILURE;
	}

	memset(&result, 0, sizeof(result));
	ret = ioctl(fd, X86_NMI_BOUNDARY_IOC_RUN, &result);
	if (ret != 0) {
		perror("NMI boundary run ioctl");
		failed = 1;
	}
	print_result(&result);
	if (validate_result(&result, 0) != 0)
		failed = 1;

	pid = syscall(SYS_getpid);
	cpu = sched_getcpu();
	if (pid <= 0 || cpu != 0) {
		fprintf(stderr, "x86-nmi-boundary: post-recovery getpid/cpu failed (%ld/%d)\n",
			pid, cpu);
		failed = 1;
	}
	result.post_syscall_pid = pid > 0 ? (__s32)pid : -1;
	result.post_syscall_cpu = cpu;
	ret = ioctl(fd, X86_NMI_BOUNDARY_IOC_FINALIZE, &result);
	if (ret != 0) {
		perror("NMI boundary finalize ioctl");
		failed = 1;
	}
	print_result(&result);
	if (validate_result(&result, 1) != 0)
		failed = 1;

	if (syscall(SYS_getpid) <= 0 || sched_getcpu() != 0) {
		fprintf(stderr, "x86-nmi-boundary: final post-cleanup syscall/cpu failed\n");
		failed = 1;
	}
	close(fd);
	if (failed)
		return EXIT_FAILURE;
	puts("X86_NMI_BOUNDARY_USER_PASS");
	return EXIT_SUCCESS;
}
