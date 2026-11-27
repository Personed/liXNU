// SPDX-License-Identifier: GPL-2.0-only
/* Userspace verifier for the disposable-VM-only #PF/#GP recovery probe. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "exception_recovery_uapi.h"

_Static_assert(X86_EXCEPTION_RECOVERY_RUN ==
	       _IOWR(0xb8, 1, struct x86_exception_recovery_request),
	       "userspace ioctl number disagrees with kernel UAPI");

static int check_linux_syscalls(pid_t expected_pid)
{
	struct timespec now;

	if (syscall(SYS_getpid) != expected_pid ||
	    syscall(SYS_getuid) < 0 ||
	    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now)) {
		perror("Linux syscall health check failed");
		return -1;
	}
	return 0;
}

static int run_case(int fd, unsigned int cpu, unsigned int vector)
{
	struct x86_exception_recovery_request request = {
		.cpu = cpu,
		.vector = vector,
	};
	uint32_t expected_flags = X86_EXCEPTION_RECOVERY_RIP_MATCHED |
		X86_EXCEPTION_RECOVERY_IDTR_RESTORED |
		X86_EXCEPTION_RECOVERY_ONCE |
		X86_EXCEPTION_RECOVERY_STACKS_INTACT |
		X86_EXCEPTION_RECOVERY_CPU_MATCHED;
	struct x86_exception_recovery_result *result = &request.result;

	if (vector == X86_EXCEPTION_RECOVERY_PF)
		expected_flags |= X86_EXCEPTION_RECOVERY_CR2_MATCHED;
	if (ioctl(fd, X86_EXCEPTION_RECOVERY_RUN, &request)) {
		fprintf(stderr, "ioctl for vector %u failed: %s\n", vector,
			strerror(errno));
		return -1;
	}
	if (result->cpu != cpu || result->vector != vector ||
	    result->flags != expected_flags || result->recovery_count != 1 ||
	    result->error_code != 0 || !result->fault_rip ||
	    (vector == X86_EXCEPTION_RECOVERY_PF && !result->cr2) ||
	    (vector == X86_EXCEPTION_RECOVERY_GP && result->cr2 != 0)) {
		fprintf(stderr,
			"vector %u metadata mismatch: cpu=%u vector=%u rip=%#" PRIx64
			" error=%#" PRIx64 " cr2=%#" PRIx64
			" recovery_count=%u flags=%#x expected_flags=%#x\n",
			vector, result->cpu, result->vector, (uint64_t)result->fault_rip,
			(uint64_t)result->error_code, (uint64_t)result->cr2, result->recovery_count,
			result->flags, expected_flags);
		return -1;
	}
	printf("X86_EXCEPTION_RECOVERY_CASE cpu=%u vector=%u rip=%#" PRIx64
	       " error=%#" PRIx64 " cr2=%#" PRIx64
	       " recovery_count=%u flags=%#x\n",
	       result->cpu, result->vector, (uint64_t)result->fault_rip,
	       (uint64_t)result->error_code, (uint64_t)result->cr2,
	       result->recovery_count,
	       result->flags);
	return 0;
}

int main(int argc, char **argv)
{
	const pid_t pid = getpid();
	char *end;
	unsigned long cpu = 0;
	int fd;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [CPU]\n", argv[0]);
		return 2;
	}
	if (argc == 2) {
		errno = 0;
		cpu = strtoul(argv[1], &end, 10);
		if (errno || !argv[1][0] || *end || cpu > UINT32_MAX) {
			fprintf(stderr, "invalid CPU id: %s\n", argv[1]);
			return 2;
		}
	}
	if (check_linux_syscalls(pid))
		return 1;
	fd = open("/dev/x86-exception-recovery", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open /dev/x86-exception-recovery");
		return 1;
	}
	if (run_case(fd, (unsigned int)cpu, X86_EXCEPTION_RECOVERY_PF) ||
	    check_linux_syscalls(pid) ||
	    run_case(fd, (unsigned int)cpu, X86_EXCEPTION_RECOVERY_GP) ||
	    check_linux_syscalls(pid)) {
		close(fd);
		return 1;
	}
	if (close(fd)) {
		perror("close exception recovery device");
		return 1;
	}
	printf("X86_EXCEPTION_RECOVERY_PASS cpu=%lu pf=passed gp=passed syscalls=passed\n",
	       cpu);
	return 0;
}
