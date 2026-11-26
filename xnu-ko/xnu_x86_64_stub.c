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
#include <asm/cpufeature.h>

struct xnu_cpu_feature {
	int bit;
	const char *name;
};

static const struct xnu_cpu_feature xnu_required_cpu_features[] = {
	{ X86_FEATURE_XMM4_2, "SSE4.2" },
	{ X86_FEATURE_AVX, "AVX" },
	{ X86_FEATURE_FMA, "FMA" },
	{ X86_FEATURE_MOVBE, "MOVBE" },
	{ X86_FEATURE_POPCNT, "POPCNT" },
	{ X86_FEATURE_ABM, "LZCNT" },
	{ X86_FEATURE_BMI1, "BMI1" },
	{ X86_FEATURE_AVX2, "AVX2" },
	{ X86_FEATURE_BMI2, "BMI2" },
};

static bool xnu_cpu_features_present(char *missing, size_t missing_size)
{
	bool present = true;
	size_t used = 0;
	int i;

	missing[0] = '\0';
	for (i = 0; i < ARRAY_SIZE(xnu_required_cpu_features); i++) {
		const struct xnu_cpu_feature *feature = &xnu_required_cpu_features[i];

		if (boot_cpu_has(feature->bit))
			continue;
		present = false;
		used += scnprintf(missing + used, missing_size - used, "%s%s",
				  used ? " " : "", feature->name);
	}
	return present;
}

static int __init xnu_x86_64_init(void)
{
	char missing[128];

	if (!xnu_cpu_features_present(missing, sizeof(missing))) {
		pr_err("xnu: CPU is missing required x86_64 features: %s\n", missing);
		return -EOPNOTSUPP;
	}
	pr_err("xnu: native x86_64 world switch is not implemented; refusing to load\n");
	return -EOPNOTSUPP;
}

module_init(xnu_x86_64_init);

MODULE_LICENSE("Proprietary");
MODULE_DESCRIPTION("Fail-closed x86_64 XNU module target");
