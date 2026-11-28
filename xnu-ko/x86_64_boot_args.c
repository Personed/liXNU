// SPDX-License-Identifier: APSL-2.0
/* Source-pinned, validation-only x86_64 boot_args core-field helper. */
#include "x86_64_boot_args.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define XNU_X86_U32_LIMIT ((xnu_boot_u64)0xffffffffU)
#define XNU_X86_U32_END   (XNU_X86_U32_LIMIT + 1)
#define XNU_X86_LAST_BOOT_PAGE (XNU_X86_U32_END - 4096U)
#define XNU_X86_EFI_PAGE_SHIFT 12U
#define XNU_X86_EFI_PAGE_MASK  ((1U << XNU_X86_EFI_PAGE_SHIFT) - 1)
#define XNU_X86_ADT_MAX_DEPTH  32U
#define XNU_X86_ADT_PROP_NAME 32U
#define XNU_X86_ADT_NODE_NAME 64U

/* A u32 physical base plus a u32 length may end exactly at 4 GiB. */
static int pa_range_fits_u32(xnu_boot_u64 start, xnu_boot_u64 size)
{
	if (!start || !size || start > XNU_X86_U32_LIMIT ||
	    size > XNU_X86_U32_END - start)
		return 0;
	return 1;
}

/*
 * vstart() adds kaddr + ksize in uint32_t and rounds physfree up to a
 * 4 KiB page. ID_MAP_VTOP() later keeps only the low 32 bits.
 */
static int kernel_range_fits_u32(xnu_boot_u64 start, xnu_boot_u64 size)
{
	if (!start || !size || start > XNU_X86_LAST_BOOT_PAGE ||
	    size > XNU_X86_LAST_BOOT_PAGE - start)
		return 0;
	return 1;
}

static int terminated_command_line(const char *s, unsigned int max)
{
	unsigned int i;

	for (i = 0; i < max; i++)
		if (!s[i])
			return 1;
	return 0;
}

int xnu_x86_boot_args_validate(const struct xnu_x86_boot_args *args)
{
	xnu_boot_u64 map_end;

	if (!args)
		return -EINVAL;
	if (args->Version != XNU_X86_BOOT_ARGS_VERSION ||
	    args->Revision > XNU_X86_BOOT_ARGS_MAX_REVISION ||
	    args->efiMode != XNU_X86_BOOT_ARGS_EFI_MODE_64)
		return -EOPNOTSUPP;
	if (!terminated_command_line(args->CommandLine,
				     XNU_X86_BOOT_ARGS_COMMAND_LINE_SIZE))
		return -ENOEXEC;
	if (!args->MemoryMap || !args->MemoryMapSize ||
	    args->MemoryMapDescriptorSize < sizeof(struct xnu_x86_efi_memory_range) ||
	    args->MemoryMapSize % args->MemoryMapDescriptorSize)
		return -ENOEXEC;
	map_end = (xnu_boot_u64)args->MemoryMap + args->MemoryMapSize;
	if (map_end > XNU_X86_U32_END)
		return -ERANGE;
	if (!pa_range_fits_u32(args->deviceTreeP, args->deviceTreeLength) ||
	    !kernel_range_fits_u32(args->kaddr, args->ksize))
		return -ERANGE;
	if (args->bootMemStart > ~(xnu_boot_u64)0 - args->bootMemSize)
		return -ERANGE;
	return 0;
}

int xnu_x86_boot_memory_map_validate(const struct xnu_x86_boot_args *args,
				     const void *map, unsigned long map_size)
{
	const unsigned char *bytes = map;
	unsigned long offset;
	int err;

	if (!map)
		return -EINVAL;
	err = xnu_x86_boot_args_validate(args);
	if (err)
		return err;
	if (map_size != args->MemoryMapSize)
		return -ENOEXEC;

	for (offset = 0; offset < map_size; offset += args->MemoryMapDescriptorSize) {
		struct xnu_x86_efi_memory_range range;
		xnu_boot_u64 first_page;

		/* Firmware strides can include padding. The caller's buffer can be unaligned. */
		memcpy(&range, bytes + offset, sizeof(range));
		if (!range.NumberOfPages || (range.PhysicalStart & XNU_X86_EFI_PAGE_MASK))
			return -ENOEXEC;
		first_page = range.PhysicalStart >> XNU_X86_EFI_PAGE_SHIFT;
		/* i386_vm_init() narrows both inclusive page indices to ppnum_t. */
		if (first_page > XNU_X86_U32_LIMIT ||
		    range.NumberOfPages - 1 > XNU_X86_U32_LIMIT - first_page)
			return -ERANGE;
	}
	return 0;
}

