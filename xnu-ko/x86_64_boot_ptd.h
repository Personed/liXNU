/* SPDX-License-Identifier: APSL-2.0 */
#pragma once

#include "x86_64_boot_args.h"

#define XNU_X86_BOOT_PTD_ENTRIES 2048U
#define XNU_X86_BOOT_PTD_LEAF_BYTES (1ULL << 21)

/*
 * Build the four BootPTD pages used by the pinned native XNU entry.
 * The caller supplies a contiguous, owned, 2 MiB-aligned physical interval
 * below 4 GiB and a private, uninstalled table. Start and length must be
 * exact multiples of 2 MiB. Page zero is excluded.
 *
 * The builder leaves the table unchanged on error. The caller is
 * responsible for verifying image offsets and reserving actual RAM.
 */
int xnu_x86_boot_ptd_build(xnu_boot_u64 *entries, unsigned int count,
			   xnu_boot_u64 owned_pa, xnu_boot_u64 owned_bytes);
