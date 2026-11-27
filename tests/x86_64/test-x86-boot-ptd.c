// SPDX-License-Identifier: APSL-2.0
#include "x86_64_boot_ptd.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#define LEAF_BYTES XNU_X86_BOOT_PTD_LEAF_BYTES
#define FLAGS 0x83ULL

static xnu_boot_u64 entries[XNU_X86_BOOT_PTD_ENTRIES];

static void test_sparse_leaves(void)
{
	unsigned int i;

	assert(xnu_x86_boot_ptd_build(entries, XNU_X86_BOOT_PTD_ENTRIES,
				       511 * LEAF_BYTES, 2 * LEAF_BYTES) == 0);
	for (i = 0; i < XNU_X86_BOOT_PTD_ENTRIES; i++)
		assert(entries[i] == ((i == 511 || i == 512) ?
				      (i * LEAF_BYTES | FLAGS) : 0));

	i = XNU_X86_BOOT_PTD_ENTRIES - 1;
	assert(xnu_x86_boot_ptd_build(entries, XNU_X86_BOOT_PTD_ENTRIES,
				       i * LEAF_BYTES, LEAF_BYTES) == 0);
	assert(entries[i] == (i * LEAF_BYTES | FLAGS));
	assert(!entries[i - 1] && !entries[511] && !entries[512]);
}

static void test_rejected_intervals(void)
{
	const unsigned int count = XNU_X86_BOOT_PTD_ENTRIES;
	const xnu_boot_u64 end = count * LEAF_BYTES;

	entries[0] = 0xdeadbeef;
	assert(xnu_x86_boot_ptd_build(NULL, count, LEAF_BYTES, LEAF_BYTES) == -EINVAL);
	assert(xnu_x86_boot_ptd_build(entries, count - 1, LEAF_BYTES, LEAF_BYTES) == -EINVAL);
	assert(xnu_x86_boot_ptd_build(entries, count, 0, LEAF_BYTES) == -EINVAL);
	assert(xnu_x86_boot_ptd_build(entries, count, LEAF_BYTES, 0) == -EINVAL);
	assert(xnu_x86_boot_ptd_build(entries, count, LEAF_BYTES - 4096, LEAF_BYTES) == -EINVAL);
	assert(xnu_x86_boot_ptd_build(entries, count, LEAF_BYTES, LEAF_BYTES + 4096) == -EINVAL);
	assert(xnu_x86_boot_ptd_build(entries, count, end, LEAF_BYTES) == -ERANGE);
	assert(xnu_x86_boot_ptd_build(entries, count, end - LEAF_BYTES,
				       2 * LEAF_BYTES) == -ERANGE);
	assert(xnu_x86_boot_ptd_build(entries, count, LEAF_BYTES,
				       UINT64_MAX & ~(LEAF_BYTES - 1)) == -ERANGE);
	assert(entries[0] == 0xdeadbeef);
}

int main(void)
{
	test_sparse_leaves();
	test_rejected_intervals();
	puts("xnu x86 boot page directory fixtures passed");
	return 0;
}
