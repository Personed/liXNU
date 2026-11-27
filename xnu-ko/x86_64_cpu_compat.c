// SPDX-License-Identifier: APSL-2.0
/* Source-pinned x86 CPU eligibility preflight; never enters XNU. */
#include "x86_64_cpu_compat.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#define XNU_X86_FAMILY_INTEL_CORE 6U

/* Values from cpuid.h, consumed by cpuid_set_cpufamily() in cpuid.c. */
static int xnu_x86_intel_model_supported(xnu_x86_cpu_u32 family,
					 xnu_x86_cpu_u32 model)
{
	if (family != XNU_X86_FAMILY_INTEL_CORE)
		return 0;

	switch (model) {
	case 0x17: /* Penryn */
	case 0x1a: /* Nehalem */
	case 0x1e: /* Lynnfield / Clarksfield */
	case 0x1f: /* Havendale / Auburndale */
	case 0x2e: /* Nehalem-EX */
	case 0x25: /* Westmere */
	case 0x2c: /* Westmere-EP / WS */
	case 0x2f: /* Westmere-EX */
	case 0x2a: /* Sandy Bridge */
	case 0x2d: /* Sandy Bridge-EP */
	case 0x3a: /* Ivy Bridge */
	case 0x3e: /* Ivy Bridge-EP */
	case 0x3c: /* Haswell */
	case 0x3f: /* Haswell-EP */
	case 0x45: /* Haswell-ULT */
	case 0x46: /* Crystalwell */
	case 0x3d: /* Broadwell */
	case 0x47: /* Broadwell-EP */
	case 0x4e: /* Skylake */
	case 0x5e: /* Skylake desktop */
	case 0x55: /* Skylake-X */
	case 0x8e: /* Kaby Lake */
	case 0x9e: /* Kaby Lake desktop */
	case 0x7e: /* Ice Lake */
	case 0x7d: /* Ice Lake desktop */
	case 0x9f: /* Ice Lake-H */
	case 0xa5: /* Comet Lake desktop */
		return 1;
	default:
		return 0;
	}
}

enum xnu_x86_cpu_preflight_result
xnu_x86_cpu_preflight(const struct xnu_x86_cpu_profile *profile)
{
	static const unsigned char genuine_intel[XNU_X86_CPU_VENDOR_SIZE] = {
		'G', 'e', 'n', 'u', 'i', 'n', 'e', 'I', 'n', 't', 'e', 'l'
	};

	if (!profile)
		return XNU_X86_CPU_INVALID_PROFILE;
	if (memcmp(profile->vendor, genuine_intel, sizeof(genuine_intel)))
		return XNU_X86_CPU_UNSUPPORTED_VENDOR;
	if (!xnu_x86_intel_model_supported(profile->family, profile->model))
		return XNU_X86_CPU_UNSUPPORTED_FAMILY_MODEL;
	if ((profile->x86_64h_features & XNU_X86_CPU_X86_64H_MASK) !=
	    XNU_X86_CPU_X86_64H_MASK)
		return XNU_X86_CPU_MISSING_X86_64H_FEATURES;
	return XNU_X86_CPU_COMPATIBLE;
}
