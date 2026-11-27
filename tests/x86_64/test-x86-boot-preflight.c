// SPDX-License-Identifier: APSL-2.0
#include "x86_64_boot_layout.h"
#include "x86_64_boot_memory_map.h"
#include "x86_64_boot_ptd.h"
#include "x86_64_page_table_audit.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#define PAGE_BYTES 4096ULL
#define PAGE_ENTRIES 512U
#define LARGE_BYTES XNU_X86_BOOT_PTD_LEAF_BYTES
#define OWNED_PA 0x02000000ULL
#define OWNED_BYTES (4 * LARGE_BYTES)
#define IMAGE_PA (OWNED_PA + LARGE_BYTES)
#define IMAGE_BYTES 0x100000ULL
#define BOOTSTRAP_BYTES 0x210000ULL
#define FIRST_USABLE (IMAGE_PA + IMAGE_BYTES + BOOTSTRAP_BYTES)
#define HIGH_ALIAS 0xffffff8000000000ULL
#define MAP_PA (OWNED_PA + PAGE_BYTES)
#define ADT_PA (OWNED_PA + 2 * PAGE_BYTES)
#define STACK_PA (OWNED_PA + 3 * PAGE_BYTES)
#define ENTRY_PA (IMAGE_PA + 6 * PAGE_BYTES)

static xnu_boot_u64 tables[6 * PAGE_ENTRIES];

struct root_adt {
	uint32_t properties, children;
	char key[32];
	uint32_t value_size;
	char value[4];
};

_Static_assert(sizeof(struct root_adt) == 48, "flat ADT fixture layout");

int main(void)
{
	const struct root_adt adt = { .properties = 1, .key = "name",
				     .value_size = 2, .value = "/" };
	struct xnu_x86_efi_memory_range map[] = {
		{
			.Type = 0, /* kEfiReservedMemoryType */
			.PhysicalStart = OWNED_PA,
			.NumberOfPages = (FIRST_USABLE - OWNED_PA) / PAGE_BYTES,
		},
		{
			.Type = 7, /* kEfiConventionalMemory */
			.PhysicalStart = FIRST_USABLE,
			.NumberOfPages = (OWNED_PA + OWNED_BYTES - FIRST_USABLE) / PAGE_BYTES,
		},
	};
	const struct xnu_x86_boot_args_core input = {
		.command_line = "",
		.memory_map_pa = MAP_PA,
		.memory_map_size = sizeof(map),
		.memory_map_descriptor_size = sizeof(map[0]),
		.memory_map_descriptor_version = 1,
		.device_tree_pa = ADT_PA,
		.device_tree_size = sizeof(adt),
		.kernel_pa = IMAGE_PA,
		.kernel_size = IMAGE_BYTES,
		.physical_memory_size = OWNED_BYTES,
	};
	const struct xnu_x86_boot_layout layout = {
		.carveout_pa = OWNED_PA,
		.carveout_size = OWNED_BYTES,
		.boot_args_pa = OWNED_PA,
		.bootstrap_bytes = BOOTSTRAP_BYTES,
		.hib_bytes = IMAGE_BYTES,
		.idle_kernel_pt_pages = 500,
	};
	const struct xnu_x86_page_table_snapshot snapshot = {
		.entries = tables,
		.size = sizeof(tables),
		.pa = IMAGE_PA,
		.root_pa = IMAGE_PA,
		.physical_bits = 46,
	};
	const struct xnu_x86_expected_mapping required[] = {
		{ OWNED_PA, OWNED_PA, sizeof(struct xnu_x86_boot_args), 0, XNU_X86_PAGE_USER },
		{ HIGH_ALIAS + MAP_PA, MAP_PA, sizeof(map), 0, XNU_X86_PAGE_USER },
		{ HIGH_ALIAS + ADT_PA, ADT_PA, sizeof(adt), 0, XNU_X86_PAGE_USER },
		{ HIGH_ALIAS + STACK_PA, STACK_PA, PAGE_BYTES,
		  XNU_X86_PAGE_WRITE, XNU_X86_PAGE_WRITE | XNU_X86_PAGE_USER },
		{ HIGH_ALIAS + ENTRY_PA, ENTRY_PA, 64,
		  XNU_X86_PAGE_EXEC, XNU_X86_PAGE_EXEC | XNU_X86_PAGE_USER },
	};
	struct xnu_x86_boot_args args;
	xnu_boot_u64 *directory = tables + 2 * PAGE_ENTRIES;
	unsigned int i;

	assert(xnu_x86_boot_args_build_core(&args, &input) == 0);
	assert(xnu_x86_boot_layout_validate(&args, &layout) == 0);
	assert(xnu_x86_boot_device_tree_validate(&args, &adt, sizeof(adt)) == 0);
	assert(xnu_x86_boot_memory_map_validate_owned(&args, map, sizeof(map),
							OWNED_PA, OWNED_PA + OWNED_BYTES,
							FIRST_USABLE) == 0);

	/* The table addresses model the final state after start.s rebases. */
	tables[0] = (IMAGE_PA + PAGE_BYTES) | 3;
	tables[511] = tables[0];
	for (i = 0; i < 4; i++)
		tables[PAGE_ENTRIES + i] = (IMAGE_PA + (2 + i) * PAGE_BYTES) | 3;
	assert(xnu_x86_boot_ptd_build(directory, XNU_X86_BOOT_PTD_ENTRIES,
				       OWNED_PA, OWNED_BYTES) == 0);
	assert(xnu_x86_page_table_audit(&snapshot, OWNED_PA, OWNED_BYTES) == 0);
	for (i = 0; i < sizeof(required) / sizeof(required[0]); i++)
		assert(xnu_x86_page_table_check_mapping(&snapshot, OWNED_PA, OWNED_BYTES,
							 &required[i]) == 0);

	/* A well-formed map must still protect the last page used by bootstrap. */
	map[0].NumberOfPages--;
	map[1].PhysicalStart -= PAGE_BYTES;
	map[1].NumberOfPages++;
	assert(xnu_x86_boot_memory_map_validate(&args, map, sizeof(map)) == 0);
	assert(xnu_x86_boot_memory_map_validate_owned(&args, map, sizeof(map),
							OWNED_PA, OWNED_PA + OWNED_BYTES,
							FIRST_USABLE) < 0);

	/* All remaining leaves are owned, but the stack must also be reachable. */
	directory[OWNED_PA / LARGE_BYTES] = 0;
	assert(xnu_x86_page_table_audit(&snapshot, OWNED_PA, OWNED_BYTES) == 0);
	assert(xnu_x86_page_table_check_mapping(&snapshot, OWNED_PA, OWNED_BYTES,
						 &required[3]) == -ENOENT);
	puts("xnu x86 integrated boot preflight fixture passed");
	return 0;
}
