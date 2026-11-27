// SPDX-License-Identifier: APSL-2.0
#include "x86_64_boot_memory_map.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

/* Pinned XNU boot.h, efi.h, i386_lowmem.h and pmap.h. */
#define EFI_RESERVED_MEMORY 0U
#define EFI_CONVENTIONAL_MEMORY 7U
#define EFI_DESCRIPTOR_VERSION 1U
#define EFI_PAGE_SHIFT 12U
#define EFI_PAGE_SIZE (1U << EFI_PAGE_SHIFT)
#define EFI_XNU_LOWMEM_PAGES 3U
#define EFI_XNU_MAX_DESCRIPTORS 128U

int xnu_x86_boot_memory_map_validate_owned(const struct xnu_x86_boot_args *args,
					   const void *map, unsigned long map_size,
					   xnu_boot_u64 owned_start, xnu_boot_u64 owned_end,
					   xnu_boot_u64 first_usable)
{
	const unsigned char *bytes = map;
	xnu_boot_u64 previous_end = owned_start;
	unsigned long offset;
	int have_conventional = 0;
	int err;

	if (!args || !map ||
	    ((owned_start | owned_end | first_usable) & (EFI_PAGE_SIZE - 1)) ||
	    first_usable < owned_start || first_usable >= owned_end ||
	    first_usable < EFI_XNU_LOWMEM_PAGES * EFI_PAGE_SIZE)
		return -EINVAL;
	if (args->MemoryMapDescriptorSize &&
	    args->MemoryMapSize / args->MemoryMapDescriptorSize > EFI_XNU_MAX_DESCRIPTORS)
		return -E2BIG;
	err = xnu_x86_boot_memory_map_validate(args, map, map_size);
	if (err)
		return err;
	if (args->MemoryMapDescriptorVersion != EFI_DESCRIPTOR_VERSION)
		return -EOPNOTSUPP;

	for (offset = 0; offset < map_size; offset += args->MemoryMapDescriptorSize) {
		struct xnu_x86_efi_memory_range range;
		xnu_boot_u64 end;

		memcpy(&range, bytes + offset, sizeof(range));
		/* The shape check limits both inclusive page indices to uint32_t. */
		end = range.PhysicalStart + (range.NumberOfPages << EFI_PAGE_SHIFT);
		/* i386_vm_init ignores the first and last representable pages. */
		if (!range.PhysicalStart || (end >> EFI_PAGE_SHIFT) > 0xffffffffU ||
		    range.PhysicalStart < owned_start || end > owned_end)
			return -ERANGE;
		if (range.PhysicalStart < previous_end)
			return -ENOEXEC;
		previous_end = end;
		if (range.Pad || range.VirtualStart || range.Attribute)
			return -EOPNOTSUPP;
		if (range.Type == EFI_RESERVED_MEMORY)
			continue;
		if (range.Type != EFI_CONVENTIONAL_MEMORY)
			return -EOPNOTSUPP;
		/* A range with base < fap and top == fap is treated as all usable. */
		if (range.PhysicalStart < first_usable)
			return -ERANGE;
		have_conventional = 1;
	}

	return have_conventional ? 0 : -ENOEXEC;
}
