// SPDX-License-Identifier: APSL-2.0
#include "x86_64_boot_layout.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#define PAGE_BYTES UINT64_C(4096)
#define DEFAULT_NKPT 500U
#define FIXED_BYTES ((DEFAULT_NKPT + 6) * PAGE_BYTES)
#define MIN_BYTES (FIXED_BYTES + 9 * PAGE_BYTES)
#define MAP_LIMIT (DEFAULT_NKPT * 512 * PAGE_BYTES)
#define GIB_BYTES (1ULL << 30)

static struct xnu_x86_boot_args boot_args(void)
{
	return (struct xnu_x86_boot_args) {
		.Version = XNU_X86_BOOT_ARGS_VERSION,
		.efiMode = XNU_X86_BOOT_ARGS_EFI_MODE_64,
		.kaddr = 0x200000,
		.ksize = 0x200001,
		.MemoryMap = 0x101000,
		.MemoryMapSize = sizeof(struct xnu_x86_efi_memory_range),
		.MemoryMapDescriptorSize = sizeof(struct xnu_x86_efi_memory_range),
		.deviceTreeP = 0x101028,
		.deviceTreeLength = 0x100,
		.PhysicalMemorySize = 0x40000000,
	};
}

static struct xnu_x86_boot_layout boot_layout(void)
{
	return (struct xnu_x86_boot_layout) {
		.carveout_pa = 0x100000,
		.carveout_size = 0x600000,
		.boot_args_pa = 0x100000,
		.bootstrap_bytes = 0x210000,
		.hib_bytes = 0x200000,
		.idle_kernel_pt_pages = DEFAULT_NKPT,
	};
}

static void test_placement(void)
{
	struct xnu_x86_boot_args args = boot_args();
	struct xnu_x86_boot_layout layout = boot_layout();

	layout.carveout_size = 0x511000; /* Exact end after rounding image size. */
	assert(xnu_x86_boot_layout_validate(&args, &layout) == 0);
	layout.carveout_size -= PAGE_BYTES;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);

	layout = boot_layout();
	args.deviceTreeP -= 4; /* Aligned, but overlaps the EFI descriptor. */
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);
	args.deviceTreeP = (uint32_t)layout.boot_args_pa;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);
	args.deviceTreeP = args.kaddr - 4; /* Crosses into the image. */
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);
	args.deviceTreeP = 0x400080; /* In the same page as the image tail. */
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);
	args.deviceTreeP = 0xff000; /* Outside the owned reservation. */
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);
}

static void test_alignment_and_zero(void)
{
	struct xnu_x86_boot_args args = boot_args();
	struct xnu_x86_boot_layout layout = boot_layout();

	assert(xnu_x86_boot_layout_validate(NULL, &layout) == -EINVAL);
	assert(xnu_x86_boot_layout_validate(&args, NULL) == -EINVAL);
	layout.boot_args_pa++;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -EINVAL);
	layout = boot_layout();
	layout.bootstrap_bytes = 0;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -EINVAL);
	layout = boot_layout();
	args.deviceTreeP++;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -EINVAL);
	args = boot_args();
	args.MemoryMapSize++;
	args.MemoryMapDescriptorSize++;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -EINVAL);
	args = boot_args();
	assert(xnu_x86_boot_layout_validate(&args, &layout) == 0);
}

static void test_table_and_address_limits(void)
{
	struct xnu_x86_boot_args args = boot_args();
	struct xnu_x86_boot_layout layout = boot_layout();

	layout.bootstrap_bytes = MIN_BYTES;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == 0);
	args.PhysicalMemorySize++;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);
	args = boot_args();
	layout.bootstrap_bytes = MIN_BYTES - PAGE_BYTES;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);
	layout.bootstrap_bytes = 0x400000;
	layout.carveout_size = 0x100000000ULL;
	args.kaddr = MAP_LIMIT - FIXED_BYTES - PAGE_BYTES;
	args.ksize = PAGE_BYTES;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == 0);
	args.ksize++;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);

	layout.idle_kernel_pt_pages = 2048;
	layout.bootstrap_bytes = 0xa00000;
	args.kaddr = 0xff5fe000;
	args.ksize = PAGE_BYTES;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == 0);
	args.ksize++; /* The bootstrap end now lands at 4 GiB. */
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);
	layout.idle_kernel_pt_pages++;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -EINVAL);

	args = boot_args();
	layout = boot_layout();
	layout.carveout_size = UINT64_MAX & ~(PAGE_BYTES - 1);
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);
}

static void test_bootstrap_budget_components(void)
{
	struct xnu_x86_boot_args args = boot_args();
	struct xnu_x86_boot_layout layout = boot_layout();

	layout.bootstrap_bytes = MIN_BYTES;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == 0);
	layout.hib_bytes++;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);
	layout.bootstrap_bytes += PAGE_BYTES;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == 0);
	layout.hib_bytes = 0;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -EINVAL);

	layout = boot_layout();
	layout.carveout_size = 0x4000000;
	args.PhysicalMemorySize = 253 * GIB_BYTES;
	/* 257 L2 pages fill the first L3 at the maximum entropy offset. */
	layout.bootstrap_bytes = FIXED_BYTES + (257 + 1 + 3) * PAGE_BYTES;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == 0);
	layout.bootstrap_bytes += PAGE_BYTES;
	args.PhysicalMemorySize++;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);
	layout.bootstrap_bytes += PAGE_BYTES;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == 0);

	layout.bootstrap_bytes = 0x2000000;
	args.PhysicalMemorySize = (14 * 512 - 255 - 4) * GIB_BYTES;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == 0);
	args.PhysicalMemorySize++;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);

	args = boot_args();
	layout.hib_bytes = GIB_BYTES;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == 0);
	layout.hib_bytes++;
	assert(xnu_x86_boot_layout_validate(&args, &layout) == -ERANGE);
}

int main(void)
{
	test_placement();
	test_alignment_and_zero();
	test_table_and_address_limits();
	test_bootstrap_budget_components();
	puts("xnu x86 boot layout fixtures passed");
	return 0;
}
