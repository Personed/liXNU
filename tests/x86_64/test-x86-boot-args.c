// SPDX-License-Identifier: APSL-2.0
#include "../../xnu-ko/x86_64_boot_args.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(struct xnu_x86_efi_memory_range) == 40,
	       "XNU EFI memory descriptor layout");
_Static_assert(offsetof(struct xnu_x86_boot_args, CommandLine) == 8,
	       "boot_args CommandLine offset");
_Static_assert(offsetof(struct xnu_x86_boot_args, MemoryMap) == 1032,
	       "boot_args MemoryMap offset");
_Static_assert(offsetof(struct xnu_x86_boot_args, VideoV1) == 1048,
	       "boot_args VideoV1 offset");
_Static_assert(offsetof(struct xnu_x86_boot_args, deviceTreeP) == 1072,
	       "boot_args deviceTreeP offset");
_Static_assert(offsetof(struct xnu_x86_boot_args, kaddr) == 1080,
	       "boot_args kaddr offset");
_Static_assert(offsetof(struct xnu_x86_boot_args, PhysicalMemorySize) == 1144,
	       "boot_args PhysicalMemorySize offset");
_Static_assert(offsetof(struct xnu_x86_boot_args, Video) == 1192,
	       "boot_args Video offset");
_Static_assert(offsetof(struct xnu_x86_boot_args, KC_hdrs_vaddr) == 1256,
	       "boot_args KC header offset");
_Static_assert(offsetof(struct xnu_x86_boot_args, __reserved4) == 1328,
	       "boot_args reserved offset");
_Static_assert(sizeof(struct xnu_x86_boot_args) == 4096,
	       "XNU x86 boot_args size");

static struct xnu_x86_boot_args_core valid_input(void)
{
	struct xnu_x86_boot_args_core in = {
		.command_line = "debug=0x144 keepsyms=1",
		.command_line_length = sizeof("debug=0x144 keepsyms=1") - 1,
		.memory_map_pa = 0x00100000,
		.memory_map_size = 2 * sizeof(struct xnu_x86_efi_memory_range),
		.memory_map_descriptor_size = sizeof(struct xnu_x86_efi_memory_range),
		.memory_map_descriptor_version = 0,
		.device_tree_pa = 0x00200000,
		.device_tree_size = 0x1000,
		.kernel_pa = 0x00400000,
		.kernel_size = 0x00200000,
		.boot_mem_start = 0x00600000,
		.boot_mem_size = 0x00100000,
		.physical_memory_size = 0x100000000ULL,
		.fsb_frequency = 100000000ULL,
	};

	return in;
}

static void test_valid_core_fields(void)
{
	struct xnu_x86_boot_args args;
	struct xnu_x86_boot_args_core in = valid_input();

	assert(xnu_x86_boot_args_build_core(&args, &in) == 0);
	assert(args.Revision == XNU_X86_BOOT_ARGS_REVISION);
	assert(args.Version == XNU_X86_BOOT_ARGS_VERSION);
	assert(args.efiMode == XNU_X86_BOOT_ARGS_EFI_MODE_64);
	assert(strcmp(args.CommandLine, in.command_line) == 0);
	assert(args.MemoryMap == in.memory_map_pa);
	assert(args.MemoryMapSize == in.memory_map_size);
	assert(args.MemoryMapDescriptorSize == in.memory_map_descriptor_size);
	assert(args.deviceTreeP == in.device_tree_pa);
	assert(args.deviceTreeLength == in.device_tree_size);
	assert(args.kaddr == in.kernel_pa);
	assert(args.ksize == in.kernel_size);
	assert(args.bootMemStart == in.boot_mem_start);
	assert(args.bootMemSize == in.boot_mem_size);
	assert(args.PhysicalMemorySize == in.physical_memory_size);
	assert(args.FSBFrequency == in.fsb_frequency);
	assert(args.Video.v_baseAddr == 0);
	assert(args.efiSystemTable == 0);
	assert(xnu_x86_boot_args_validate(&args) == 0);

	args.Revision = XNU_X86_BOOT_ARGS_MAX_REVISION;
	assert(xnu_x86_boot_args_validate(&args) == 0);
}

static void test_malformed_lengths(void)
{
	struct xnu_x86_boot_args args;
	struct xnu_x86_boot_args_core in = valid_input();

	in.memory_map_size--;
	assert(xnu_x86_boot_args_build_core(&args, &in) == -EINVAL);

	in = valid_input();
	in.memory_map_descriptor_size = sizeof(struct xnu_x86_efi_memory_range) - 1;
	assert(xnu_x86_boot_args_build_core(&args, &in) == -EINVAL);

	in = valid_input();
	in.memory_map_descriptor_size = 0;
	assert(xnu_x86_boot_args_build_core(&args, &in) == -EINVAL);

	in = valid_input();
	in.command_line_length = XNU_X86_BOOT_ARGS_COMMAND_LINE_SIZE;
	assert(xnu_x86_boot_args_build_core(&args, &in) == -EINVAL);
}

