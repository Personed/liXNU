// SPDX-License-Identifier: APSL-2.0
/*
 * Strict, parse-only x86_64 Mach-O subset. Keep this object out of xnu.ko:
 * parsing does not establish a safe Linux/XNU execution boundary.
 *
 * ABI references (pinned public XNU headers):
 *   osfmk/mach/machine.h: CPU_TYPE_X86_64, CPU_SUBTYPE_X86_64_{ALL,H}
 *   EXTERNAL_HEADERS/mach-o/loader.h: Mach-O commands, fileset entries, and segments
 *   EXTERNAL_HEADERS/mach-o/fixup-chains.h: chained-fixup starts/layouts
 *   dyld common/MachOFileAbstraction.hpp: fileset entry ID offset is relative to command
 *   dyld mach_o/UnsafeHeader.cpp: fileset entry ID offset and string bounds validation
 *   osfmk/mach/i386/thread_status.h: x86_THREAD_STATE64 flavor and count
 *   osfmk/mach/i386/_structs.h: x86_thread_state64 register order
 */
#include "macho_parse_x86_64.h"

#define MACH_HEADER_64_SIZE 32U
#define LOAD_COMMAND_SIZE 8U
#define SEGMENT_COMMAND_64_SIZE 72U
#define SECTION_64_SIZE 80U
#define DYLD_CHAINED_FIXUPS_HEADER_SIZE 28U
#define DYLD_CHAINED_STARTS_IN_SEGMENT_HEADER_SIZE 22U

#define MH_MAGIC_64 0xfeedfacfU
#define MH_EXECUTE 2U
#define MH_FILESET 12U
#define CPU_TYPE_X86_64 0x01000007U
#define CPU_SUBTYPE_X86_64_ALL 3U
#define CPU_SUBTYPE_X86_64_H 8U
#define LC_SEGMENT_64 0x19U
#define LC_SYMTAB 0x2U
#define LC_DYLD_INFO 0x22U
#define LC_CODE_SIGNATURE 0x1dU
#define LC_SEGMENT_SPLIT_INFO 0x1eU
#define LC_FUNCTION_STARTS 0x26U
#define LC_DATA_IN_CODE 0x29U
#define LC_DYLIB_CODE_SIGN_DRS 0x2bU
#define LC_LINKER_OPTIMIZATION_HINT 0x2eU
#define LC_REQ_DYLD 0x80000000U
#define LC_DYLD_EXPORTS_TRIE 0x80000033U
#define LC_DYLD_CHAINED_FIXUPS 0x80000034U
#define LC_DYLD_INFO_ONLY 0x80000022U
#define LC_FILESET_ENTRY (0x35U | LC_REQ_DYLD)
#define LC_UNIXTHREAD 0x5U
#define X86_THREAD_STATE64 4U
#define X86_THREAD_STATE64_COUNT 42U
#define X86_THREAD_STATE64_RIP_INDEX 16U
#define FILESET_ENTRY_COMMAND_SIZE 32U
#define LINKEDIT_DATA_COMMAND_SIZE 16U
#define DYLD_INFO_COMMAND_SIZE 48U
#define SYMTAB_COMMAND_SIZE 24U
#define NLIST_64_SIZE 16U

#define DYLD_CHAINED_PTR_START_NONE 0xffffU
#define DYLD_CHAINED_PTR_START_MULTI 0x8000U
#define DYLD_CHAINED_PTR_START_LAST 0x8000U
#define DYLD_CHAINED_PTR_64 2U
#define DYLD_CHAINED_PTR_64_OFFSET 6U
#define DYLD_CHAINED_PTR_64_KERNEL_CACHE 8U
#define DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE 11U

#define VM_PROT_READ 1U
#define VM_PROT_WRITE 2U
#define VM_PROT_EXECUTE 4U
#define SG_NORELOC 0x4U
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000U
#define S_REGULAR 0x0U
#define SECTION_TYPE_MASK 0xffU
#define S_ZEROFILL 0x1U
#define S_GB_ZEROFILL 0xcU
#define S_THREAD_LOCAL_ZEROFILL 0x12U

struct segment_info {
	const unsigned char *name;
	xnu_macho_u64 vmaddr;
	xnu_macho_u64 vmsize;
	xnu_macho_u64 fileoff;
	xnu_macho_u64 filesize;
	xnu_macho_u32 maxprot;
	xnu_macho_u32 initprot;
	xnu_macho_u32 nsects;
	xnu_macho_u32 flags;
};

struct header_region {
	xnu_macho_u32 file_type;
	xnu_macho_u32 cpu_subtype;
	xnu_macho_u32 ncmds;
	xnu_macho_u32 sizeofcmds;
	unsigned long command_off;
	unsigned long command_end;
};

static xnu_macho_u32 read_u32le(const unsigned char *p)
{
	return (xnu_macho_u32)p[0] |
	       ((xnu_macho_u32)p[1] << 8) |
	       ((xnu_macho_u32)p[2] << 16) |
	       ((xnu_macho_u32)p[3] << 24);
}

static xnu_macho_u64 read_u64le(const unsigned char *p)
{
	return (xnu_macho_u64)read_u32le(p) |
	       ((xnu_macho_u64)read_u32le(p + 4) << 32);
}

