// SPDX-License-Identifier: APSL-2.0
#include "../../xnu-ko/x86_64_cpu_compat.h"

#include <assert.h>
#include <string.h>

static struct xnu_x86_cpu_profile valid_intel(void)
{
	struct xnu_x86_cpu_profile profile = {
		.family = 6,
		.model = 0x3c, /* Haswell, explicitly recognized in cpuid_set_cpufamily(). */
		.x86_64h_features = XNU_X86_CPU_X86_64H_MASK,
	};

	memcpy(profile.vendor, "GenuineIntel", XNU_X86_CPU_VENDOR_SIZE);
	return profile;
}

static void test_valid_intel(void)
{
	struct xnu_x86_cpu_profile profile = valid_intel();

	assert(xnu_x86_cpu_preflight(&profile) == XNU_X86_CPU_COMPATIBLE);
	assert(xnu_x86_cpu_preflight(NULL) == XNU_X86_CPU_INVALID_PROFILE);
}

static void test_amd_fails_closed_even_with_all_features(void)
{
	struct xnu_x86_cpu_profile profile = valid_intel();

	memcpy(profile.vendor, "AuthenticAMD", XNU_X86_CPU_VENDOR_SIZE);
	assert(xnu_x86_cpu_preflight(&profile) == XNU_X86_CPU_UNSUPPORTED_VENDOR);
}

static void test_unknown_intel_model_fails_closed(void)
{
	struct xnu_x86_cpu_profile profile = valid_intel();

	profile.model = 0x97; /* Alder Lake is not in the pinned source switch. */
	assert(xnu_x86_cpu_preflight(&profile) ==
	       XNU_X86_CPU_UNSUPPORTED_FAMILY_MODEL);
	profile.family = 0xf;
	assert(xnu_x86_cpu_preflight(&profile) ==
	       XNU_X86_CPU_UNSUPPORTED_FAMILY_MODEL);
}

static void test_every_x86_64h_feature_is_required(void)
{
	static const xnu_x86_cpu_u32 feature_bits[] = {
		XNU_X86_CPU_FEAT_FMA, XNU_X86_CPU_FEAT_SSE4_2,
		XNU_X86_CPU_FEAT_MOVBE, XNU_X86_CPU_FEAT_POPCNT,
		XNU_X86_CPU_FEAT_AVX, XNU_X86_CPU_FEAT_LZCNT,
		XNU_X86_CPU_FEAT_BMI1, XNU_X86_CPU_FEAT_AVX2,
		XNU_X86_CPU_FEAT_BMI2,
	};
	struct xnu_x86_cpu_profile profile = valid_intel();
	unsigned int i;

	for (i = 0; i < sizeof(feature_bits) / sizeof(feature_bits[0]); i++) {
		profile.x86_64h_features = XNU_X86_CPU_X86_64H_MASK & ~feature_bits[i];
		assert(xnu_x86_cpu_preflight(&profile) ==
		       XNU_X86_CPU_MISSING_X86_64H_FEATURES);
	}
}

int main(void)
{
	test_valid_intel();
	test_amd_fails_closed_even_with_all_features();
	test_unknown_intel_model_fails_closed();
	test_every_x86_64h_feature_is_required();
	return 0;
}