/* Flattened DeviceTreeNode and DeviceTreeNodeProperty from XNU's device_tree.h. */
int xnu_x86_boot_device_tree_validate(const struct xnu_x86_boot_args *args,
				      const void *tree, unsigned long tree_size)
{
	const unsigned char *bytes = tree;
	xnu_boot_u32 pending[XNU_X86_ADT_MAX_DEPTH] = { 1 };
	unsigned int depth = 1;
	unsigned long offset = 0;
	int err;

	if (!tree)
		return -EINVAL;
	err = xnu_x86_boot_args_validate(args);
	if (err)
		return err;
	if (tree_size != args->deviceTreeLength)
		return -ENOEXEC;

	while (depth) {
		xnu_boot_u32 node[2];
		xnu_boot_u32 i;
		int has_name = 0;

		if (!pending[depth - 1]) {
			depth--;
			continue;
		}
		pending[depth - 1]--;
		if (tree_size - offset < sizeof(node))
			return -ENOEXEC;
		memcpy(node, bytes + offset, sizeof(node));
		offset += sizeof(node);

		for (i = 0; i < node[0]; i++) {
			const unsigned char *name = bytes + offset;
			xnu_boot_u32 length, padding;

			if (tree_size - offset < XNU_X86_ADT_PROP_NAME + sizeof(length))
				return -ENOEXEC;
			if (!name[0] || !memchr(name, 0, XNU_X86_ADT_PROP_NAME))
				return -ENOEXEC;
			memcpy(&length, name + XNU_X86_ADT_PROP_NAME, sizeof(length));
			offset += XNU_X86_ADT_PROP_NAME + sizeof(length);
			padding = (4U - (length & 3U)) & 3U;
			if (length > tree_size - offset ||
			    padding > tree_size - offset - length)
				return -ENOEXEC;

			if (!memcmp(name, "name", sizeof("name"))) {
				const unsigned char *value = bytes + offset;

				if (has_name || !length || length > XNU_X86_ADT_NODE_NAME ||
				    !value[0] || !memchr(value, 0, length))
					return -ENOEXEC;
				has_name = 1;
			}
			offset += (unsigned long)length + padding;
		}
		if (!has_name || node[1] > (tree_size - offset) / sizeof(node))
			return -ENOEXEC;
		if (node[1]) {
			if (depth == XNU_X86_ADT_MAX_DEPTH)
				return -EOPNOTSUPP;
			pending[depth++] = node[1];
		}
	}
	return offset == tree_size ? 0 : -ENOEXEC;
}

int xnu_x86_boot_args_build_core(struct xnu_x86_boot_args *out,
				 const struct xnu_x86_boot_args_core *in)
{
	int err;

	if (!out || !in || !in->command_line ||
	    in->command_line_length >= XNU_X86_BOOT_ARGS_COMMAND_LINE_SIZE)
		return -EINVAL;
	if (!pa_range_fits_u32(in->memory_map_pa, in->memory_map_size) ||
	    !pa_range_fits_u32(in->device_tree_pa, in->device_tree_size) ||
	    !kernel_range_fits_u32(in->kernel_pa, in->kernel_size))
		return -ERANGE;
	if (in->memory_map_size > XNU_X86_U32_LIMIT ||
	    in->memory_map_descriptor_size > XNU_X86_U32_LIMIT ||
	    in->memory_map_descriptor_version > XNU_X86_U32_LIMIT ||
	    in->device_tree_size > XNU_X86_U32_LIMIT ||
	    in->kernel_size > XNU_X86_U32_LIMIT)
		return -ERANGE;
	if (in->memory_map_descriptor_size < sizeof(struct xnu_x86_efi_memory_range) ||
	    in->memory_map_size % in->memory_map_descriptor_size)
		return -EINVAL;
	if (in->boot_mem_start > ~(xnu_boot_u64)0 - in->boot_mem_size)
		return -ERANGE;

	memset(out, 0, sizeof(*out));
	out->Revision = XNU_X86_BOOT_ARGS_REVISION;
	out->Version = XNU_X86_BOOT_ARGS_VERSION;
	out->efiMode = XNU_X86_BOOT_ARGS_EFI_MODE_64;
	if (in->command_line_length)
		memcpy(out->CommandLine, in->command_line,
		       in->command_line_length);
	out->CommandLine[in->command_line_length] = '\0';
	out->MemoryMap = (xnu_boot_u32)in->memory_map_pa;
	out->MemoryMapSize = (xnu_boot_u32)in->memory_map_size;
	out->MemoryMapDescriptorSize =
		(xnu_boot_u32)in->memory_map_descriptor_size;
	out->MemoryMapDescriptorVersion =
		(xnu_boot_u32)in->memory_map_descriptor_version;
	out->deviceTreeP = (xnu_boot_u32)in->device_tree_pa;
	out->deviceTreeLength = (xnu_boot_u32)in->device_tree_size;
	out->kaddr = (xnu_boot_u32)in->kernel_pa;
	out->ksize = (xnu_boot_u32)in->kernel_size;
	out->bootMemStart = in->boot_mem_start;
	out->bootMemSize = in->boot_mem_size;
	out->PhysicalMemorySize = in->physical_memory_size;
	out->FSBFrequency = in->fsb_frequency;

	err = xnu_x86_boot_args_validate(out);
	if (err)
		memset(out, 0, sizeof(*out));
	return err;
}
