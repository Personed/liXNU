// SPDX-License-Identifier: APSL-2.0
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "x86_64_boot_memory_map.h"

#define PAGE_SIZE 4096ULL
#define OWNED_START 0x100000ULL
#define FIRST_USABLE (OWNED_START + 4 * PAGE_SIZE)
#define OWNED_END (FIRST_USABLE + 260 * PAGE_SIZE)
#define MAP_STRIDE (sizeof(struct xnu_x86_efi_memory_range) + 8)

static struct xnu_x86_efi_memory_range descriptor(unsigned int type,
						 uint64_t start, uint64_t pages)
{
	return (struct xnu_x86_efi_memory_range) {
		.Type = type,
		.PhysicalStart = start,
		.NumberOfPages = pages,
	};
}

static int validate(const struct xnu_x86_efi_memory_range *ranges, unsigned int count,
		    uint64_t owned_start, uint64_t owned_end, uint64_t first_usable,
		    unsigned int version, unsigned long omitted_bytes)
{
	struct xnu_x86_boot_args args;
	const struct xnu_x86_boot_args_core core = {
		.command_line = "",
		.memory_map_pa = 0x70000,
		.memory_map_size = MAP_STRIDE * count,
		.memory_map_descriptor_size = MAP_STRIDE,
		.memory_map_descriptor_version = version,
		.device_tree_pa = 0x80000,
		.device_tree_size = 4096,
		.kernel_pa = OWNED_START,
		.kernel_size = 4096,
	};
	_Alignas(struct xnu_x86_efi_memory_range) unsigned char data[1 + 129 * MAP_STRIDE] = {0};
	unsigned int i;

	assert(count <= 129);
	assert(xnu_x86_boot_args_build_core(&args, &core) == 0);
	for (i = 0; i < count; i++)
		memcpy(data + 1 + i * MAP_STRIDE, &ranges[i], sizeof(ranges[i]));
	return xnu_x86_boot_memory_map_validate_owned(&args, data + 1,
						     core.memory_map_size - omitted_bytes,
						     owned_start, owned_end, first_usable);
}

static int validate_owned(const struct xnu_x86_efi_memory_range *ranges, unsigned int count)
{
	return validate(ranges, count, OWNED_START, OWNED_END, FIRST_USABLE, 1, 0);
}

static void test_prefix_and_order(void)
{
	struct xnu_x86_efi_memory_range ranges[] = {
		descriptor(0, OWNED_START, 4),
		descriptor(7, FIRST_USABLE, 2),
		descriptor(0, FIRST_USABLE + 2 * PAGE_SIZE, 2),
		descriptor(7, FIRST_USABLE + 5 * PAGE_SIZE, 2),
	};
	assert(validate_owned(ranges, 4) == 0);
	assert(validate(ranges, 4, OWNED_START, OWNED_END, FIRST_USABLE, 1, 1) == -ENOEXEC);
	assert(validate(ranges, 4, OWNED_START, OWNED_END, FIRST_USABLE + 1, 1, 0) == -EINVAL);
	assert(validate(ranges, 4, OWNED_START, OWNED_END, OWNED_END, 1, 0) == -EINVAL);
	assert(validate(ranges, 4, 0, OWNED_END, 2 * PAGE_SIZE, 1, 0) == -EINVAL);

	ranges[0] = descriptor(7, OWNED_START, 5);
	/* Inclusive top equals fap; XNU would expose the whole prefix. */
	assert(validate_owned(ranges, 1) == -ERANGE);
	ranges[0].NumberOfPages++;
	assert(validate_owned(ranges, 1) == -ERANGE);
	ranges[0] = descriptor(0, OWNED_START - PAGE_SIZE, 2);
	assert(validate_owned(ranges, 4) == -ERANGE);
	ranges[0] = descriptor(0, OWNED_START, 5);
	assert(validate_owned(ranges, 4) == -ENOEXEC);
	ranges[0] = descriptor(0, OWNED_START, 4);
	ranges[3] = descriptor(7, OWNED_END - PAGE_SIZE, 2);
	assert(validate_owned(ranges, 4) == -ERANGE);
	ranges[3] = descriptor(7, OWNED_END - PAGE_SIZE, 1);
	assert(validate_owned(ranges, 4) == 0);
	ranges[0] = ranges[3];
	assert(validate_owned(ranges, 2) == -ENOEXEC);
}

static void test_descriptor_profile(void)
{
	struct xnu_x86_efi_memory_range range = descriptor(7, FIRST_USABLE, 2);
	assert(validate(&range, 1, OWNED_START, OWNED_END, FIRST_USABLE, 2, 0) == -EOPNOTSUPP);
	range.Attribute = 1ULL << 63;
	assert(validate_owned(&range, 1) == -EOPNOTSUPP);
	range.Attribute = 0;
	range.VirtualStart = 0x100000;
	assert(validate_owned(&range, 1) == -EOPNOTSUPP);
	range.VirtualStart = 0;
	range.Pad = 1;
	assert(validate_owned(&range, 1) == -EOPNOTSUPP);
	range.Pad = 0;
	range.Type = 3; /* XNU normally treats boot-services code as conventional. */
	assert(validate_owned(&range, 1) == -EOPNOTSUPP);
	range.Type = 11; /* MMIO */
	assert(validate_owned(&range, 1) == -EOPNOTSUPP);
	range.Type = 0;
	assert(validate_owned(&range, 1) == -ENOEXEC);
	range = descriptor(0, 0, 1);
	assert(validate(&range, 1, 0, OWNED_END, FIRST_USABLE, 1, 0) == -ERANGE);
	range = descriptor(7, UINT32_MAX * PAGE_SIZE, 1);
	assert(validate(&range, 1, OWNED_START, range.PhysicalStart + PAGE_SIZE,
			FIRST_USABLE, 1, 0) == -ERANGE);
}

static void test_region_capacity(void)
{
	struct xnu_x86_efi_memory_range ranges[129];
	unsigned int i;

	for (i = 0; i < 129; i++)
		ranges[i] = descriptor(7, FIRST_USABLE + 2 * i * PAGE_SIZE, 1);
	assert(validate_owned(ranges, 128) == 0);
	assert(validate_owned(ranges, 129) == -E2BIG);
}

int main(void)
{
	test_prefix_and_order();
	test_descriptor_profile();
	test_region_capacity();
	puts("x86_64 owned EFI map fixtures passed");
	return 0;
}
