/* SPDX-License-Identifier: APSL-2.0 */
#pragma once

/*
 * Parse-only x86_64 Mach-O reader. This API deliberately has no loader,
 * allocator, rebase, or XNU entry operation. Values match the public XNU
 * headers pinned at xnu-12377.121.6.
 */
typedef unsigned int xnu_macho_u32;
typedef unsigned long long xnu_macho_u64;

#define XNU_MACHO_X86_64_PARSE_EINVAL 22
#define XNU_MACHO_X86_64_PARSE_ENOEXEC 8
#define XNU_MACHO_X86_64_PARSE_EOPNOTSUPP 95

#define XNU_MACHO_X86_64_HAS_SYMTAB (1U << 0)
#define XNU_MACHO_X86_64_HAS_DYLD_INFO (1U << 1)
#define XNU_MACHO_X86_64_HAS_CHAINED_FIXUPS (1U << 2)
#define XNU_MACHO_X86_64_HAS_UNINTERPRETED_LINKEDIT (1U << 3)
#define XNU_MACHO_X86_64_HAS_UNCLASSIFIED_COMMANDS (1U << 4)

struct xnu_macho_x86_64_info {
	xnu_macho_u64 vm_low;
	xnu_macho_u64 vm_high;
	xnu_macho_u64 entry;
	xnu_macho_u32 segment_count;
	/* Nonzero mapped segments; excludes the __PAGEZERO reservation. */
	xnu_macho_u32 mapped_segment_count;
	/* Metadata only: success never means the image is loadable or fixups work. */
	xnu_macho_u32 inspection_flags;
	xnu_macho_u32 unclassified_load_command_count;
	xnu_macho_u32 linkedit_data_command_count;
	xnu_macho_u32 reserved;
};

/*
 * Return 0 for the metadata inspection subset. Return -EOPNOTSUPP for a
 * required command or ABI outside that subset and -ENOEXEC for malformed
 * input. Chained-fixup and dyld-info extents are bounds-checked and flagged,
 * never interpreted. Unknown optional commands are counted. Callers must not
 * infer loader/fixup support from success. On every failure with a non-NULL
 * output pointer, zero the complete output.
 */
int xnu_macho_x86_64_parse(const void *file, unsigned long file_size,
			   struct xnu_macho_x86_64_info *out);
