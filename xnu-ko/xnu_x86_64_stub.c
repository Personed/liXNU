// SPDX-License-Identifier: APSL-2.0
/*
 * Fail-closed x86_64 module target.
 *
 * This allows the module packaging and CI target to build on x86_64 while
 * native XNU entry, return, fault delivery, and context isolation are being
 * ported. Do not replace this with a successful no-op: loading an XNU module
 * without those architecture boundaries would be unsafe.
 */
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <asm/cpuid.h>
#include <linux/string.h>
#include "x86_64_cpu_compat.h"

struct xnu_cpu_feature {
	xnu_x86_cpu_u32 profile_mask;
	const char *name;
};

static const struct xnu_cpu_feature xnu_required_cpu_features[] = {
	{ XNU_X86_CPU_FEAT_FMA, "FMA" },
	{ XNU_X86_CPU_FEAT_SSE4_2, "SSE4.2" },
	{ XNU_X86_CPU_FEAT_MOVBE, "MOVBE" },
	{ XNU_X86_CPU_FEAT_POPCNT, "POPCNT" },
	{ XNU_X86_CPU_FEAT_AVX, "AVX" },
	{ XNU_X86_CPU_FEAT_LZCNT, "LZCNT" },
	{ XNU_X86_CPU_FEAT_BMI1, "BMI1" },
	{ XNU_X86_CPU_FEAT_AVX2, "AVX2" },
	{ XNU_X86_CPU_FEAT_BMI2, "BMI2" },
};

static void xnu_read_cpu_profile(struct xnu_x86_cpu_profile *profile)
{
	u32 eax, ebx, ecx, edx;
	u32 max_basic, max_extended;
	u32 base_family, base_model, extended_family, extended_model;
	unsigned int i;

	memset(profile, 0, sizeof(*profile));
	cpuid(0, &eax, &ebx, &ecx, &edx);
	max_basic = eax;
	memcpy(profile->vendor, &ebx, 4);
	memcpy(profile->vendor + 4, &edx, 4);
	memcpy(profile->vendor + 8, &ecx, 4);

	if (max_basic >= 1) {
		cpuid(1, &eax, &ebx, &ecx, &edx);
		base_family = (eax >> 8) & 0xf;
		base_model = (eax >> 4) & 0xf;
		extended_family = (eax >> 20) & 0xff;
		extended_model = (eax >> 16) & 0xf;
		profile->family = base_family;
		profile->model = base_model;
		/* Match XNU's cpuid_set_generic_info() family/model folding. */
		if (base_family == 0x6 || base_family == 0xf)
			profile->model |= extended_model << 4;
		if (base_family == 0xf)
			profile->family += extended_family;
		if (ecx & (1U << 12))
			profile->x86_64h_features |= XNU_X86_CPU_FEAT_FMA;
		if (ecx & (1U << 20))
			profile->x86_64h_features |= XNU_X86_CPU_FEAT_SSE4_2;
		if (ecx & (1U << 22))
			profile->x86_64h_features |= XNU_X86_CPU_FEAT_MOVBE;
		if (ecx & (1U << 23))
			profile->x86_64h_features |= XNU_X86_CPU_FEAT_POPCNT;
		if (ecx & (1U << 28))
			profile->x86_64h_features |= XNU_X86_CPU_FEAT_AVX;
	}

	cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
	max_extended = eax;
	if (max_extended >= 0x80000001) {
		cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
		if (ecx & (1U << 5))
			profile->x86_64h_features |= XNU_X86_CPU_FEAT_LZCNT;
	}
	if (max_basic >= 7) {
		cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
		if (ebx & (1U << 3))
			profile->x86_64h_features |= XNU_X86_CPU_FEAT_BMI1;
		if (ebx & (1U << 5))
			profile->x86_64h_features |= XNU_X86_CPU_FEAT_AVX2;
		if (ebx & (1U << 8))
			profile->x86_64h_features |= XNU_X86_CPU_FEAT_BMI2;
	}
}

static int xnu_cpu_preflight(char *missing, size_t missing_size)
{
	struct xnu_x86_cpu_profile profile;
	unsigned int used = 0, i;
	enum xnu_x86_cpu_preflight_result result;

	xnu_read_cpu_profile(&profile);
	result = xnu_x86_cpu_preflight(&profile);
	if (result == XNU_X86_CPU_MISSING_X86_64H_FEATURES) {
		missing[0] = '\0';
		for (i = 0; i < ARRAY_SIZE(xnu_required_cpu_features); i++) {
			const struct xnu_cpu_feature *feature = &xnu_required_cpu_features[i];

			if (profile.x86_64h_features & feature->profile_mask)
				continue;
			used += scnprintf(missing + used, missing_size - used, "%s%s",
					  used ? " " : "", feature->name);
		}
		return result;
	}
	if (result == XNU_X86_CPU_UNSUPPORTED_VENDOR)
		pr_err("xnu: pinned XNU source requires GenuineIntel; this CPU vendor is unsupported\n");
	else if (result == XNU_X86_CPU_UNSUPPORTED_FAMILY_MODEL)
		pr_err("xnu: pinned XNU source does not recognize Intel family %u model 0x%x\n",
		       profile.family, profile.model);
	else if (result == XNU_X86_CPU_INVALID_PROFILE)
		pr_err("xnu: invalid CPU preflight profile\n");
	return result;
}

static int __init xnu_x86_64_init(void)
{
	char missing[128];
	int status;

	status = xnu_cpu_preflight(missing, sizeof(missing));
	if (status == XNU_X86_CPU_MISSING_X86_64H_FEATURES) {
		pr_err("xnu: CPU is missing required x86_64 features: %s\n", missing);
		return -EOPNOTSUPP;
	}
	if (status != XNU_X86_CPU_COMPATIBLE)
		return -EOPNOTSUPP;
	pr_err("xnu: native x86_64 world switch is not implemented; refusing to load\n");
	return -EOPNOTSUPP;
}

module_init(xnu_x86_64_init);

MODULE_LICENSE("Proprietary");
MODULE_DESCRIPTION("Fail-closed x86_64 XNU module target");
