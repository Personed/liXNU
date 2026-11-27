/* SPDX-License-Identifier: APSL-2.0 */
#pragma once

/*
 * Parse-only x86_64 Mach-O reader. This API deliberately has no loader,
 * allocator, rebase, or XNU entry operation. Values match the public XNU
 * headers pinned at xnu-12377.121.6.
 */
typedef unsigned int xnu_macho_u32;
typedef unsigned short xnu_macho_u16;
typedef unsigned long long xnu_macho_u64;

#define XNU_MACHO_X86_64_PARSE_EINVAL 22
#define XNU_MACHO_X86_64_PARSE_ENOEXEC 8
#define XNU_MACHO_X86_64_PARSE_EOPNOTSUPP 95

/* Bounded plan keeps the in-kernel result object below a 2 KiB stack frame. */
#define XNU_MACHO_X86_64_MAX_SEGMENTS 24U

#define XNU_MACHO_X86_64_HAS_SYMTAB (1U << 0)
#define XNU_MACHO_X86_64_HAS_DYLD_INFO (1U << 1)
#define XNU_MACHO_X86_64_HAS_CHAINED_FIXUPS (1U << 2)
#define XNU_MACHO_X86_64_HAS_UNINTERPRETED_LINKEDIT (1U << 3)
#define XNU_MACHO_X86_64_HAS_UNCLASSIFIED_COMMANDS (1U << 4)
#define XNU_MACHO_X86_64_IS_FILESET (1U << 5)
#define XNU_MACHO_X86_64_HAS_KERNEL_ENTRY (1U << 6)

struct xnu_macho_x86_64_segment {
	unsigned char name[16];
	xnu_macho_u64 vmaddr;
	xnu_macho_u64 vmsize;
	xnu_macho_u64 fileoff;
	xnu_macho_u64 filesize;
	xnu_macho_u32 maxprot;
	xnu_macho_u32 initprot;
	xnu_macho_u32 flags;
	xnu_macho_u32 section_count;
	/* Zero when no chained-starts record describes this segment. */
	xnu_macho_u32 chained_pointer_format;
	xnu_macho_u16 chained_page_size;
	xnu_macho_u16 chained_page_count;
};

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
	/* Counts from dyld_chained_starts_in_image, zero without that command. */
	xnu_macho_u32 chained_starts_segment_count;
	xnu_macho_u32 chained_starts_nonempty_segment_count;
	/* Root header identity and nested fileset metadata; never a load plan. */
	xnu_macho_u32 file_type;
	xnu_macho_u32 cpu_subtype;
	xnu_macho_u32 load_command_count;
	xnu_macho_u32 load_command_bytes;
	xnu_macho_u32 fileset_entry_count;
	xnu_macho_u32 kernel_entry_filetype;
	xnu_macho_u32 kernel_entry_cpusubtype;
	xnu_macho_u32 kernel_entry_ncmds;
	xnu_macho_u32 kernel_entry_sizeofcmds;
	xnu_macho_u64 kernel_entry_vmaddr;
	xnu_macho_u64 kernel_entry_fileoff;
	struct xnu_macho_x86_64_segment segments[XNU_MACHO_X86_64_MAX_SEGMENTS];
};

/*
 * Return 0 for the metadata inspection subset, including bounded outer and
 * embedded MH_FILESET header/load-command framing for exactly one
 * com.apple.kernel entry. Fileset metadata is not validated for loading.
 * Return -EOPNOTSUPP for a required command, ABI, segment count, page size, or
 * pointer format outside that subset and -ENOEXEC for malformed input. The
 * output contains at most XNU_MACHO_X86_64_MAX_SEGMENTS descriptors in Mach-O
 * command order. Chained-
 * fixup headers and starts tables are structurally validated and summarized,
 * never interpreted or applied. For nonzero import counts, a nonempty symbols
 * pool after the header is required and the import table extent is checked;
 * individual import name offsets and string contents are not validated.
 * Dyld page-count prefixes are preserved; x86_64 MULTI/LAST starts are
 * unsupported. Nonzero max_valid_pointer values must agree across nonempty
 * starts records. The sole segment-count exception accepts one zero-VM
 * segment immediately before a final __LINKEDIT only when its starts slot is
 * empty. A file-backed CTF exception requires a zero-VM SG_NORELOC segment
 * with read-only protections and one bounded (__CTF,__ctf) section; it is not
 * included in mapped VM counts.
 * Dyld-info extents are only bounds-checked and flagged. Unknown optional
 * commands are counted. Callers must not infer loader/fixup support from
 * success. On every failure with a non-NULL output pointer, zero the complete
 * output.
 */
int xnu_macho_x86_64_parse(const void *file, unsigned long file_size,
			   struct xnu_macho_x86_64_info *out);
