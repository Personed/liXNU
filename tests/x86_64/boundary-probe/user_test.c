// SPDX-License-Identifier: GPL-2.0-only
/* Userspace half of the disposable x86_64 boundary probe. */
#define _GNU_SOURCE
#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "boundary_probe_uapi.h"

#define LEGACY_XSTATE_SIZE 512U

/* Round down with exceptions masked, distinct from the kernel's sentinels. */
static const uint16_t x87_control = 0x077f;
static const uint32_t mxcsr_control = 0x3f80;

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

struct xsave_config {
	size_t size;
	uint64_t mask;
};

_Static_assert(X86_BOUNDARY_PROBE_RUN == _IOW(0xb7, 1, struct x86_boundary_probe_request),
	       "userspace ioctl number disagrees with kernel UAPI");

static long invoke_with_xstate(int fd, const struct x86_boundary_probe_request *request,
			       uint32_t after[8][8], void *xstate_before,
			       void *xstate_after, const struct xsave_config *config)
{
	register long rax __asm__("rax") = SYS_ioctl;
	register long rdi __asm__("rdi") = fd;
	register long rsi __asm__("rsi") = X86_BOUNDARY_PROBE_RUN;
	register const struct x86_boundary_probe_request *rdx __asm__("rdx") = request;
	const uint32_t mask_low = (uint32_t)config->mask;
	const uint32_t mask_high = (uint32_t)(config->mask >> 32);
	unsigned char original_legacy[LEGACY_XSTATE_SIZE] __attribute__((aligned(16)));
	long syscall_result;

	/* Capture the seeded state around the raw syscall before C can use it.
	 * Restore the original legacy FPU state before returning to C. */
	__asm__ volatile(
			"fxsave64 %[original_legacy]\n\t"
			"fninit\n\t"
			"fldcw %[x87_control]\n\t"
			"fld1\n\t"
			"ldmxcsr %[mxcsr_control]\n\t"
		"vmovdqu 0(%[vectors]), %%ymm0\n\t"
		"vmovdqu 32(%[vectors]), %%ymm1\n\t"
		"vmovdqu 64(%[vectors]), %%ymm2\n\t"
		"vmovdqu 96(%[vectors]), %%ymm3\n\t"
		"vmovdqu 128(%[vectors]), %%ymm4\n\t"
		"vmovdqu 160(%[vectors]), %%ymm5\n\t"
		"vmovdqu 192(%[vectors]), %%ymm6\n\t"
		"vmovdqu 224(%[vectors]), %%ymm7\n\t"
		"movl %[mask_low], %%eax\n\t"
		"movl %[mask_high], %%edx\n\t"
		"xsave64 (%[xstate_before])\n\t"
		"mov %[request_ptr], %%rdx\n\t"
		"mov %[syscall_nr], %%eax\n\t"
		"syscall\n\t"
		"mov %%rax, %[syscall_result]\n\t"
		"vmovdqu %%ymm0, 0(%[after])\n\t"
		"vmovdqu %%ymm1, 32(%[after])\n\t"
		"vmovdqu %%ymm2, 64(%[after])\n\t"
		"vmovdqu %%ymm3, 96(%[after])\n\t"
		"vmovdqu %%ymm4, 128(%[after])\n\t"
		"vmovdqu %%ymm5, 160(%[after])\n\t"
		"vmovdqu %%ymm6, 192(%[after])\n\t"
		"vmovdqu %%ymm7, 224(%[after])\n\t"
		"movl %[mask_low], %%eax\n\t"
		"movl %[mask_high], %%edx\n\t"
		"xsave64 (%[xstate_after])\n\t"
			"fxrstor64 %[original_legacy]\n\t"
		: "+&a"(rax), "+&D"(rdi), "+&S"(rsi), "+&d"(rdx),
			  [syscall_result] "=m"(syscall_result),
			  [original_legacy] "=&m"(original_legacy)
		: [request_ptr] "r"(request), [syscall_nr] "i"(SYS_ioctl),
		  [xstate_before] "r"(xstate_before), [xstate_after] "r"(xstate_after),
		  [after] "r"(after),
		  [mask_low] "m"(mask_low), [mask_high] "m"(mask_high),
			  [x87_control] "m"(x87_control), [mxcsr_control] "m"(mxcsr_control),
		  [vectors] "r"(vectors)
		: "cc", "rcx", "r10", "r11", "memory", "ymm0", "ymm1", "ymm2", "ymm3",
		  "ymm4", "ymm5", "ymm6", "ymm7");
	return syscall_result;
}

static bool legacy_sentinels_present(const void *image)
{
	const unsigned char *bytes = image;
	uint64_t st0_significand, xstate_bv;
	uint32_t mxcsr;
	uint16_t fcw, fsw, st0_exponent;

	/* In the standard XSAVE format, the legacy area holds FCW, FTW,
	 * MXCSR, and 80-bit ST(0). After FNINIT, FLD1 pushes onto physical
	 * register 7 (FSW.TOP=7), sets its abridged tag, and stores the encoding
	 * of 1.0. */
	memcpy(&fcw, bytes, sizeof(fcw));
	memcpy(&fsw, bytes + 2, sizeof(fsw));
	memcpy(&mxcsr, bytes + 24, sizeof(mxcsr));
	memcpy(&st0_significand, bytes + 32, sizeof(st0_significand));
	memcpy(&st0_exponent, bytes + 40, sizeof(st0_exponent));
	memcpy(&xstate_bv, bytes + LEGACY_XSTATE_SIZE, sizeof(xstate_bv));
	return (xstate_bv & 3) == 3 && fcw == x87_control &&
	       ((fsw >> 11) & 7) == 7 && bytes[4] == 0x80 &&
	       mxcsr == mxcsr_control &&
	       st0_significand == UINT64_C(0x8000000000000000) &&
	       st0_exponent == 0x3fff;
}

