// SPDX-License-Identifier: GPL-2.0-only
/* Userspace half of the disposable x86_64 boundary probe. */
#define _GNU_SOURCE
#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "boundary_probe_uapi.h"

static const uint32_t vectors[8][8] __attribute__((aligned(32))) = {
	{0x00000001, 0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66666666, 0x77777777},
	{0x88888888, 0x99999999, 0xaaaaaaaa, 0xbbbbbbbb, 0xcccccccc, 0xdddddddd, 0xeeeeeeee, 0xffffffff},
	{0x01020304, 0x11223344, 0x55667788, 0x90abcdef, 0xfedcba09, 0x87654321, 0x44332211, 0x04030201},
	{0x13579bdf, 0x2468ace0, 0x0f1e2d3c, 0x4b5a6978, 0x8796a5b4, 0xc3d2e1f0, 0x55aa55aa, 0xaa55aa55},
	{0xdeadbeef, 0xcafebabe, 0x01234567, 0x89abcdef, 0xf0e1d2c3, 0xb4a59687, 0x10203040, 0x50607080},
	{0xabcdef01, 0x23456789, 0x76543210, 0xfedcba98, 0x31415926, 0x53589793, 0x23846264, 0x33832795},
	{0xaaaaaaaa, 0x55555555, 0xaaaaaaaa, 0x55555555, 0xaaaaaaaa, 0x55555555, 0xaaaaaaaa, 0x55555555},
	{0x00000000, 0xffffffff, 0x00000000, 0xffffffff, 0x00000000, 0xffffffff, 0x00000000, 0xffffffff},
};

_Static_assert(X86_BOUNDARY_PROBE_RUN == _IOW(0xb7, 1, struct x86_boundary_probe_request),
	       "userspace ioctl number disagrees with kernel UAPI");

static long invoke_with_xstate(int fd, const struct x86_boundary_probe_request *request,
			       uint32_t after[8][8])
{
	register long rax __asm__("rax") = SYS_ioctl;
	register long rdi __asm__("rdi") = fd;
	register long rsi __asm__("rsi") = X86_BOUNDARY_PROBE_RUN;
	register const struct x86_boundary_probe_request *rdx __asm__("rdx") = request;

	/* The raw syscall preserves RCX/R11 as required by the syscall ABI and lets us
	 * inspect the live YMM registers before any userspace C code can use them. */
	__asm__ volatile(
		"vmovdqu %[v0], %%ymm0\n\t"
		"vmovdqu %[v1], %%ymm1\n\t"
		"vmovdqu %[v2], %%ymm2\n\t"
		"vmovdqu %[v3], %%ymm3\n\t"
		"vmovdqu %[v4], %%ymm4\n\t"
		"vmovdqu %[v5], %%ymm5\n\t"
		"vmovdqu %[v6], %%ymm6\n\t"
		"vmovdqu %[v7], %%ymm7\n\t"
		"syscall\n\t"
		"vmovdqu %%ymm0, %[o0]\n\t"
		"vmovdqu %%ymm1, %[o1]\n\t"
		"vmovdqu %%ymm2, %[o2]\n\t"
		"vmovdqu %%ymm3, %[o3]\n\t"
		"vmovdqu %%ymm4, %[o4]\n\t"
		"vmovdqu %%ymm5, %[o5]\n\t"
		"vmovdqu %%ymm6, %[o6]\n\t"
		"vmovdqu %%ymm7, %[o7]\n\t"
		: "+a"(rax), "+D"(rdi), "+S"(rsi), "+d"(rdx),
		  [o0] "=m"(after[0]), [o1] "=m"(after[1]),
		  [o2] "=m"(after[2]), [o3] "=m"(after[3]),
		  [o4] "=m"(after[4]), [o5] "=m"(after[5]),
		  [o6] "=m"(after[6]), [o7] "=m"(after[7])
		: [v0] "m"(vectors[0]), [v1] "m"(vectors[1]),
		  [v2] "m"(vectors[2]), [v3] "m"(vectors[3]),
		  [v4] "m"(vectors[4]), [v5] "m"(vectors[5]),
		  [v6] "m"(vectors[6]), [v7] "m"(vectors[7])
		: "cc", "rcx", "r11", "memory", "ymm0", "ymm1", "ymm2", "ymm3",
		  "ymm4", "ymm5", "ymm6", "ymm7");
	return rax;
}

static int check_xsave_avx(void)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned int xcr0_low, xcr0_high;

	if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) ||
	    !(ecx & (1U << 26)) || !(ecx & (1U << 27)) || !(ecx & (1U << 28))) {
		fprintf(stderr, "CPU does not expose XSAVE, OSXSAVE and AVX\n");
		return -1;
	}
	__asm__ volatile("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0));
	if ((xcr0_low & 0x6) != 0x6) {
		fprintf(stderr, "OS has not enabled XMM+YMM state in XCR0 (0x%x:%x)\n",
			xcr0_high, xcr0_low);
		return -1;
	}
	return 0;
}

static int check_linux_syscalls(pid_t expected_pid)
{
	struct timespec now;
	long pid = syscall(SYS_getpid);
	long uid = syscall(SYS_getuid);

	if (pid != expected_pid || uid < 0 || syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &now)) {
		perror("Linux syscall health check failed");
		return -1;
	}
	return 0;
}

static int run_probe_ioctl(int fd, pid_t pid, unsigned int cpu, unsigned int mode)
{
	struct x86_boundary_probe_request request = {.cpu = cpu, .mode = mode};
	uint32_t after[8][8] __attribute__((aligned(32)));
	long result;
	long expected = mode == X86_BOUNDARY_PROBE_MODE_RETURN ? 0 : -ECANCELED;

	if (check_linux_syscalls(pid))
		return -1;
	result = invoke_with_xstate(fd, &request, after);
	if (result != expected) {
		fprintf(stderr, "cpu %u mode %u returned %ld, expected %ld\n",
			cpu, mode, result, expected);
		return -1;
	}
	if (memcmp(vectors, after, sizeof(vectors))) {
		fprintf(stderr, "cpu %u mode %u changed userspace XSAVE/YMM state\n", cpu, mode);
		return -1;
	}
	if (check_linux_syscalls(pid))
		return -1;
	return 0;
}

int main(void)
{
	const pid_t pid = getpid();
	long cpu_count;
	int fd, cpu;

	if (check_xsave_avx())
		return 1;
	cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
	if (cpu_count < 1) {
		fprintf(stderr, "could not determine online CPU count\n");
		return 1;
	}
	fd = open("/dev/x86-boundary-probe", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open /dev/x86-boundary-probe");
		return 1;
	}
	errno = 0;
	if (lseek(fd, 0, SEEK_SET) != -1 || errno != ESPIPE) {
		fprintf(stderr, "probe device unexpectedly supports lseek\n");
		return 1;
	}
	if (check_linux_syscalls(pid))
		return 1;
	for (cpu = 0; cpu < cpu_count; cpu++) {
		if (run_probe_ioctl(fd, pid, cpu, X86_BOUNDARY_PROBE_MODE_RETURN) ||
		    run_probe_ioctl(fd, pid, cpu, X86_BOUNDARY_PROBE_MODE_ABORT))
			return 1;
	}
	if (close(fd)) {
		perror("close probe device");
		return 1;
	}
	printf("x86 boundary probe passed: %ld CPUs, return/abort, host state, XSAVE/YMM, Linux syscalls\n",
	       cpu_count);
	return 0;
}
