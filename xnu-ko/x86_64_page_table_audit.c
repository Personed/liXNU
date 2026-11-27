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
#define PTE_WRITE (1ULL << 1)
#define PTE_USER (1ULL << 2)
#define PTE_LARGE (1ULL << 7)
#define PTE_LARGE_PAT (1ULL << 12)
#define PTE_NX (1ULL << 63)
#define ALL_PERMISSIONS (XNU_X86_PAGE_WRITE | XNU_X86_PAGE_EXEC | XNU_X86_PAGE_USER)

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

static int read_table(struct audit *audit, xnu_boot_u64 table_pa,
		      const xnu_boot_u64 **table)
{
	const struct xnu_x86_page_table_snapshot *snapshot = audit->snapshot;

	if (++audit->visits > PT_MAX_VISITS)
		return -E2BIG;
	if (!span_contains(snapshot->pa, snapshot->size, table_pa, PT_BYTES))
		return -ERANGE;
	*table = snapshot->entries + (size_t)(table_pa - snapshot->pa) /
		 sizeof(**table);
	return 0;
}

static int decode_entry(const struct audit *audit, xnu_boot_u64 entry,
			unsigned int level, xnu_boot_u64 *pa,
			xnu_boot_u64 *leaf_size)
{
	if (!(entry & PTE_PRESENT) ||
	    (entry & ~(audit->address_mask | PT_PAGE_MASK | PTE_NX)))
		return -ENOEXEC;
	*pa = entry & audit->address_mask;
	if (level == 1) {
		*leaf_size = PT_BYTES;
	} else if (entry & PTE_LARGE) {
		if (level != 2)
			return -EOPNOTSUPP;
		*leaf_size = PT_LARGE_BYTES;
		*pa &= ~PTE_LARGE_PAT;
		if (*pa & (PT_LARGE_BYTES - 1))
			return -ENOEXEC;
	} else {
		*leaf_size = 0;
		return 0;
	}
	if (!span_contains(audit->owned_pa, audit->owned_bytes, *pa, *leaf_size))
		return -ERANGE;
	return 0;
}

static int walk(struct audit *audit, xnu_boot_u64 table_pa, unsigned int level)
{
	const xnu_boot_u64 *table;
	unsigned int i;
	int err;

	err = read_table(audit, table_pa, &table);
	if (err)
		return err;

	for (i = 0; i < PT_ENTRIES; i++) {
		xnu_boot_u64 entry = table[i];
		xnu_boot_u64 pa, size;

		if (!entry)
			continue;
		err = decode_entry(audit, entry, level, &pa, &size);
		if (err)
			return err;
		if (!size) {
			err = walk(audit, pa, level - 1);
			if (err)
				return err;
		} else {
			audit->mapped = true;
		}
	}
	return 0;
}

static int init_audit(struct audit *audit,
		      const struct xnu_x86_page_table_snapshot *snapshot,
		      xnu_boot_u64 owned_pa, xnu_boot_u64 owned_bytes)
{
	xnu_boot_u64 physical_limit;

	if (!snapshot || !snapshot->entries || !snapshot->size || !owned_bytes ||
	    snapshot->physical_bits < 32 || snapshot->physical_bits > 52 ||
	    ((snapshot->pa | snapshot->root_pa | snapshot->size |
	      owned_pa | owned_bytes) & PT_PAGE_MASK))
		return -EINVAL;
	physical_limit = 1ULL << snapshot->physical_bits;
	if (owned_pa >= physical_limit || owned_bytes > physical_limit - owned_pa ||
	    !span_contains(owned_pa, owned_bytes, snapshot->pa, snapshot->size))
		return -ERANGE;
	*audit = (struct audit) {
		.snapshot = snapshot,
		.owned_pa = owned_pa,
		.owned_bytes = owned_bytes,
		.address_mask = (physical_limit - 1) & ~PT_PAGE_MASK,
	};
	return 0;
}

int xnu_x86_page_table_audit(const struct xnu_x86_page_table_snapshot *snapshot,
			     xnu_boot_u64 owned_pa, xnu_boot_u64 owned_bytes)
{
	struct audit audit;
	int err;

	err = init_audit(&audit, snapshot, owned_pa, owned_bytes);
	if (err)
		return err;
	err = walk(&audit, snapshot->root_pa, 4);
	if (err)
		return err;
	return audit.mapped ? 0 : -ENOEXEC;
}

static int lookup(struct audit *audit, xnu_boot_u64 va, xnu_boot_u64 *pa,
		  xnu_boot_u64 *available, unsigned int *permissions)
{
	xnu_boot_u64 table_pa = audit->snapshot->root_pa;
	unsigned int level;

	*permissions = ALL_PERMISSIONS;
	for (level = 4; level; level--) {
		const xnu_boot_u64 *table;
		xnu_boot_u64 entry, size;
		unsigned int shift = 12 + 9 * (level - 1);
		int err;

		err = read_table(audit, table_pa, &table);
		if (err)
			return err;
		entry = table[(va >> shift) & (PT_ENTRIES - 1)];
		if (!entry)
			return -ENOENT;
		err = decode_entry(audit, entry, level, &table_pa, &size);
		if (err)
			return err;
		if (!(entry & PTE_WRITE))
			*permissions &= ~XNU_X86_PAGE_WRITE;
		if (entry & PTE_NX)
			*permissions &= ~XNU_X86_PAGE_EXEC;
		if (!(entry & PTE_USER))
			*permissions &= ~XNU_X86_PAGE_USER;
		if (size) {
			xnu_boot_u64 offset = va & (size - 1);

			*pa = table_pa + offset;
			*available = size - offset;
			return 0;
		}
	}
	return -ENOEXEC;
}

static bool canonical_span(xnu_boot_u64 va, xnu_boot_u64 size)
{
	xnu_boot_u64 half = va >> 47;

	return size && size - 1 <= ~(xnu_boot_u64)0 - va &&
	       (half == 0 || half == 0x1ffff) &&
	       (va + (size - 1)) >> 47 == half;
}

int xnu_x86_page_table_check_mapping(
	const struct xnu_x86_page_table_snapshot *snapshot,
	xnu_boot_u64 owned_pa, xnu_boot_u64 owned_bytes,
	const struct xnu_x86_expected_mapping *expected)
{
	xnu_boot_u64 offset = 0;
	struct audit audit;
	int err;

	if (!expected || !canonical_span(expected->va, expected->size) ||
	    (expected->permission_mask & ~ALL_PERMISSIONS) ||
	    (expected->permissions & ~expected->permission_mask))
		return -EINVAL;
	err = init_audit(&audit, snapshot, owned_pa, owned_bytes);
	if (err)
		return err;
	if (!span_contains(owned_pa, owned_bytes, expected->pa, expected->size))
		return -ERANGE;

	while (offset < expected->size) {
		xnu_boot_u64 pa, available, left = expected->size - offset;
		unsigned int permissions;

		err = lookup(&audit, expected->va + offset, &pa, &available, &permissions);
		if (err)
			return err;
		if (pa != expected->pa + offset)
			return -ERANGE;
		if ((permissions & expected->permission_mask) != expected->permissions)
			return -EPERM;
		offset += available < left ? available : left;
	}
	return 0;
}