static int get_xsave_config(struct xsave_config *config)
{
	unsigned int eax, ebx, ecx, edx, max_leaf;
	unsigned int xcr0_low, xcr0_high;
	uint64_t xcr0, supported;

	max_leaf = __get_cpuid_max(0, NULL);
	if (max_leaf < 0x0d || !__get_cpuid(1, &eax, &ebx, &ecx, &edx) ||
	    !(ecx & (1U << 26)) || !(ecx & (1U << 27)) || !(ecx & (1U << 28))) {
		fprintf(stderr, "CPU does not expose CPUID.0D, XSAVE, OSXSAVE and AVX\n");
		return -1;
	}
	__asm__ volatile("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0));
	if ((xcr0_low & 0x7) != 0x7) {
		fprintf(stderr, "OS has not enabled x87+XMM+YMM state in XCR0 (0x%x:%x)\n",
			xcr0_high, xcr0_low);
		return -1;
	}
	__cpuid_count(0x0d, 0, eax, ebx, ecx, edx);
	xcr0 = ((uint64_t)xcr0_high << 32) | xcr0_low;
	supported = ((uint64_t)edx << 32) | eax;
	if (xcr0 & ~supported) {
		fprintf(stderr, "XCR0 includes state unsupported by CPUID.0D\n");
		return -1;
	}
	if (ebx < 576 || ebx > (1U << 20)) {
		fprintf(stderr, "unexpected enabled XSAVE image size: %u\n", ebx);
		return -1;
	}
	config->size = ebx;
	config->mask = xcr0;
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

static int run_probe_ioctl(int fd, pid_t pid, unsigned int cpu, unsigned int mode,
			   const struct xsave_config *config)
{
	struct x86_boundary_probe_request request = {.cpu = cpu, .mode = mode};
	uint32_t after[8][8] __attribute__((aligned(32)));
	void *xstate_before = NULL, *xstate_after = NULL;
	long result;
	long expected = mode == X86_BOUNDARY_PROBE_MODE_RETURN ? 0 : -ECANCELED;
	int alloc_error;
	size_t mismatch;

	alloc_error = posix_memalign(&xstate_before, 64, config->size);
	if (!alloc_error)
		alloc_error = posix_memalign(&xstate_after, 64, config->size);
	if (alloc_error) {
		fprintf(stderr, "could not allocate aligned XSAVE snapshots: %s\n",
			strerror(alloc_error));
		free(xstate_before);
		free(xstate_after);
		return -1;
	}
	memset(xstate_before, 0, config->size);
	memset(xstate_after, 0, config->size);
	if (check_linux_syscalls(pid))
		goto fail;
	result = invoke_with_xstate(fd, &request, after, xstate_before,
				    xstate_after, config);
	if (result != expected) {
		fprintf(stderr, "cpu %u mode %u returned %ld, expected %ld\n",
			cpu, mode, result, expected);
		goto fail;
	}
	if (!legacy_sentinels_present(xstate_before)) {
		fprintf(stderr, "cpu %u mode %u did not capture seeded x87/MXCSR state\n",
			cpu, mode);
		goto fail;
	}
	if (memcmp(vectors, after, sizeof(vectors))) {
		fprintf(stderr, "cpu %u mode %u changed userspace XSAVE/YMM state\n", cpu, mode);
		goto fail;
	}
	if (memcmp(xstate_before, xstate_after, config->size)) {
		for (mismatch = 0; mismatch < config->size; mismatch++) {
			if (((unsigned char *)xstate_before)[mismatch] !=
			    ((unsigned char *)xstate_after)[mismatch])
				break;
		}
		fprintf(stderr,
			"cpu %u mode %u changed enabled XSAVE state at byte %zu (size=%zu xcr0=%#llx)\n",
			cpu, mode, mismatch, config->size,
			(unsigned long long)config->mask);
		goto fail;
	}
	if (check_linux_syscalls(pid))
		goto fail;
	free(xstate_before);
	free(xstate_after);
	return 0;

fail:
	free(xstate_before);
	free(xstate_after);
	return -1;
}

int main(void)
{
	const pid_t pid = getpid();
	struct xsave_config xsave;
	long cpu_count;
	int fd, cpu;

	if (get_xsave_config(&xsave))
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
		if (run_probe_ioctl(fd, pid, cpu, X86_BOUNDARY_PROBE_MODE_RETURN, &xsave) ||
		    run_probe_ioctl(fd, pid, cpu, X86_BOUNDARY_PROBE_MODE_ABORT, &xsave))
			return 1;
	}
	if (close(fd)) {
		perror("close probe device");
		return 1;
	}
	printf("x86 boundary probe passed: %ld CPUs, return/abort, host state, enabled XSAVE/x87/MXCSR/YMM, Linux syscalls (xcr0=%#llx size=%zu)\n",
	       cpu_count, (unsigned long long)xsave.mask, xsave.size);
	return 0;
}
