// SPDX-License-Identifier: APSL-2.0
#include "x86_64_boot_ptd.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#define BOOT_APERTURE_BYTES (XNU_X86_BOOT_PTD_ENTRIES * XNU_X86_BOOT_PTD_LEAF_BYTES)
#define BOOT_LEAF_MASK (XNU_X86_BOOT_PTD_LEAF_BYTES - 1)
#define BOOT_PDE_PRESENT (1ULL << 0)
#define BOOT_PDE_WRITE (1ULL << 1)
#define BOOT_PDE_LARGE (1ULL << 7)

int xnu_x86_boot_ptd_build(xnu_boot_u64 *entries, unsigned int count,
			   xnu_boot_u64 owned_pa, xnu_boot_u64 owned_bytes)
{
	const xnu_boot_u64 flags = BOOT_PDE_PRESENT | BOOT_PDE_WRITE | BOOT_PDE_LARGE;
	xnu_boot_u64 owned_end, pa;
	unsigned int i;

	if (!entries || count != XNU_X86_BOOT_PTD_ENTRIES ||
	    !owned_pa || !owned_bytes || ((owned_pa | owned_bytes) & BOOT_LEAF_MASK))
		return -EINVAL;
	if (owned_pa >= BOOT_APERTURE_BYTES ||
	    owned_bytes > BOOT_APERTURE_BYTES - owned_pa)
		return -ERANGE;

	owned_end = owned_pa + owned_bytes;
	for (i = 0, pa = 0; i < count; i++, pa += XNU_X86_BOOT_PTD_LEAF_BYTES)
		entries[i] = pa >= owned_pa && pa < owned_end ? pa | flags : 0;
	return 0;
}