static void test_unrepresentable_ranges(void)
{
	struct xnu_x86_boot_args args;
	struct xnu_x86_boot_args_core in = valid_input();

	in.memory_map_pa = 0x100000000ULL;
	assert(xnu_x86_boot_args_build_core(&args, &in) == -ERANGE);

	in = valid_input();
	in.memory_map_pa = 0xfffffff0ULL;
	in.memory_map_size = 80;
	assert(xnu_x86_boot_args_build_core(&args, &in) == -ERANGE);

	in = valid_input();
	in.device_tree_pa = 0xfffffff0ULL;
	in.device_tree_size = 0x1000;
	assert(xnu_x86_boot_args_build_core(&args, &in) == -ERANGE);

	in = valid_input();
	in.kernel_pa = 0xfff00000ULL;
	in.kernel_size = 0x00200000;
	assert(xnu_x86_boot_args_build_core(&args, &in) == -ERANGE);

	in = valid_input();
	in.boot_mem_start = UINT64_MAX - 7;
	in.boot_mem_size = 8;
	assert(xnu_x86_boot_args_build_core(&args, &in) == -ERANGE);
}

static void test_last_u32_page_ranges(void)
{
	struct xnu_x86_boot_args args;
	struct xnu_x86_boot_args_core in = valid_input();

	/* The u32 base plus length describes every byte through UINT32_MAX. */
	in.memory_map_pa = 0xfffff000ULL;
	in.memory_map_size = 0x1000;
	in.memory_map_descriptor_size = 0x1000;
	in.device_tree_pa = 0xfffff000ULL;
	in.device_tree_size = 0x1000;
	assert(xnu_x86_boot_args_build_core(&args, &in) == 0);
	assert(args.MemoryMap == 0xfffff000U);
	assert(args.MemoryMapSize == 0x1000U);
	assert(args.deviceTreeP == 0xfffff000U);
	assert(args.deviceTreeLength == 0x1000U);
	assert(xnu_x86_boot_args_validate(&args) == 0);

	/* XNU computes this sum in uint32_t, so the same kernel interval wraps. */
	in = valid_input();
	in.kernel_pa = 0xfffff000ULL;
	in.kernel_size = 0x1000;
	assert(xnu_x86_boot_args_build_core(&args, &in) == -ERANGE);
}

static void test_validator_rejects_u32_overflow(void)
{
	struct xnu_x86_boot_args args;
	struct xnu_x86_boot_args_core in = valid_input();

	assert(xnu_x86_boot_args_build_core(&args, &in) == 0);
	args.MemoryMap = 0xfffff000U;
	args.MemoryMapSize = 0x2000U;
	args.MemoryMapDescriptorSize = 0x1000U;
	assert(xnu_x86_boot_args_validate(&args) == -ERANGE);

	assert(xnu_x86_boot_args_build_core(&args, &in) == 0);
	args.kaddr = 0xfffff000U;
	args.ksize = 0x1000U;
	assert(xnu_x86_boot_args_validate(&args) == -ERANGE);
}

static void test_reject_unsupported_or_malformed_output(void)
{
	struct xnu_x86_boot_args args;
	struct xnu_x86_boot_args_core in = valid_input();

	assert(xnu_x86_boot_args_build_core(&args, &in) == 0);
	args.Version++;
	assert(xnu_x86_boot_args_validate(&args) == -EOPNOTSUPP);

	assert(xnu_x86_boot_args_build_core(&args, &in) == 0);
	args.efiMode = 32;
	assert(xnu_x86_boot_args_validate(&args) == -EOPNOTSUPP);

	assert(xnu_x86_boot_args_build_core(&args, &in) == 0);
	memset(args.CommandLine, 'x', sizeof(args.CommandLine));
	assert(xnu_x86_boot_args_validate(&args) == -ENOEXEC);

	assert(xnu_x86_boot_args_build_core(&args, &in) == 0);
	args.MemoryMapDescriptorSize = 0;
	assert(xnu_x86_boot_args_validate(&args) == -ENOEXEC);
	assert(xnu_x86_boot_args_validate(NULL) == -EINVAL);
}

int main(void)
{
	test_valid_core_fields();
	test_malformed_lengths();
	test_unrepresentable_ranges();
	test_last_u32_page_ranges();
	test_validator_rejects_u32_overflow();
	test_reject_unsupported_or_malformed_output();
	puts("xnu x86 boot_args layout/validation fixtures passed");
	return 0;
}