static xnu_macho_u16 read_u16le(const unsigned char *p)
{
	return (xnu_macho_u16)p[0] | ((xnu_macho_u16)p[1] << 8);
}

static void clear_info(struct xnu_macho_x86_64_info *out)
{
	unsigned char *bytes = (unsigned char *)out;
	unsigned long i;

	for (i = 0; i < sizeof(*out); i++)
		bytes[i] = 0;
}

static int reject(struct xnu_macho_x86_64_info *out, int error)
{
	clear_info(out);
	return error;
}

static int validate_load_command_region(const unsigned char *file,
					unsigned long command_off,
					unsigned long command_end,
					xnu_macho_u32 ncmds)
{
	xnu_macho_u32 i;

	if (command_end < command_off ||
	    (command_end - command_off) % LOAD_COMMAND_SIZE ||
	    ncmds > (command_end - command_off) / LOAD_COMMAND_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;

	for (i = 0; i < ncmds; i++) {
		xnu_macho_u32 command_size;

		if (command_end - command_off < LOAD_COMMAND_SIZE)
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
		command_size = read_u32le(file + command_off + 4);
		if (command_size < LOAD_COMMAND_SIZE ||
		    (command_size & (LOAD_COMMAND_SIZE - 1U)) ||
		    command_size > command_end - command_off)
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
		command_off += command_size;
	}

	if (command_off != command_end)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	return 0;
}

static int parse_header_region(const unsigned char *file,
			       unsigned long file_size,
			       xnu_macho_u64 header_fileoff,
			       struct header_region *out)
{
	const unsigned char *header;
	unsigned long header_off;
	xnu_macho_u32 cputype;
	xnu_macho_u32 sizeofcmds;
	int err;

	if (header_fileoff > (xnu_macho_u64)file_size)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	header_off = (unsigned long)header_fileoff;
	if (file_size - header_off < MACH_HEADER_64_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	header = file + header_off;
	if (read_u32le(header) != MH_MAGIC_64)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	cputype = read_u32le(header + 4);
	if (cputype != CPU_TYPE_X86_64)
		return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	out->cpu_subtype = read_u32le(header + 8);
	if (out->cpu_subtype != CPU_SUBTYPE_X86_64_ALL &&
	    out->cpu_subtype != CPU_SUBTYPE_X86_64_H)
		return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	out->file_type = read_u32le(header + 12);
	if (read_u32le(header + 28) != 0)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	out->ncmds = read_u32le(header + 16);
	sizeofcmds = read_u32le(header + 20);
	if (!out->ncmds || sizeofcmds < LOAD_COMMAND_SIZE ||
	    (unsigned long)sizeofcmds >
		file_size - header_off - MACH_HEADER_64_SIZE ||
	    (sizeofcmds & (LOAD_COMMAND_SIZE - 1U)) ||
	    out->ncmds > sizeofcmds / LOAD_COMMAND_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	out->sizeofcmds = sizeofcmds;
	out->command_off = header_off + MACH_HEADER_64_SIZE;
	out->command_end = out->command_off + (unsigned long)sizeofcmds;
	err = validate_load_command_region(file, out->command_off,
					   out->command_end, out->ncmds);
	if (err)
		return err;
	return 0;
}

static int fileset_entry_id(const unsigned char *command,
			    xnu_macho_u32 command_size,
			    int *is_kernel_entry)
{
	static const unsigned char kernel_id[] = "com.apple.kernel";
	xnu_macho_u32 entry_id;
	const unsigned char *id;
	unsigned long available;
	unsigned long id_size = 0;
	unsigned long string_end;
	unsigned long padding;
	unsigned long i;
	int match = 1;

	if (command_size < FILESET_ENTRY_COMMAND_SIZE ||
	    read_u32le(command + 28) != 0)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	entry_id = read_u32le(command + 24);
	if (entry_id < FILESET_ENTRY_COMMAND_SIZE || entry_id >= command_size)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	for (i = FILESET_ENTRY_COMMAND_SIZE; i < entry_id; i++)
		if (command[i] != 0)
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	id = command + entry_id;
	available = command_size - entry_id;
	while (id_size < available && id[id_size] != '\0')
		id_size++;
	if (!id_size || id_size == available)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	/* lc_str is relative to command start; keep the string inside cmdsize. */
	string_end = (unsigned long)entry_id + id_size + 1UL;
	padding = (LOAD_COMMAND_SIZE -
		   (string_end & (LOAD_COMMAND_SIZE - 1U))) &
		  (LOAD_COMMAND_SIZE - 1U);
	if (padding > command_size - string_end ||
	    string_end + padding != command_size)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	for (i = id_size + 1UL; i < available; i++)
		if (id[i] != 0)
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if (id_size != sizeof(kernel_id) - 1UL)
		match = 0;
	else {
		for (i = 0; i < id_size; i++)
			if (id[i] != kernel_id[i])
				match = 0;
	}
	*is_kernel_entry = match;
	return 0;
}

static int parse_fileset(const unsigned char *file, unsigned long file_size,
			struct xnu_macho_x86_64_info *out)
{
	struct header_region outer;
	struct header_region kernel;
	unsigned long command_off;
	unsigned long kernel_fileoff = 0;
	xnu_macho_u64 kernel_vmaddr = 0;
	xnu_macho_u32 kernel_entry_count = 0;
	xnu_macho_u32 fileset_entry_count = 0;
	xnu_macho_u32 i;
	int err;

	err = parse_header_region(file, file_size, 0, &outer);
	if (err)
		return err;
	if (outer.file_type != MH_FILESET)
		return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	command_off = outer.command_off;
	for (i = 0; i < outer.ncmds; i++) {
		xnu_macho_u32 command = read_u32le(file + command_off);
		xnu_macho_u32 command_size = read_u32le(file + command_off + 4);

		if (command == LC_FILESET_ENTRY) {
			int is_kernel;
			xnu_macho_u64 fileoff;

			if (fileset_entry_id(file + command_off, command_size,
					    &is_kernel))
				return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
			fileset_entry_count++;
			fileoff = read_u64le(file + command_off + 16);
			if (fileoff > (xnu_macho_u64)file_size ||
			    file_size - (unsigned long)fileoff < MACH_HEADER_64_SIZE)
				return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
			if (is_kernel) {
				/* fileoff is the byte offset; vmaddr remains opaque metadata. */
				kernel_entry_count++;
				kernel_fileoff = (unsigned long)fileoff;
				kernel_vmaddr = read_u64le(file + command_off + 8);
			}
		}
		command_off += command_size;
	}
	if (command_off != outer.command_end)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if (kernel_entry_count != 1)
		return kernel_entry_count ? -XNU_MACHO_X86_64_PARSE_ENOEXEC :
			-XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	err = parse_header_region(file, file_size, kernel_fileoff, &kernel);
	if (err)
		return err;
	if (kernel.file_type != MH_EXECUTE)
		return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;

	out->file_type = outer.file_type;
	out->cpu_subtype = outer.cpu_subtype;
	out->load_command_count = outer.ncmds;
	out->load_command_bytes = outer.sizeofcmds;
	out->fileset_entry_count = fileset_entry_count;
	out->kernel_entry_filetype = kernel.file_type;
	out->kernel_entry_cpusubtype = kernel.cpu_subtype;
	out->kernel_entry_ncmds = kernel.ncmds;
	out->kernel_entry_sizeofcmds = kernel.sizeofcmds;
	out->kernel_entry_vmaddr = kernel_vmaddr;
	out->kernel_entry_fileoff = (xnu_macho_u64)kernel_fileoff;
	out->inspection_flags |= XNU_MACHO_X86_64_IS_FILESET |
				 XNU_MACHO_X86_64_HAS_KERNEL_ENTRY;
	return 0;
}

static int fixed_name_is(const unsigned char *name, const char *expected,
			 unsigned long expected_len)
{
	unsigned long i;

	for (i = 0; i < 16; i++) {
		unsigned char want = i < expected_len ? (unsigned char)expected[i] : 0;

		if (name[i] != want)
			return 0;
	}
	return 1;
}

static int is_pagezero(const unsigned char *name)
{
	return fixed_name_is(name, "__PAGEZERO", 10);
}

static int is_zerofill(xnu_macho_u32 flags)
{
	xnu_macho_u32 type = flags & SECTION_TYPE_MASK;

	return type == S_ZEROFILL || type == S_GB_ZEROFILL ||
	       type == S_THREAD_LOCAL_ZEROFILL;
}

static int is_file_only_ctf(const struct segment_info *segment)
{
	return fixed_name_is(segment->name, "__CTF", 5) && !segment->vmsize &&
	       segment->filesize;
}

static int file_range_ok(unsigned long file_size, xnu_macho_u32 offset,
			 xnu_macho_u32 size)
{
	return (unsigned long)offset <= file_size &&
	       (unsigned long)size <= file_size - (unsigned long)offset;
}

static int is_linkedit_data_command(xnu_macho_u32 command)
{
	return command == LC_CODE_SIGNATURE || command == LC_SEGMENT_SPLIT_INFO ||
	       command == LC_FUNCTION_STARTS || command == LC_DATA_IN_CODE ||
	       command == LC_DYLIB_CODE_SIGN_DRS ||
	       command == LC_LINKER_OPTIMIZATION_HINT ||
	       command == LC_DYLD_EXPORTS_TRIE;
}

static int parse_linkedit_data(const unsigned char *file,
			       unsigned long file_size,
			       unsigned long command_off,
			       xnu_macho_u32 command_size)
{
	if (command_size != LINKEDIT_DATA_COMMAND_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if (!file_range_ok(file_size, read_u32le(file + command_off + 8),
			   read_u32le(file + command_off + 12)))
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	return 0;
}

static int parse_dyld_info(const unsigned char *file, unsigned long file_size,
			   unsigned long command_off,
			   xnu_macho_u32 command_size)
{
	xnu_macho_u32 i;

	if (command_size != DYLD_INFO_COMMAND_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	for (i = 0; i < 5; i++) {
		xnu_macho_u32 offset = read_u32le(file + command_off + 8 + i * 8);
		xnu_macho_u32 size = read_u32le(file + command_off + 12 + i * 8);

		if (!file_range_ok(file_size, offset, size))
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	}
	return 0;
}

static int relative_range_ok(unsigned long region_size, xnu_macho_u32 offset,
			     unsigned long size)
{
	return (unsigned long)offset <= region_size &&
	       size <= region_size - (unsigned long)offset;
}

static int x86_chained_pointer_format_supported(xnu_macho_u16 pointer_format)
{
	return pointer_format == DYLD_CHAINED_PTR_64 ||
	       pointer_format == DYLD_CHAINED_PTR_64_OFFSET ||
	       pointer_format == DYLD_CHAINED_PTR_64_KERNEL_CACHE ||
	       pointer_format == DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE;
}

static int chained_start_file_range_ok(
	const struct xnu_macho_x86_64_segment *segment,
	xnu_macho_u32 page_index, xnu_macho_u16 start,
	xnu_macho_u16 page_size)
{
	xnu_macho_u64 segment_offset =
		(xnu_macho_u64)page_index * page_size + start;

	/* Accepted x86_64 chained pointers occupy an eight-byte file-backed word. */
	return start < page_size && page_size - start >= 8 &&
	       segment_offset <= segment->vmsize &&
	       segment->vmsize - segment_offset >= 8 &&
	       segment_offset <= segment->filesize &&
	       segment->filesize - segment_offset >= 8;
}

static int parse_chained_starts_in_segment(
	const unsigned char *starts, unsigned long starts_size,
	xnu_macho_u32 starts_table_size, xnu_macho_u32 starts_offset,
	struct xnu_macho_x86_64_segment *segment,
	xnu_macho_u64 preferred_load_address,
	xnu_macho_u32 *max_valid_pointer_out)
{
	const unsigned char *record;
	xnu_macho_u32 record_size;
	xnu_macho_u16 page_size;
	xnu_macho_u16 pointer_format;
	xnu_macho_u16 page_count;
	xnu_macho_u64 segment_offset;
	unsigned long min_size;
	xnu_macho_u64 pages_in_segment;
	xnu_macho_u32 i;

	if ((starts_offset & 3U) || starts_offset < starts_table_size ||
	    !relative_range_ok(starts_size, starts_offset,
			       DYLD_CHAINED_STARTS_IN_SEGMENT_HEADER_SIZE))
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;

	record = starts + starts_offset;
	record_size = read_u32le(record);
	page_size = read_u16le(record + 4);
	pointer_format = read_u16le(record + 6);
	segment_offset = read_u64le(record + 8);
	page_count = read_u16le(record + 20);
	*max_valid_pointer_out = read_u32le(record + 16);
	min_size = DYLD_CHAINED_STARTS_IN_SEGMENT_HEADER_SIZE +
		   (unsigned long)page_count * 2UL;
	if (!page_count || record_size < min_size || (record_size & 1U) ||
	    !relative_range_ok(starts_size, starts_offset, record_size))
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if (page_size != 0x1000U && page_size != 0x4000U)
		return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	if (!x86_chained_pointer_format_supported(pointer_format))
		return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	if (!segment->vmsize ||
	    segment_offset != segment->vmaddr - preferred_load_address)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;

	pages_in_segment = segment->vmsize / page_size;
	if (segment->vmsize % page_size)
		pages_in_segment++;
	if ((xnu_macho_u64)page_count > pages_in_segment)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;

	for (i = 0; i < page_count; i++) {
		xnu_macho_u16 start =
			read_u16le(record + DYLD_CHAINED_STARTS_IN_SEGMENT_HEADER_SIZE +
				   (unsigned long)i * 2UL);

		if (start == DYLD_CHAINED_PTR_START_NONE)
			continue;
		if (start & DYLD_CHAINED_PTR_START_MULTI)
			return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
		if (!chained_start_file_range_ok(segment, i, start, page_size))
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	}

	segment->chained_pointer_format = pointer_format;
	segment->chained_page_size = page_size;
	segment->chained_page_count = page_count;
	return 0;
}

static int parse_chained_fixups(const unsigned char *file,
				unsigned long file_size,
				xnu_macho_u32 data_offset,
				xnu_macho_u32 data_size,
				xnu_macho_u64 preferred_load_address,
				struct xnu_macho_x86_64_info *out)
{
	const unsigned char *payload;
	const unsigned char *starts;
	xnu_macho_u32 version;
	xnu_macho_u32 starts_offset;
	xnu_macho_u32 imports_offset;
	xnu_macho_u32 symbols_offset;
	xnu_macho_u32 imports_count;
	xnu_macho_u32 imports_format;
	xnu_macho_u32 symbols_format;
	xnu_macho_u32 starts_segment_count;
	xnu_macho_u32 starts_table_size;
	unsigned long starts_size;
	unsigned long imports_entry_size = 0;
	unsigned long imports_table_size = 0;
	unsigned long i;
	unsigned long range_start[XNU_MACHO_X86_64_MAX_SEGMENTS];
	unsigned long range_end[XNU_MACHO_X86_64_MAX_SEGMENTS];
	unsigned long range_count = 0;
	xnu_macho_u16 image_pointer_format = 0;
	xnu_macho_u32 image_max_valid_pointer = 0;
	int have_image_pointer_format = 0;
	int have_image_max_valid_pointer = 0;
	int has_zero_vm_segment_omission = 0;

	if (!file_range_ok(file_size, data_offset, data_size) ||
	    data_size < DYLD_CHAINED_FIXUPS_HEADER_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	payload = file + data_offset;
	version = read_u32le(payload);
	starts_offset = read_u32le(payload + 4);
	imports_offset = read_u32le(payload + 8);
	symbols_offset = read_u32le(payload + 12);
	imports_count = read_u32le(payload + 16);
	imports_format = read_u32le(payload + 20);
	symbols_format = read_u32le(payload + 24);
	if (version != 0)
		return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	if (starts_offset < DYLD_CHAINED_FIXUPS_HEADER_SIZE ||
	    (starts_offset & 3U) ||
	    !relative_range_ok(data_size, starts_offset, sizeof(xnu_macho_u32)) ||
	    imports_offset > data_size || symbols_offset > data_size ||
	    starts_offset > imports_offset || imports_offset > symbols_offset)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if (symbols_format != 0U)
		return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	switch (imports_format) {
	case 1: imports_entry_size = 4; break;
	case 2: imports_entry_size = 8; break;
	case 3: imports_entry_size = 16; break;
	default:
		return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	}
	imports_table_size = (unsigned long)imports_count * imports_entry_size;
	if ((imports_offset & 3U) ||
	    !relative_range_ok(symbols_offset, imports_offset, imports_table_size))
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	/* Imports carry name offsets into a nonempty symbols pool. */
	if (imports_count && symbols_offset >= data_size)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;

	starts_size = imports_offset - starts_offset;
	if (starts_size < sizeof(xnu_macho_u32))
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	starts = payload + starts_offset;
	starts_segment_count = read_u32le(starts);
	if (starts_segment_count > out->segment_count ||
	    starts_segment_count > XNU_MACHO_X86_64_MAX_SEGMENTS)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if (starts_segment_count != out->segment_count) {
		struct xnu_macho_x86_64_segment *linkedit;
		struct xnu_macho_x86_64_segment *omitted;

		if (starts_segment_count + 1U != out->segment_count ||
		    !starts_segment_count)
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
		linkedit = &out->segments[out->segment_count - 1U];
		omitted = &out->segments[starts_segment_count - 1U];
		/* Match Tahoe dyld's narrow CTF exception: the sole omitted slot is
		 * the zero-VM segment immediately before a final __LINKEDIT.
		 */
		if (!fixed_name_is(linkedit->name, "__LINKEDIT", 10) || omitted->vmsize)
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
		has_zero_vm_segment_omission = 1;
	}
	starts_table_size = 4U + starts_segment_count * 4U;
	if (starts_size < starts_table_size)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	out->chained_starts_segment_count = starts_segment_count;
	for (i = 0; i < starts_segment_count; i++) {
		xnu_macho_u32 segment_info_offset = read_u32le(starts + 4 + i * 4UL);
		struct xnu_macho_x86_64_segment *segment = &out->segments[i];
		unsigned long end;
		unsigned long j;
		xnu_macho_u32 max_valid_pointer;
		int err;

		if (has_zero_vm_segment_omission &&
		    i == starts_segment_count - 1U && segment_info_offset)
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
		if (!segment_info_offset)
			continue;
		err = parse_chained_starts_in_segment(starts, starts_size,
						     starts_table_size,
						     segment_info_offset,
						     segment,
						     preferred_load_address,
						     &max_valid_pointer);
		if (err)
			return err;
		if (max_valid_pointer) {
			if (!have_image_max_valid_pointer) {
				image_max_valid_pointer = max_valid_pointer;
				have_image_max_valid_pointer = 1;
			} else if (max_valid_pointer != image_max_valid_pointer) {
				return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
			}
		}
		if (!have_image_pointer_format) {
			image_pointer_format =
				(xnu_macho_u16)segment->chained_pointer_format;
			have_image_pointer_format = 1;
		} else if (segment->chained_pointer_format != image_pointer_format) {
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
		}
		end = (unsigned long)segment_info_offset +
		      read_u32le(starts + segment_info_offset);
		for (j = 0; j < range_count; j++) {
			if ((unsigned long)segment_info_offset < range_end[j] &&
			    range_start[j] < end)
				return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
		}
		range_start[range_count] = segment_info_offset;
		range_end[range_count] = end;
		range_count++;
		out->chained_starts_nonempty_segment_count++;
	}
	return 0;
}

static int parse_symtab(const unsigned char *file, unsigned long file_size,
			unsigned long command_off,
			xnu_macho_u32 command_size)
{
	xnu_macho_u32 symoff;
	xnu_macho_u32 nsyms;
	xnu_macho_u32 stroff;
	xnu_macho_u32 strsize;

	if (command_size != SYMTAB_COMMAND_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	symoff = read_u32le(file + command_off + 8);
	nsyms = read_u32le(file + command_off + 12);
	stroff = read_u32le(file + command_off + 16);
	strsize = read_u32le(file + command_off + 20);
	if ((unsigned long)symoff > file_size ||
	    (unsigned long)nsyms >
	    (file_size - (unsigned long)symoff) / NLIST_64_SIZE ||
	    !file_range_ok(file_size, stroff, strsize))
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	return 0;
}

static int parse_segment(const unsigned char *file, unsigned long file_size,
			 unsigned long command_off, xnu_macho_u32 command_size,
			 struct segment_info *segment)
{
	xnu_macho_u64 vm_end;
	xnu_macho_u32 maxprot;
	xnu_macho_u32 i;
	unsigned long sections_size;
	int file_only_ctf;

	if (command_size < SEGMENT_COMMAND_64_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;

	segment->name = file + command_off + 8;
	segment->vmaddr = read_u64le(file + command_off + 24);
	segment->vmsize = read_u64le(file + command_off + 32);
	segment->fileoff = read_u64le(file + command_off + 40);
	segment->filesize = read_u64le(file + command_off + 48);
	maxprot = read_u32le(file + command_off + 56);
	segment->maxprot = maxprot;
	segment->initprot = read_u32le(file + command_off + 60);
	segment->nsects = read_u32le(file + command_off + 64);
	segment->flags = read_u32le(file + command_off + 68);
	file_only_ctf = is_file_only_ctf(segment);

	if (segment->nsects >
	    (command_size - SEGMENT_COMMAND_64_SIZE) / SECTION_64_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	sections_size = (unsigned long)segment->nsects * SECTION_64_SIZE;
	if (sections_size != command_size - SEGMENT_COMMAND_64_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if ((segment->filesize > segment->vmsize &&
	     !(file_only_ctf && segment->flags == SG_NORELOC)) ||
	    segment->fileoff > file_size ||
	    segment->filesize > file_size - (unsigned long)segment->fileoff)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if (file_only_ctf &&
	    (segment->flags != SG_NORELOC || segment->nsects != 1 ||
	     segment->maxprot != VM_PROT_READ ||
	     segment->initprot != VM_PROT_READ))
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if (segment->vmaddr > ~0ULL - segment->vmsize)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	vm_end = segment->vmaddr + segment->vmsize;
	if ((maxprot | segment->initprot) &
	    ~(VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE))
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if (segment->initprot & ~maxprot)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;

	for (i = 0; i < segment->nsects; i++) {
		unsigned long section_off = command_off + SEGMENT_COMMAND_64_SIZE +
					    (unsigned long)i * SECTION_64_SIZE;
		const unsigned char *section = file + section_off;
		xnu_macho_u64 addr = read_u64le(section + 32);
		xnu_macho_u64 size = read_u64le(section + 40);
		xnu_macho_u32 offset = read_u32le(section + 48);
		xnu_macho_u32 align = read_u32le(section + 52);
		xnu_macho_u32 reloff = read_u32le(section + 56);
		xnu_macho_u32 nreloc = read_u32le(section + 60);
		xnu_macho_u32 flags = read_u32le(section + 64);
		xnu_macho_u64 section_delta;

		if (!fixed_name_is(section + 16, (const char *)segment->name, 16))
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
		if (addr < segment->vmaddr)
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
		section_delta = addr - segment->vmaddr;
		if (file_only_ctf) {
			if (!fixed_name_is(section, "__ctf", 5) ||
			    addr != segment->vmaddr ||
			    size > ~0ULL - addr ||
			    !size || size > segment->filesize ||
			    offset != segment->fileoff || flags != S_REGULAR ||
			    reloff || nreloc)
				return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
		} else if (section_delta > segment->vmsize ||
			   size > segment->vmsize - section_delta || addr > vm_end) {
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
		}
		if (align >= 64)
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;

		if (!is_zerofill(flags)) {
			xnu_macho_u64 file_delta;

			if ((xnu_macho_u64)offset < segment->fileoff)
				return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
			file_delta = (xnu_macho_u64)offset - segment->fileoff;
			if (file_delta > segment->filesize ||
			    size > segment->filesize - file_delta ||
			    (unsigned long)offset > file_size ||
			    size > file_size - (unsigned long)offset)
				return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
			if (file_delta != section_delta)
				return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
		}
		if (nreloc && ((unsigned long)reloff > file_size ||
			       (unsigned long)nreloc >
			       (file_size - (unsigned long)reloff) / 8))
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	}

	if (is_pagezero(segment->name) &&
	    (segment->filesize || segment->nsects || segment->initprot))
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	return 0;
}

static void export_segment_plan(struct xnu_macho_x86_64_segment *out,
				const struct segment_info *in)
{
	unsigned long i;

	for (i = 0; i < 16; i++)
		out->name[i] = in->name[i];
	out->vmaddr = in->vmaddr;
	out->vmsize = in->vmsize;
	out->fileoff = in->fileoff;
	out->filesize = in->filesize;
	out->maxprot = in->maxprot;
	out->initprot = in->initprot;
	out->flags = in->flags;
	out->section_count = in->nsects;
}

/* The caller has already validated every section's VM and file ranges. */
static xnu_macho_u32 match_hib_text(const unsigned char *section,
				  const struct segment_info *segment,
				  xnu_macho_u64 entry, xnu_macho_u64 *fileoff)
{
	xnu_macho_u32 i, matches = 0;

	if (!fixed_name_is(segment->name, "__HIB", 5) ||
	    segment->initprot != (VM_PROT_READ | VM_PROT_WRITE) ||
	    segment->maxprot != segment->initprot)
		return 0;

	for (i = 0; i < segment->nsects; i++, section += SECTION_64_SIZE) {
		xnu_macho_u64 addr = read_u64le(section + 32);
		xnu_macho_u64 size = read_u64le(section + 40);
		xnu_macho_u32 flags = read_u32le(section + 64);

		if (entry < addr || entry - addr >= size)
			continue;
		if (++matches != 1 || !fixed_name_is(section, "__text", 6) ||
		    (flags & SECTION_TYPE_MASK) != S_REGULAR ||
		    !(flags & S_ATTR_PURE_INSTRUCTIONS))
			return 0;
		*fileoff = read_u32le(section + 48) + (entry - addr);
	}
	return matches;
}

static int parse_image(const void *input, unsigned long file_size,
		       struct xnu_macho_x86_64_info *out,
		       xnu_macho_u64 *hib_entry_fileoff)
{
	const unsigned char *file = input;
	unsigned long command_off;
	unsigned long command_end;
	xnu_macho_u32 ncmds;
	xnu_macho_u32 sizeofcmds;
	xnu_macho_u32 i;
	xnu_macho_u32 thread_count = 0;
	xnu_macho_u32 dyld_info_count = 0;
	xnu_macho_u32 chained_fixups_count = 0;
	xnu_macho_u32 chained_fixups_data_offset = 0;
	xnu_macho_u32 chained_fixups_data_size = 0;
	xnu_macho_u32 symtab_count = 0;
	xnu_macho_u32 entry_matches = 0;
	xnu_macho_u32 hib_matches = 0;
	xnu_macho_u32 vm_matches = 0;
	xnu_macho_u64 hib_fileoff = 0;
	xnu_macho_u64 entry = 0;
	xnu_macho_u64 preferred_load_address = 0;
	xnu_macho_u32 text_segment_count = 0;
	int have_loaded_segment = 0;

	if (!out)
		return -XNU_MACHO_X86_64_PARSE_EINVAL;
	clear_info(out);
	if (!file || file_size < MACH_HEADER_64_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if (read_u32le(file) != MH_MAGIC_64)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if (read_u32le(file + 4) != CPU_TYPE_X86_64)
		return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	if (read_u32le(file + 8) != CPU_SUBTYPE_X86_64_ALL &&
	    read_u32le(file + 8) != CPU_SUBTYPE_X86_64_H)
		return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	if (read_u32le(file + 12) == MH_FILESET && !hib_entry_fileoff) {
		int err = parse_fileset(file, file_size, out);

		return err ? reject(out, err) : 0;
	}
	if (read_u32le(file + 12) != MH_EXECUTE)
		return -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	if (read_u32le(file + 28) != 0)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;

	ncmds = read_u32le(file + 16);
	sizeofcmds = read_u32le(file + 20);
	if (sizeofcmds > file_size - MACH_HEADER_64_SIZE ||
	    (sizeofcmds & 7U) || ncmds > sizeofcmds / LOAD_COMMAND_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	command_off = MACH_HEADER_64_SIZE;
	command_end = command_off + sizeofcmds;

	for (i = 0; i < ncmds; i++) {
		xnu_macho_u32 command;
		xnu_macho_u32 command_size;

		if (command_end - command_off < LOAD_COMMAND_SIZE)
			return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
		command = read_u32le(file + command_off);
		command_size = read_u32le(file + command_off + 4);
		if (command_size < LOAD_COMMAND_SIZE || (command_size & 7U) ||
		    command_size > command_end - command_off)
			return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);

		if (command == LC_SEGMENT_64) {
			struct segment_info segment;
			struct xnu_macho_x86_64_segment *plan_segment;
			xnu_macho_u64 vm_end;
			int err = parse_segment(file, file_size, command_off,
						command_size, &segment);

			if (err)
				return reject(out, err);
			if (out->segment_count >= XNU_MACHO_X86_64_MAX_SEGMENTS)
				return reject(out, -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP);
			if (fixed_name_is(segment.name, "__TEXT", 6)) {
				if (++text_segment_count == 1)
					preferred_load_address = segment.vmaddr;
			}
			plan_segment = &out->segments[out->segment_count];
			export_segment_plan(plan_segment, &segment);
			out->segment_count++;
			if (segment.vmsize && !is_pagezero(segment.name)) {
				vm_end = segment.vmaddr + segment.vmsize;
				if (!have_loaded_segment || segment.vmaddr < out->vm_low)
					out->vm_low = segment.vmaddr;
				if (!have_loaded_segment || vm_end > out->vm_high)
					out->vm_high = vm_end;
				have_loaded_segment = 1;
				out->mapped_segment_count++;
			}
		} else if (command == LC_UNIXTHREAD) {
			xnu_macho_u32 flavor;
			xnu_macho_u32 count;
			unsigned long expected_size = LOAD_COMMAND_SIZE + 8U +
						      X86_THREAD_STATE64_COUNT * 4U;

			if (++thread_count != 1)
				return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
			if (command_size < LOAD_COMMAND_SIZE + 8U)
				return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
			flavor = read_u32le(file + command_off + LOAD_COMMAND_SIZE);
			count = read_u32le(file + command_off + LOAD_COMMAND_SIZE + 4);
			if (flavor != X86_THREAD_STATE64)
				return reject(out, -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP);
			if (count != X86_THREAD_STATE64_COUNT || command_size != expected_size)
				return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
			entry = read_u64le(file + command_off + LOAD_COMMAND_SIZE + 8U +
					  X86_THREAD_STATE64_RIP_INDEX * 8U);
		} else if (command == LC_DYLD_CHAINED_FIXUPS) {
			if (++chained_fixups_count != 1)
				return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
			if (parse_linkedit_data(file, file_size, command_off,
						command_size))
				return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
			chained_fixups_data_offset = read_u32le(file + command_off + 8);
			chained_fixups_data_size = read_u32le(file + command_off + 12);
			out->inspection_flags |= XNU_MACHO_X86_64_HAS_CHAINED_FIXUPS;
			out->linkedit_data_command_count++;
		} else if (command == LC_DYLD_INFO || command == LC_DYLD_INFO_ONLY) {
			if (++dyld_info_count != 1)
				return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
			if (parse_dyld_info(file, file_size, command_off, command_size))
				return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
			out->inspection_flags |= XNU_MACHO_X86_64_HAS_DYLD_INFO;
		} else if (command == LC_SYMTAB) {
			if (++symtab_count != 1)
				return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
			if (parse_symtab(file, file_size, command_off, command_size))
				return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
			out->inspection_flags |= XNU_MACHO_X86_64_HAS_SYMTAB;
		} else if (is_linkedit_data_command(command)) {
			if (parse_linkedit_data(file, file_size, command_off,
						command_size))
				return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
			out->inspection_flags |= XNU_MACHO_X86_64_HAS_UNINTERPRETED_LINKEDIT;
			out->linkedit_data_command_count++;
		} else if (command & LC_REQ_DYLD) {
			return reject(out, -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP);
		} else {
			out->inspection_flags |= XNU_MACHO_X86_64_HAS_UNCLASSIFIED_COMMANDS;
			out->unclassified_load_command_count++;
		}
		command_off += command_size;
	}

	if (command_off != command_end || thread_count != 1 ||
	    !have_loaded_segment || out->vm_high <= out->vm_low)
		return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
	if (chained_fixups_count) {
		int err;

		if (text_segment_count != 1)
			return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
		err = parse_chained_fixups(file, file_size,
					       chained_fixups_data_offset,
					       chained_fixups_data_size,
					       preferred_load_address, out);

		if (err)
			return reject(out, err);
	}

	/* Entry validation is deliberately deferred until all commands are parsed. */
	command_off = MACH_HEADER_64_SIZE;
	for (i = 0; i < ncmds; i++) {
		xnu_macho_u32 command = read_u32le(file + command_off);
		xnu_macho_u32 command_size = read_u32le(file + command_off + 4);

		if (command == LC_SEGMENT_64) {
			struct segment_info segment;
			int err = parse_segment(file, file_size, command_off,
						command_size, &segment);

			if (err)
				return reject(out, err);
			if (segment.vmsize && !is_pagezero(segment.name) &&
			    entry >= segment.vmaddr) {
				if ((segment.initprot & VM_PROT_EXECUTE) &&
				    entry - segment.vmaddr < segment.filesize)
					entry_matches++;
				if (hib_entry_fileoff && entry - segment.vmaddr < segment.vmsize) {
					vm_matches++;
					hib_matches += match_hib_text(file + command_off +
								      SEGMENT_COMMAND_64_SIZE,
								      &segment, entry, &hib_fileoff);
				}
			}
		}
		command_off += command_size;
	}
	if (hib_entry_fileoff) {
		if (vm_matches != 1 || hib_matches != 1 || entry_matches)
			return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
		*hib_entry_fileoff = hib_fileoff;
	} else if (entry_matches != 1) {
		return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);
	}

	out->file_type = MH_EXECUTE;
	out->cpu_subtype = read_u32le(file + 8);
	out->load_command_count = ncmds;
	out->load_command_bytes = sizeofcmds;
	out->entry = entry;
	return 0;
}

int xnu_macho_x86_64_parse(const void *input, unsigned long file_size,
			   struct xnu_macho_x86_64_info *out)
{
	return parse_image(input, file_size, out, 0);
}

#ifndef __KERNEL__
int xnu_macho_x86_64_inspect_hib_entry(const void *input, unsigned long file_size,
				     struct xnu_macho_x86_64_info *out,
				     xnu_macho_u64 *entry_fileoff)
{
	if (!entry_fileoff) {
		if (out)
			clear_info(out);
		return -XNU_MACHO_X86_64_PARSE_EINVAL;
	}
	*entry_fileoff = 0;
	return parse_image(input, file_size, out, entry_fileoff);
}
#endif
