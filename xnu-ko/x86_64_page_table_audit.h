/* SPDX-License-Identifier: APSL-2.0 */
#pragma once

#include "x86_64_boot_args.h"

#ifndef __KERNEL__
#include <stddef.h>
#endif

struct xnu_x86_page_table_snapshot {
	const xnu_boot_u64 *entries;
	size_t size;
	xnu_boot_u64 pa;
	xnu_boot_u64 root_pa;
	unsigned int physical_bits;
};

/*
 * Check a private, stable snapshot of four-level x86_64 page tables.
 * Every reachable table must fit in the buffer and every present 4 KiB
 * or 2 MiB leaf must fit in the caller's owned physical region. The
 * snapshot itself must be sourced wholly from that region. The selected
 * profile uses plain physical addresses, NX, and zero absent entries.
 * The walk returns -E2BIG after 8192 table visits. Each path to a shared
 * page counts as a visit, including the low and high bootstrap aliases.
 *
 * This checks physical coverage. The caller still owns pinning, mapping
 * permissions and virtual layout, and must keep the verified tables stable
 * until entry. This function neither reads CR3 nor installs any mapping.
 */
int xnu_x86_page_table_audit(const struct xnu_x86_page_table_snapshot *snapshot,
			     xnu_boot_u64 owned_pa, xnu_boot_u64 owned_bytes);

#define XNU_X86_PAGE_WRITE (1U << 0)
#define XNU_X86_PAGE_EXEC (1U << 1)
#define XNU_X86_PAGE_USER (1U << 2)

struct xnu_x86_expected_mapping {
	xnu_boot_u64 va;
	xnu_boot_u64 pa;
	xnu_boot_u64 size;
	unsigned int permissions;
	unsigned int permission_mask;
};

/*
 * Check that a short handoff object, such as the entry code, stack, or
 * boot arguments, maps to the intended contiguous physical bytes. Readability
 * follows from presence in this profile. Flag bits in permission_mask must
 * equal the combined page table permission bits in permissions. The
 * caller must establish CR0.WP, EFER.NXE and its CPU protection policy.
 * Each lookup uses the same 8192-visit budget across the entire range.
 * Use the full audit to check the other branches of the root as well.
 */
int xnu_x86_page_table_check_mapping(
	const struct xnu_x86_page_table_snapshot *snapshot,
	xnu_boot_u64 owned_pa, xnu_boot_u64 owned_bytes,
	const struct xnu_x86_expected_mapping *expected);
