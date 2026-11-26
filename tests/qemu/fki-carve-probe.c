// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>
#include "../../fki-ko/fki.h"

static int __init fki_carve_probe_init(void)
{
	struct fki_region region = {0};
	unsigned int pass;

	for (pass = 1; pass <= 2; pass++) {
		int err = fki_contig_alloc(16UL << 20, &region);

		if (err) {
			pr_err("fki-carve-probe: allocation %u failed: %d\n", pass, err);
			return err;
		}
		if (region.size != (16UL << 20) || region.pa != 0x70000000) {
			pr_err("fki-carve-probe: allocation %u got %#llx bytes at physical %#llx\n",
			       pass, (unsigned long long)region.size, (unsigned long long)region.pa);
			fki_contig_free(&region);
			return -ERANGE;
		}
		pr_info("fki-carve-probe: allocation %u got %llu bytes at physical %#llx\n",
			pass, (unsigned long long)region.size, (unsigned long long)region.pa);
		fki_contig_free(&region);
	}
	return 0;
}

static void __exit fki_carve_probe_exit(void) {}

module_init(fki_carve_probe_init);
module_exit(fki_carve_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QEMU smoke test for the FKI physical memory carve-out");
