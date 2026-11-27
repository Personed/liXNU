/* SPDX-License-Identifier: APSL-2.0 */
#pragma once

/*
 * Source-pinned x86 CPU preflight facts from apple-oss-distributions/xnu
 * xnu-12377.121.6 (ac9718fb1af618d5ce8678d0dc6e8a58f252216f),
 * osfmk/i386/cpuid.h and cpuid.c. These checks describe this public source
 * pin; they do not prove compatibility with the unbuilt Tahoe 26.7.3 (25G342) image.
 */
#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 xnu_x86_cpu_u32;
#else
#include <stdint.h>
typedef uint32_t xnu_x86_cpu_u32;
#endif

#define XNU_X86_CPU_VENDOR_SIZE 12U

/* Normalized bits for the pinned CPUID_X86_64_H_* feature masks. */
#define XNU_X86_CPU_FEAT_FMA      (1U << 0)
#define XNU_X86_CPU_FEAT_SSE4_2  (1U << 1)
#define XNU_X86_CPU_FEAT_MOVBE   (1U << 2)
#define XNU_X86_CPU_FEAT_POPCNT  (1U << 3)
#define XNU_X86_CPU_FEAT_AVX     (1U << 4)
#define XNU_X86_CPU_FEAT_LZCNT   (1U << 5)
#define XNU_X86_CPU_FEAT_BMI1    (1U << 6)
#define XNU_X86_CPU_FEAT_AVX2    (1U << 7)
#define XNU_X86_CPU_FEAT_BMI2    (1U << 8)
#define XNU_X86_CPU_X86_64H_MASK ((1U << 9) - 1U)

struct xnu_x86_cpu_profile {
	unsigned char vendor[XNU_X86_CPU_VENDOR_SIZE];
	xnu_x86_cpu_u32 family;
	xnu_x86_cpu_u32 model;
	/* Raw CPUID capability and the host Linux-enabled subset are both required. */
	xnu_x86_cpu_u32 x86_64h_features;
	xnu_x86_cpu_u32 linux_enabled_x86_64h_features;
};

enum xnu_x86_cpu_preflight_result {
	XNU_X86_CPU_COMPATIBLE = 0,
	XNU_X86_CPU_INVALID_PROFILE,
	XNU_X86_CPU_UNSUPPORTED_VENDOR,
	XNU_X86_CPU_UNSUPPORTED_FAMILY_MODEL,
	XNU_X86_CPU_MISSING_X86_64H_FEATURES,
};

/*
 * Check the pinned XNU Intel family/model whitelist and the project's current
 * x86_64h host floor. The source itself can select X86_ARCH1 when x86_64h
 * features are absent; this helper keeps the existing stricter host gate
 * explicit until the actual target artifact/profile justifies changing it.
 * No entry is attempted here; callers must still fail closed until a complete
 * Linux/XNU world boundary is available.
 */
enum xnu_x86_cpu_preflight_result
xnu_x86_cpu_preflight(const struct xnu_x86_cpu_profile *profile);
