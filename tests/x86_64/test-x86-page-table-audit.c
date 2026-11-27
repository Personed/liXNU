// SPDX-License-Identifier: APSL-2.0
#include "x86_64_page_table_audit.h"
#include "x86_64_boot_ptd.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define PAGE_BYTES 4096ULL
#define LARGE_BYTES (1ULL << 21)
#define BASE 0x200000ULL
#define OWNED_BYTES (2 * LARGE_BYTES)
#define ENTRIES 512U
#define PRESENT_RW 3ULL
#define LARGE (1ULL << 7)
#define LARGE_PAT (1ULL << 12)
#define NX (1ULL << 63)

static xnu_boot_u64 pages[4 * ENTRIES];
#define ENTRY(page, slot) pages[(page) * ENTRIES + (slot)]
static const struct xnu_x86_page_table_snapshot snapshot = {
	.entries = pages,
	.size = sizeof(pages),
	.pa = BASE,
	.root_pa = BASE,
	.physical_bits = 46,
};

static void make_tables(void)
{
	memset(pages, 0, sizeof(pages));
	ENTRY(0, 0) = (BASE + PAGE_BYTES) | PRESENT_RW;
	ENTRY(0, 511) = ENTRY(0, 0); /* The high alias shares the low walk. */
	ENTRY(1, 0) = (BASE + 2 * PAGE_BYTES) | PRESENT_RW;
	ENTRY(2, 1) = (BASE + 3 * PAGE_BYTES) | PRESENT_RW;
	ENTRY(2, 2) = (BASE + LARGE_BYTES) | PRESENT_RW | LARGE | LARGE_PAT | NX;
	ENTRY(3, 0) = BASE | PRESENT_RW;
	ENTRY(3, 1) = (BASE + OWNED_BYTES - PAGE_BYTES) | PRESENT_RW | NX;
}

static void test_owned_leaves(void)
{
	make_tables();
	assert(xnu_x86_page_table_audit(&snapshot, BASE, OWNED_BYTES) == 0);
	ENTRY(3, 1) += PAGE_BYTES;
	assert(xnu_x86_page_table_audit(&snapshot, BASE, OWNED_BYTES) == -ERANGE);
	make_tables();
	ENTRY(2, 2) += LARGE_BYTES;
	assert(xnu_x86_page_table_audit(&snapshot, BASE, OWNED_BYTES) == -ERANGE);
	make_tables();
	assert(xnu_x86_page_table_audit(&snapshot, BASE, OWNED_BYTES - PAGE_BYTES) == -ERANGE);
}

static void test_bad_entries_and_snapshot(void)
{
	struct xnu_x86_page_table_snapshot changed = snapshot;

	make_tables();
	changed.root_pa += sizeof(pages);
	assert(xnu_x86_page_table_audit(&changed, BASE, OWNED_BYTES) == -ERANGE);
	changed = snapshot;
	changed.size -= PAGE_BYTES;
	assert(xnu_x86_page_table_audit(&changed, BASE, OWNED_BYTES) == -ERANGE);
	ENTRY(2, 2) |= 1ULL << 14;
	assert(xnu_x86_page_table_audit(&snapshot, BASE, OWNED_BYTES) == -ENOEXEC);
	make_tables();
	ENTRY(1, 0) = PRESENT_RW | LARGE;
	assert(xnu_x86_page_table_audit(&snapshot, BASE, OWNED_BYTES) == -EOPNOTSUPP);
	make_tables();
	ENTRY(3, 0) |= 1ULL << 46;
	assert(xnu_x86_page_table_audit(&snapshot, BASE, OWNED_BYTES) == -ENOEXEC);
	make_tables();
	ENTRY(3, 2) = 2; /* Absent but contains flags. */
	assert(xnu_x86_page_table_audit(&snapshot, BASE, OWNED_BYTES) == -ENOEXEC);
	memset(pages, 0, sizeof(pages));
	assert(xnu_x86_page_table_audit(&snapshot, BASE, OWNED_BYTES) == -ENOEXEC);
}

static void test_work_limit(void)
{
	unsigned int i;

	make_tables();
	for (i = 0; i < ENTRIES; i++) {
		ENTRY(0, i) = ENTRY(0, 0);
		ENTRY(1, i) = ENTRY(1, 0);
	}
	assert(xnu_x86_page_table_audit(&snapshot, BASE, OWNED_BYTES) == -E2BIG);
}

static void test_bootstrap_tree(void)
{
	static xnu_boot_u64 boot_pages[6 * ENTRIES];
	const xnu_boot_u64 base = 511 * LARGE_BYTES;
	struct xnu_x86_page_table_snapshot boot = {
		.entries = boot_pages,
		.size = sizeof(boot_pages),
		.pa = base,
		.root_pa = base,
		.physical_bits = 46,
	};
	xnu_boot_u64 *directory = boot_pages + 2 * ENTRIES;
	unsigned int i;

	/* Compare the ready-to-install tree after the top-level rebases. */
	boot_pages[0] = (base + PAGE_BYTES) | PRESENT_RW;
	boot_pages[511] = boot_pages[0];
	for (i = 0; i < 4; i++)
		boot_pages[ENTRIES + i] = (base + (2 + i) * PAGE_BYTES) | PRESENT_RW;
	assert(xnu_x86_boot_ptd_build(directory, XNU_X86_BOOT_PTD_ENTRIES,
				       base, OWNED_BYTES) == 0);
	assert(xnu_x86_page_table_audit(&boot, base, OWNED_BYTES) == 0);
	directory[0] = PRESENT_RW | LARGE;
	assert(xnu_x86_page_table_audit(&boot, base, OWNED_BYTES) == -ERANGE);

	/* Remove the low path so only the high alias sees this leaf. */
	boot_pages[0] = 0;
	assert(xnu_x86_page_table_audit(&boot, base, OWNED_BYTES) == -ERANGE);
	directory[0] = 0;
	assert(xnu_x86_page_table_audit(&boot, base, OWNED_BYTES) == 0);
}

int main(void)
{
	test_owned_leaves();
	test_bad_entries_and_snapshot();
	test_work_limit();
	test_bootstrap_tree();
	puts("xnu x86 page table ownership fixtures passed");
	return 0;
}
