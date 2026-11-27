// SPDX-License-Identifier: APSL-2.0
#include "x86_64_boot_layout.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

/* XNU xnu-12377.121.6, osfmk/i386/pmap.h and Idle_PTs_init(). */
#define BOOT_PAGE_SIZE 4096ULL
#define BOOT_PAGE_MASK (BOOT_PAGE_SIZE - 1)
#define BOOT_PTE_PER_PAGE 512ULL
#define BOOT_PD_PAGES 4ULL
#define BOOT_LAST_PA 0xffffffffULL
#define BOOT_GIB (1ULL << 30)
#define BOOT_MAX_MEMORY (2048ULL * BOOT_GIB) /* K64_MAXMEM in vm_param.h */
#define BOOT_LARGE_PAGE_SIZE (1ULL << 21)
#define BOOT_PHYSMAP_MAX_OFFSET 255ULL

struct boot_span {
	xnu_boot_u64 start;
	xnu_boot_u64 size;
};

int xnu_x86_boot_layout_validate(const struct xnu_x86_boot_args *args,
				 const struct xnu_x86_boot_layout *layout)
{
	xnu_boot_u64 carveout_end, kernel_end, first_free, fixed_bytes, mapped_limit;
	xnu_boot_u64 physmap_l2_pages, physmap_l3_pages, dblmap_l1_pages, table_bytes;
	unsigned int i, j;
	int err;

	if (!layout)
		return -EINVAL;
	err = xnu_x86_boot_args_validate(args);
	if (err)
		return err;

	if (!layout->carveout_size || !layout->boot_args_pa ||
	    !layout->bootstrap_bytes || !layout->idle_kernel_pt_pages ||
	    !layout->hib_bytes || !args->PhysicalMemorySize ||
	    layout->idle_kernel_pt_pages > BOOT_PD_PAGES * BOOT_PTE_PER_PAGE)
		return -EINVAL;
	if ((layout->carveout_pa | layout->carveout_size | layout->boot_args_pa |
	     args->kaddr | layout->bootstrap_bytes) & BOOT_PAGE_MASK)
		return -EINVAL;
	/* XNU walks native EFI descriptors and aligns absolute ADT addresses. */
	if (((args->MemoryMap | args->MemoryMapDescriptorSize) & 7U) ||
	    (args->deviceTreeP & 3U))
		return -EINVAL;
	if (layout->carveout_size > ~(xnu_boot_u64)0 - layout->carveout_pa)
		return -ERANGE;
	if (args->PhysicalMemorySize > BOOT_MAX_MEMORY)
		return -ERANGE;
	carveout_end = layout->carveout_pa + layout->carveout_size;

	kernel_end = (xnu_boot_u64)args->kaddr + args->ksize;
	first_free = (kernel_end + BOOT_PAGE_MASK) & ~BOOT_PAGE_MASK;
	if (first_free > BOOT_LAST_PA ||
	    layout->bootstrap_bytes > BOOT_LAST_PA - first_free)
		return -ERANGE;

	/* KPTphys, IdlePTD, IdlePDPT, IdlePML4, before fillkpt(). */
	fixed_bytes = (layout->idle_kernel_pt_pages + BOOT_PD_PAGES + 2) *
		      BOOT_PAGE_SIZE;
	mapped_limit = layout->idle_kernel_pt_pages * BOOT_PTE_PER_PAGE *
		       BOOT_PAGE_SIZE;
	/*
	 * One L2 page per GiB, including the 4 GiB MMIO allowance. The
	 * first L3 can start as late as index 255. The K64_MAXMEM bound
	 * above keeps the physmap inside the pinned 14-slot capacity.
	 */
	physmap_l2_pages = args->PhysicalMemorySize / BOOT_GIB + 4;
	if (args->PhysicalMemorySize % BOOT_GIB)
		physmap_l2_pages++;
	physmap_l3_pages = (physmap_l2_pages + BOOT_PHYSMAP_MAX_OFFSET +
			  BOOT_PTE_PER_PAGE - 1) / BOOT_PTE_PER_PAGE;
	/* doublemap_init(): one L3 and L2, one L1 per 2 MiB of __HIB. */
	dblmap_l1_pages = layout->hib_bytes / BOOT_LARGE_PAGE_SIZE;
	if (layout->hib_bytes % BOOT_LARGE_PAGE_SIZE)
		dblmap_l1_pages++;
	if (dblmap_l1_pages > BOOT_PTE_PER_PAGE)
		return -ERANGE;

	table_bytes = fixed_bytes + (physmap_l2_pages + physmap_l3_pages +
				     dblmap_l1_pages + 2) * BOOT_PAGE_SIZE;
	if (layout->bootstrap_bytes < table_bytes ||
	    first_free + fixed_bytes > mapped_limit)
		return -ERANGE;
	if (args->kaddr < layout->carveout_pa ||
	    first_free + layout->bootstrap_bytes > carveout_end)
		return -ERANGE;

	{
		const struct boot_span metadata[] = {
			{ layout->boot_args_pa, sizeof(*args) },
			{ args->MemoryMap, args->MemoryMapSize },
			{ args->deviceTreeP, args->deviceTreeLength },
		};
		const unsigned int count = sizeof(metadata) / sizeof(metadata[0]);

		/* Keep all metadata below the image, on pages mapped by fillkpt. */
		for (i = 0; i < count; i++)
			if (metadata[i].start < layout->carveout_pa ||
			    metadata[i].start >= args->kaddr ||
			    metadata[i].size > args->kaddr - metadata[i].start)
				return -ERANGE;

		for (i = 0; i < count; i++)
			for (j = i + 1; j < count; j++)
				if (metadata[i].start < metadata[j].start + metadata[j].size &&
				    metadata[j].start < metadata[i].start + metadata[i].size)
					return -ERANGE;
	}
	return 0;
}
