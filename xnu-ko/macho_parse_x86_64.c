// SPDX-License-Identifier: APSL-2.0
/*
 * Strict, parse-only x86_64 Mach-O subset. Keep this object out of xnu.ko:
 * parsing does not establish a safe Linux/XNU execution boundary.
 *
 * ABI references (pinned public XNU headers):
 *   osfmk/mach/machine.h: CPU_TYPE_X86_64, CPU_SUBTYPE_X86_64_{ALL,H}
 *   EXTERNAL_HEADERS/mach-o/loader.h: Mach-O commands and segment layouts
 *   osfmk/mach/i386/thread_status.h: x86_THREAD_STATE64 flavor and count
 *   osfmk/mach/i386/_structs.h: x86_thread_state64 register order
 */
#include "macho_parse_x86_64.h"

#define MACH_HEADER_64_SIZE 32U
#define LOAD_COMMAND_SIZE 8U
#define SEGMENT_COMMAND_64_SIZE 72U
#define SECTION_64_SIZE 80U

#define MH_MAGIC_64 0xfeedfacfU
#define MH_EXECUTE 2U
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
#define LC_DYLD_EXPORTS_TRIE 0x80000033U
#define LC_DYLD_CHAINED_FIXUPS 0x80000034U
#define LC_DYLD_INFO_ONLY 0x80000022U
#define LC_UNIXTHREAD 0x5U
#define X86_THREAD_STATE64 4U
#define X86_THREAD_STATE64_COUNT 42U
#define X86_THREAD_STATE64_RIP_INDEX 16U
#define LC_REQ_DYLD 0x80000000U
#define LINKEDIT_DATA_COMMAND_SIZE 16U
#define DYLD_INFO_COMMAND_SIZE 48U
#define SYMTAB_COMMAND_SIZE 24U
#define NLIST_64_SIZE 16U

#define VM_PROT_READ 1U
#define VM_PROT_WRITE 2U
#define VM_PROT_EXECUTE 4U
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
	xnu_macho_u32 initprot;
	xnu_macho_u32 nsects;
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

static void clear_info(struct xnu_macho_x86_64_info *out)
{
	out->vm_low = 0;
	out->vm_high = 0;
	out->entry = 0;
	out->segment_count = 0;
	out->mapped_segment_count = 0;
	out->inspection_flags = 0;
	out->unclassified_load_command_count = 0;
	out->linkedit_data_command_count = 0;
	out->reserved = 0;
}

static int reject(struct xnu_macho_x86_64_info *out, int error)
{
	clear_info(out);
	return error;
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

	if (command_size < SEGMENT_COMMAND_64_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;

	segment->name = file + command_off + 8;
	segment->vmaddr = read_u64le(file + command_off + 24);
	segment->vmsize = read_u64le(file + command_off + 32);
	segment->fileoff = read_u64le(file + command_off + 40);
	segment->filesize = read_u64le(file + command_off + 48);
	maxprot = read_u32le(file + command_off + 56);
	segment->initprot = read_u32le(file + command_off + 60);
	segment->nsects = read_u32le(file + command_off + 64);

	if (segment->nsects >
	    (command_size - SEGMENT_COMMAND_64_SIZE) / SECTION_64_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	sections_size = (unsigned long)segment->nsects * SECTION_64_SIZE;
	if (sections_size != command_size - SEGMENT_COMMAND_64_SIZE)
		return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
	if (segment->filesize > segment->vmsize ||
	    segment->fileoff > file_size ||
	    segment->filesize > file_size - (unsigned long)segment->fileoff)
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
		if (section_delta > segment->vmsize ||
		    size > segment->vmsize - section_delta || addr > vm_end)
			return -XNU_MACHO_X86_64_PARSE_ENOEXEC;
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

int xnu_macho_x86_64_parse(const void *input, unsigned long file_size,
			   struct xnu_macho_x86_64_info *out)
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
	xnu_macho_u32 symtab_count = 0;
	xnu_macho_u32 entry_matches = 0;
	xnu_macho_u64 entry = 0;
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
			xnu_macho_u64 vm_end;
			int err = parse_segment(file, file_size, command_off,
						command_size, &segment);

			if (err)
				return reject(out, err);
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

	/* Entry validation is deliberately deferred until all commands are parsed. */
	entry_matches = 0;
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
			    (segment.initprot & VM_PROT_EXECUTE) &&
			    entry >= segment.vmaddr &&
			    entry - segment.vmaddr < segment.filesize)
				entry_matches++;
		}
		command_off += command_size;
	}
	if (entry_matches != 1)
		return reject(out, -XNU_MACHO_X86_64_PARSE_ENOEXEC);

	out->entry = entry;
	return 0;
}
