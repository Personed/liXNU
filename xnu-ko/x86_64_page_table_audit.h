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
 *
 * This checks physical coverage. The caller still owns pinning, mapping
 * permissions and virtual layout, and must keep the verified tables stable
 * until entry. This function neither reads CR3 nor installs any mapping.
 */
int xnu_x86_page_table_audit(const struct xnu_x86_page_table_snapshot *snapshot,
			     xnu_boot_u64 owned_pa, xnu_boot_u64 owned_bytes);
