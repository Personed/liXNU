/* SPDX-License-Identifier: APSL-2.0 */
#pragma once

#include "x86_64_boot_args.h"

struct xnu_x86_boot_layout {
	xnu_boot_u64 carveout_pa;
	xnu_boot_u64 carveout_size;
	xnu_boot_u64 boot_args_pa;
	/* Reserve from round_page(kaddr + ksize), including the fixed tables. */
	xnu_boot_u64 bootstrap_bytes;
	/* The selected XNU image's nonempty __HIB virtual size. */
	xnu_boot_u64 hib_bytes;
	/* The selected XNU build's NKPT, including any build override. */
	xnu_boot_u32 idle_kernel_pt_pages;
};

/*
 * Validate placement for the source-pinned core boot objects. The caller
 * supplies a host-owned contiguous reservation and the bootstrap budget for
 * the selected artifact. This checks address arithmetic, containment and the
 * pinned non-KASAN Idle_PTs_init() allocation bound. Unlike the core
 * boot_args format check, placement requires nonzero PhysicalMemorySize
 * at or below the pinned K64_MAXMEM (2 TiB).
 * It does not verify ownership, EFI/ADT content, optional boot fields,
 * mappings, or that the supplied budget covers all build-specific writes.
 *
 * Returns 0, a core boot_args error, -EINVAL for missing or misaligned
 * layout inputs, or -ERANGE for a collision or insufficient space.
 */
int xnu_x86_boot_layout_validate(const struct xnu_x86_boot_args *args,
				 const struct xnu_x86_boot_layout *layout);
