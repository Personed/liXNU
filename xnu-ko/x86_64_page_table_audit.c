// SPDX-License-Identifier: APSL-2.0
#include "x86_64_page_table_audit.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#include <stdbool.h>
#endif

#define PT_BYTES (1ULL << 12)
#define PT_PAGE_MASK (PT_BYTES - 1)
#define PT_ENTRIES 512U
/* Count visits, including repeated aliases of the same table page. */
#define PT_MAX_VISITS 8192U
#define PT_LARGE_BYTES (1ULL << 21)
#define PTE_PRESENT 1ULL
#define PTE_LARGE (1ULL << 7)
#define PTE_LARGE_PAT (1ULL << 12)
#define PTE_NX (1ULL << 63)

struct audit {
	const struct xnu_x86_page_table_snapshot *snapshot;
	xnu_boot_u64 owned_pa;
	xnu_boot_u64 owned_bytes;
	xnu_boot_u64 address_mask;
	unsigned int visits;
	bool mapped;
};

static bool span_contains(xnu_boot_u64 base, xnu_boot_u64 bytes,
			  xnu_boot_u64 pa, xnu_boot_u64 size)
{
	return pa >= base && size <= bytes && pa - base <= bytes - size;
}

static int walk(struct audit *audit, xnu_boot_u64 table_pa, unsigned int level)
{
	const struct xnu_x86_page_table_snapshot *snapshot = audit->snapshot;
	const xnu_boot_u64 *table;
	unsigned int i;

	if (++audit->visits > PT_MAX_VISITS)
		return -E2BIG;
	if (!span_contains(snapshot->pa, snapshot->size, table_pa, PT_BYTES))
		return -ERANGE;
	table = snapshot->entries + (size_t)(table_pa - snapshot->pa) /
		sizeof(*table);

	for (i = 0; i < PT_ENTRIES; i++) {
		xnu_boot_u64 entry = table[i];
		xnu_boot_u64 pa, size;
		int err;

		if (!entry)
			continue;
		if (!(entry & PTE_PRESENT) ||
		    (entry & ~(audit->address_mask | PT_PAGE_MASK | PTE_NX)))
			return -ENOEXEC;
		pa = entry & audit->address_mask;
		if (level == 1) {
			size = PT_BYTES;
		} else if (entry & PTE_LARGE) {
			if (level != 2)
				return -EOPNOTSUPP;
			size = PT_LARGE_BYTES;
			pa &= ~PTE_LARGE_PAT;
			if (pa & (size - 1))
				return -ENOEXEC;
		} else {
			err = walk(audit, pa, level - 1);
			if (err)
				return err;
			continue;
		}
		if (!span_contains(audit->owned_pa, audit->owned_bytes, pa, size))
			return -ERANGE;
		audit->mapped = true;
	}
	return 0;
}

int xnu_x86_page_table_audit(const struct xnu_x86_page_table_snapshot *snapshot,
			     xnu_boot_u64 owned_pa, xnu_boot_u64 owned_bytes)
{
	struct audit audit = {
		.snapshot = snapshot,
		.owned_pa = owned_pa,
		.owned_bytes = owned_bytes,
	};
	xnu_boot_u64 physical_limit;
	int err;

	if (!snapshot || !snapshot->entries || !snapshot->size || !owned_bytes ||
	    snapshot->physical_bits < 32 || snapshot->physical_bits > 52 ||
	    ((snapshot->pa | snapshot->root_pa | snapshot->size |
	      owned_pa | owned_bytes) & PT_PAGE_MASK))
		return -EINVAL;
	physical_limit = 1ULL << snapshot->physical_bits;
	if (owned_pa >= physical_limit || owned_bytes > physical_limit - owned_pa ||
	    !span_contains(owned_pa, owned_bytes, snapshot->pa, snapshot->size))
		return -ERANGE;
	audit.address_mask = (physical_limit - 1) & ~PT_PAGE_MASK;
	err = walk(&audit, snapshot->root_pa, 4);
	if (err)
		return err;
	return audit.mapped ? 0 : -ENOEXEC;
}
