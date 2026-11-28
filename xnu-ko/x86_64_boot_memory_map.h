/* SPDX-License-Identifier: APSL-2.0 */
#pragma once

#include "x86_64_boot_args.h"

/*
 * Validate a synthetic EFI v1 map for XNU public source ac9718fb. All listed
 * ranges must be inside [owned_start, owned_end). Conventional memory must
 * start at or after first_usable, a page-aligned upper bound on first_avail
 * that includes all loader and bootstrap allocations. Reserved descriptors
 * may cover the owned prefix or holes. Omitted pages are unavailable. The
 * map must be sorted, disjoint and contain conventional memory. All virtual
 * starts and attributes must be zero for this profile.
 *
 * The caller must establish Linux ownership and keep args and map stable
 * during validation. This checks the map's description of memory. It does
 * not inspect physical memory or validate the placement of the handoff.
 */
int xnu_x86_boot_memory_map_validate_owned(const struct xnu_x86_boot_args *args,
					   const void *map, unsigned long map_size,
					   xnu_boot_u64 owned_start, xnu_boot_u64 owned_end,
					   xnu_boot_u64 first_usable);
