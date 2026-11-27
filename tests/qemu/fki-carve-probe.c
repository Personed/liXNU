// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include "../../fki-ko/fki.h"

/* Defaults match tests/qemu/prepare.sh; override them for a direct ARM64 host. */
static unsigned long long expected_base = 0x70000000ULL;
module_param(expected_base, ullong, 0444);
MODULE_PARM_DESC(expected_base, "Expected 2 MiB-aligned physical carve-out base in bytes");

static unsigned long expected_mb = 256;
module_param(expected_mb, ulong, 0444);
MODULE_PARM_DESC(expected_mb, "Expected physical carve-out size in MiB");

static unsigned long request_mb = 16;
module_param(request_mb, ulong, 0444);
MODULE_PARM_DESC(request_mb, "Requested contiguous allocation size in MiB");

static int __init fki_carve_probe_init(void)
{
	u64 expected_end, expected_size, first_pa = 0;
	unsigned long request_size, allocated_size, align_mask = SZ_2M - 1;
	unsigned long max_contig_size = (~0UL >> 1) + 1;
	unsigned int pass;

	if (!expected_mb || expected_mb > (~0UL >> 20) ||
	    (u64)expected_base > ~0ULL - ((u64)expected_mb << 20)) {
		pr_err("fki-carve-probe: invalid expected carve-out bounds\n");
		return -EINVAL;
	}
	if ((u64)expected_base & align_mask) {
		pr_err("fki-carve-probe: expected carve-out base is not 2 MiB aligned\n");
		return -EINVAL;
	}
	if (!request_mb || request_mb > (~0UL >> 20)) {
		pr_err("fki-carve-probe: invalid request size\n");
		return -EINVAL;
	}
	request_size = request_mb << 20;
	if (request_size > max_contig_size) {
		pr_err("fki-carve-probe: request size %lu exceeds the contiguous allocation limit %lu\n",
		       request_size, max_contig_size);
		return -ERANGE;
	}
	allocated_size = (request_size + align_mask) & ~align_mask;
	expected_size = (u64)expected_mb << 20;
	expected_end = (u64)expected_base + expected_size;
	if ((u64)allocated_size > expected_size) {
		pr_err("fki-carve-probe: aligned request %lu bytes exceeds expected carve-out %llu bytes\n",
		       allocated_size, (unsigned long long)expected_size);
		return -ERANGE;
	}
	pr_info("fki-carve-probe: expect [%#llx, %#llx), request %lu MiB, aligned allocation %lu bytes\n",
		(unsigned long long)expected_base, (unsigned long long)expected_end,
		request_mb, allocated_size);

	for (pass = 1; pass <= 2; pass++) {
		struct fki_region region = {0};
		int err = fki_contig_alloc(request_size, &region);

		if (err) {
			pr_err("fki-carve-probe: allocation %u failed: %d\n", pass, err);
			return err;
		}
		if (region.size != allocated_size) {
			pr_err("fki-carve-probe: allocation %u returned %llu bytes, expected %lu\n",
			       pass, (unsigned long long)region.size, allocated_size);
			fki_contig_free(&region);
			return -ERANGE;
		}
		if (region.pa < (u64)expected_base || region.pa > expected_end ||
		    region.size > expected_end - region.pa) {
			pr_err("fki-carve-probe: allocation %u at %#llx with size %llu is outside expected carve-out [%#llx, %#llx)\n",
			       pass, (unsigned long long)region.pa,
			       (unsigned long long)region.size,
			       (unsigned long long)expected_base, (unsigned long long)expected_end);
			fki_contig_free(&region);
			return -ERANGE;
		}
		pr_info("fki-carve-probe: allocation %u got %llu bytes at physical %#llx\n",
			pass, (unsigned long long)region.size, (unsigned long long)region.pa);
		pr_info("fki-carve-probe: allocation %u within expected carve-out [%#llx, %#llx)\n",
			pass, (unsigned long long)expected_base, (unsigned long long)expected_end);
		if (pass == 1) {
			first_pa = region.pa;
		} else if (region.pa == first_pa) {
			pr_info("fki-carve-probe: same-pa-reuse: yes (%#llx)\n",
				(unsigned long long)region.pa);
		} else {
			pr_err("fki-carve-probe: same-pa-reuse: no (first %#llx, second %#llx)\n",
			       (unsigned long long)first_pa, (unsigned long long)region.pa);
			fki_contig_free(&region);
			return -ERANGE;
		}
		fki_contig_free(&region);
	}
	return 0;
}

static void __exit fki_carve_probe_exit(void) {}

module_init(fki_carve_probe_init);
module_exit(fki_carve_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FKI physical carve-out allocation and reuse probe");
