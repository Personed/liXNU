// SPDX-License-Identifier: APSL-2.0
/* Synthetic format fixtures only; no Apple kernel image is bundled. */
#include <stdio.h>
#include <string.h>

#include "../../xnu-ko/macho_parse_x86_64.h"

_Static_assert(sizeof(struct xnu_macho_x86_64_info) <= 2048,
	       "parser result must stay bounded for kernel stack callers");

#define FILE_BYTES 4096U
#define HEADER_SIZE 32U
#define PAGEZERO_OFF HEADER_SIZE
#define PAGEZERO_SIZE 72U
#define TEXT_OFF (PAGEZERO_OFF + PAGEZERO_SIZE)
#define TEXT_SIZE (72U + 80U)
#define SECTION_OFF (TEXT_OFF + 72U)
#define THREAD_OFF (TEXT_OFF + TEXT_SIZE)
#define THREAD_SIZE (8U + 8U + 42U * 4U)
#define COMMAND_BYTES (PAGEZERO_SIZE + TEXT_SIZE + THREAD_SIZE)
#define COMMANDS_END (HEADER_SIZE + COMMAND_BYTES)

#define MH_MAGIC_64 0xfeedfacfU
#define MH_EXECUTE 2U
#define CPU_TYPE_X86_64 0x01000007U
#define CPU_SUBTYPE_X86_64_ALL 3U
#define CPU_SUBTYPE_X86_64_H 8U
#define LC_SEGMENT_64 0x19U
#define LC_UNIXTHREAD 0x5U
#define LC_DYLD_INFO_ONLY 0x80000022U
#define LC_DYLD_CHAINED_FIXUPS 0x80000034U
#define X86_THREAD_STATE64 4U
#define X86_THREAD_STATE64_COUNT 42U
#define RIP_OFFSET (THREAD_OFF + 16U + 16U * 8U)

static void put_u32(unsigned char *p, unsigned int value)
{
	p[0] = value & 0xffU;
	p[1] = (value >> 8) & 0xffU;
	p[2] = (value >> 16) & 0xffU;
	p[3] = (value >> 24) & 0xffU;
}

static void put_u16(unsigned char *p, unsigned short value)
{
	p[0] = value & 0xffU;
	p[1] = (value >> 8) & 0xffU;
}

static void put_u64(unsigned char *p, unsigned long long value)
{
	put_u32(p, (unsigned int)value);
	put_u32(p + 4, (unsigned int)(value >> 32));
}

static void put_name(unsigned char *p, const char *name)
{
	unsigned int i;
	int ended = 0;

	for (i = 0; i < 16; i++) {
		if (!ended && !name[i])
			ended = 1;
		p[i] = ended ? 0 : (unsigned char)name[i];
	}
}

static void make_valid_image(unsigned char *file, unsigned int subtype)
{
	memset(file, 0, FILE_BYTES);
	put_u32(file, MH_MAGIC_64);
	put_u32(file + 4, CPU_TYPE_X86_64);
	put_u32(file + 8, subtype);
	put_u32(file + 12, MH_EXECUTE);
	put_u32(file + 16, 3);
	put_u32(file + 20, COMMAND_BYTES);

	put_u32(file + PAGEZERO_OFF, LC_SEGMENT_64);
	put_u32(file + PAGEZERO_OFF + 4, PAGEZERO_SIZE);
	put_name(file + PAGEZERO_OFF + 8, "__PAGEZERO");
	put_u64(file + PAGEZERO_OFF + 24, 0);
	put_u64(file + PAGEZERO_OFF + 32, 0x1000);

	put_u32(file + TEXT_OFF, LC_SEGMENT_64);
	put_u32(file + TEXT_OFF + 4, TEXT_SIZE);
	put_name(file + TEXT_OFF + 8, "__TEXT");
	put_u64(file + TEXT_OFF + 24, 0x1000);
	put_u64(file + TEXT_OFF + 32, 0x2000);
	put_u64(file + TEXT_OFF + 40, 0);
	put_u64(file + TEXT_OFF + 48, 0x200);
	put_u32(file + TEXT_OFF + 56, 5);
	put_u32(file + TEXT_OFF + 60, 5);
	put_u32(file + TEXT_OFF + 64, 1);

	put_name(file + SECTION_OFF, "__text");
	put_name(file + SECTION_OFF + 16, "__TEXT");
	put_u64(file + SECTION_OFF + 32, 0x1100);
	put_u64(file + SECTION_OFF + 40, 0x20);
	put_u32(file + SECTION_OFF + 48, 0x100);
	put_u32(file + SECTION_OFF + 52, 4);

	put_u32(file + THREAD_OFF, LC_UNIXTHREAD);
	put_u32(file + THREAD_OFF + 4, THREAD_SIZE);
	put_u32(file + THREAD_OFF + 8, X86_THREAD_STATE64);
	put_u32(file + THREAD_OFF + 12, X86_THREAD_STATE64_COUNT);
	put_u64(file + RIP_OFFSET, 0x1100);
}

static int info_is_zero(const struct xnu_macho_x86_64_info *info)
{
	const unsigned char *bytes = (const unsigned char *)info;
	unsigned int i;

	for (i = 0; i < sizeof(*info); i++)
		if (bytes[i])
			return 0;
	return 1;
}

static int expect_ok(const char *name, unsigned int subtype)
{
	unsigned char file[FILE_BYTES];
	unsigned char original[FILE_BYTES];
	struct xnu_macho_x86_64_info info;
	int result;

	make_valid_image(file, subtype);
	memcpy(original, file, sizeof(file));
	memset(&info, 0xa5, sizeof(info));
	result = xnu_macho_x86_64_parse(file, FILE_BYTES, &info);
	if (result || info.vm_low != 0x1000 || info.vm_high != 0x3000 ||
	    info.entry != 0x1100 || info.segment_count != 2 ||
	    info.mapped_segment_count != 1 || info.inspection_flags ||
	    info.unclassified_load_command_count || info.linkedit_data_command_count ||
	    info.segments[0].vmaddr != 0 || info.segments[0].vmsize != 0x1000 ||
	    info.segments[0].fileoff || info.segments[0].filesize ||
	    memcmp(info.segments[0].name, "__PAGEZERO", 10) ||
	    info.segments[1].vmaddr != 0x1000 || info.segments[1].vmsize != 0x2000 ||
	    info.segments[1].fileoff || info.segments[1].filesize != 0x200 ||
	    info.segments[1].maxprot != 5 || info.segments[1].initprot != 5 ||
	    info.segments[1].section_count != 1 || info.segments[1].flags ||
	    memcmp(file, original, sizeof(file))) {
		fprintf(stderr, "FAIL: %s (result=%d, low=%llx, high=%llx, entry=%llx)\n",
			name, result, info.vm_low, info.vm_high, info.entry);
		return 1;
	}
	return 0;
}

static void append_command(unsigned char *file, unsigned int command,
			   unsigned int command_size)
{
	put_u32(file + COMMANDS_END, command);
	put_u32(file + COMMANDS_END + 4, command_size);
	put_u32(file + 16, 4);
	put_u32(file + 20, COMMAND_BYTES + command_size);
}

static void append_chained_fixups_valid(unsigned char *file)
{
	unsigned char *payload = file + 512;
	unsigned char *starts = payload + 28;
	unsigned char *segment = starts + 12;

	append_command(file, LC_DYLD_CHAINED_FIXUPS, 16);
	put_u32(file + COMMANDS_END + 8, 512);
	put_u32(file + COMMANDS_END + 12, 68);
	put_u32(payload, 0); /* fixups_version */
	put_u32(payload + 4, 28); /* starts_offset */
	put_u32(payload + 8, 68); /* empty imports at end of starts region */
	put_u32(payload + 12, 68); /* empty symbols at end of imports */
	put_u32(payload + 16, 0); /* imports_count */
	put_u32(payload + 20, 1); /* imports_format */
	put_u32(payload + 24, 0); /* uncompressed symbol strings */
	put_u32(starts, 2); /* segment_count includes __PAGEZERO */
	put_u32(starts + 4, 0); /* no starts for __PAGEZERO */
	put_u32(starts + 8, 12); /* segment record offset relative to starts */
	put_u32(segment, 26); /* 22-byte header + two page_start entries */
	put_u32(segment + 4, 0x000b1000U); /* format 11, page size 4K */
	put_u64(segment + 8, 0); /* __TEXT preferred load address */
	put_u32(segment + 16, 0); /* max_valid_pointer */
	put_u32(segment + 20, 2); /* page_count and first page_start */
	put_u16(segment + 22, 0x10); /* structurally valid page offset */
	put_u16(segment + 24, 0xffff); /* second page has no fixups */
}

static void append_chained_fixups_with_import(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + COMMANDS_END + 12, 84);
	put_u32(file + 512 + 8, 68);
	put_u32(file + 512 + 12, 72);
	put_u32(file + 512 + 16, 1);
	put_u32(file + 512 + 20, 1);
}

static void append_chained_fixups_import_without_symbols(unsigned char *file)
{
	append_chained_fixups_with_import(file);
	put_u32(file + 512 + 12, 0);
}

static void append_chained_fixups_import_with_empty_symbols(unsigned char *file)
{
	append_chained_fixups_with_import(file);
	put_u32(file + 512 + 12, 84);
}

static void append_chained_fixups_import_symbols_offset_in_header(unsigned char *file)
{
	append_chained_fixups_with_import(file);
	put_u32(file + 512 + 12, 24);
}

static void append_chained_fixups_bad_imports_format(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 20, 0);
}

static void append_chained_fixups_bad_symbols_format(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 24, 1);
}

static void append_chained_fixups_multi_starts(unsigned char *file)
{
	unsigned char *payload = file + 512;
	unsigned char *starts = payload + 28;
	unsigned char *segment = starts + 12;

	append_command(file, LC_DYLD_CHAINED_FIXUPS, 16);
	put_u32(file + COMMANDS_END + 8, 512);
	put_u32(file + COMMANDS_END + 12, 72);
	put_u32(payload + 4, 28);
	put_u32(payload + 8, 72);
	put_u32(payload + 12, 72);
	put_u32(payload + 20, 1);
	put_u32(starts, 2);
	put_u32(starts + 4, 0);
	put_u32(starts + 8, 12);
	put_u32(segment, 30); /* header + two page starts + two overflow entries */
	put_u32(segment + 4, 0x000b1000U);
	put_u64(segment + 8, 0);
	put_u32(segment + 16, 0);
	put_u32(segment + 20, 2); /* two 4K pages */
	put_u16(segment + 22, 0x8002); /* MULTI points at overflow index 2 */
	put_u16(segment + 24, 0xffff); /* second page has no chain */
	put_u16(segment + 26, 0x10);
	put_u16(segment + 28, 0x8020); /* LAST, start offset 0x20 */
}

static void append_chained_fixups_two_segments(unsigned char *file,
					       unsigned int second_format)
{
	unsigned char *low_segment = file + PAGEZERO_OFF;
	unsigned char *payload = file + 512;
	unsigned char *starts = payload + 28;
	unsigned char *low_starts = starts + 12;
	unsigned char *text_starts = starts + 36;

	put_name(low_segment + 8, "__LOW");
	put_u64(low_segment + 24, 0x800);
	put_u64(low_segment + 32, 0x1000);
	put_u64(low_segment + 40, 0x300);
	put_u64(low_segment + 48, 0x100);
	put_u32(low_segment + 56, 3);
	put_u32(low_segment + 60, 3);
	append_command(file, LC_DYLD_CHAINED_FIXUPS, 16);
	put_u32(file + COMMANDS_END + 8, 512);
	put_u32(file + COMMANDS_END + 12, 96);
	put_u32(payload + 4, 28);
	put_u32(payload + 8, 96);
	put_u32(payload + 12, 96);
	put_u32(payload + 20, 1);
	put_u32(starts, 2);
	put_u32(starts + 4, 12);
	put_u32(starts + 8, 36);
	put_u32(low_starts, 24);
	put_u32(low_starts + 4, 0x00021000U); /* 4K page, DYLD_CHAINED_PTR_64 */
	put_u64(low_starts + 8, 0xfffffffffffff800ULL);
	put_u16(low_starts + 20, 1);
	put_u16(low_starts + 22, 0x10);
	put_u32(text_starts, 26);
	put_u32(text_starts + 4, (second_format << 16) | 0x1000U);
	put_u64(text_starts + 8, 0); /* __TEXT preferred load address */
	put_u16(text_starts + 20, 2);
	put_u16(text_starts + 22, 0x10);
	put_u16(text_starts + 24, 0xffff);
}

static void append_chained_fixups_lower_segment_valid(unsigned char *file)
{
	append_chained_fixups_two_segments(file, 2);
	put_u32(file + 512 + 28 + 12 + 16, 1);
	put_u32(file + 512 + 28 + 36 + 16, 1);
}

static void append_chained_fixups_mixed_formats(unsigned char *file)
{
	append_chained_fixups_two_segments(file, 6);
}

static void append_chained_fixups_conflicting_max_valid_pointer(
	unsigned char *file)
{
	append_chained_fixups_two_segments(file, 2);
	put_u32(file + 512 + 28 + 12 + 16, 1);
	put_u32(file + 512 + 28 + 36 + 16, 2);
}

static void append_chained_fixups_ctf_omission(unsigned char *file)
{
	unsigned char *ctf = file + COMMANDS_END;
	unsigned char *linkedit = ctf + 72;
	unsigned char *fixups_command = linkedit + 72;
	unsigned char *payload = file + 1024;
	unsigned char *starts = payload + 28;
	unsigned char *text_starts = starts + 16;

	put_u32(ctf, LC_SEGMENT_64);
	put_u32(ctf + 4, 72);
	put_name(ctf + 8, "__CTF");
	put_u64(ctf + 24, 0x3000);
	put_u32(linkedit, LC_SEGMENT_64);
	put_u32(linkedit + 4, 72);
	put_name(linkedit + 8, "__LINKEDIT");
	put_u64(linkedit + 24, 0x3000);
	put_u64(linkedit + 32, 0x1000);
	put_u64(linkedit + 40, 0x300);
	put_u64(linkedit + 48, 0x100);
	put_u32(linkedit + 56, 1);
	put_u32(linkedit + 60, 1);
	put_u32(fixups_command, LC_DYLD_CHAINED_FIXUPS);
	put_u32(fixups_command + 4, 16);
	put_u32(fixups_command + 8, 1024);
	put_u32(fixups_command + 12, 72);
	put_u32(file + 16, 6);
	put_u32(file + 20, COMMAND_BYTES + 160);
	put_u32(payload + 4, 28);
	put_u32(payload + 8, 72);
	put_u32(payload + 12, 72);
	put_u32(payload + 20, 1);
	put_u32(starts, 3); /* stale linker table omits only trailing LINKEDIT */
	put_u32(starts + 4, 0);
	put_u32(starts + 8, 16);
	put_u32(starts + 12, 0); /* zero-sized __CTF has no fixups */
	put_u32(text_starts, 26);
	put_u32(text_starts + 4, 0x000b1000U);
	put_u64(text_starts + 8, 0);
	put_u16(text_starts + 20, 2);
	put_u16(text_starts + 22, 0x10);
	put_u16(text_starts + 24, 0xffff);
}

static void append_chained_fixups_ctf_nonzero(unsigned char *file)
{
	append_chained_fixups_ctf_omission(file);
	put_u64(file + COMMANDS_END + 32, 0x1000);
	put_u64(file + COMMANDS_END + 72 + 24, 0x4000);
}

static void append_chained_fixups_zero_vm_other_name(unsigned char *file)
{
	append_chained_fixups_ctf_omission(file);
	put_name(file + COMMANDS_END + 8, "__OTHER");
}

static void append_chained_fixups_prefix_valid(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 28 + 12, 24); /* header plus one page_start */
	put_u16(file + 512 + 28 + 12 + 20, 1); /* valid prefix of two VM pages */
}

static void append_chained_fixups_second_page_start_not_file_backed(
	unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u16(file + 512 + 28 + 12 + 24, 0x10);
}

static void append_chained_fixups_starts_after_imports(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 4, 40);
	put_u32(file + 512 + 8, 36);
}

static void append_chained_fixups_symbols_before_imports(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 12, 64);
}

static void append_chained_fixups_outside_file(unsigned char *file)
{
	append_command(file, LC_DYLD_CHAINED_FIXUPS, 16);
	put_u32(file + COMMANDS_END + 8, FILE_BYTES - 8);
	put_u32(file + COMMANDS_END + 12, 32);
}

static void append_chained_fixups_truncated_header(unsigned char *file)
{
	append_command(file, LC_DYLD_CHAINED_FIXUPS, 16);
	put_u32(file + COMMANDS_END + 8, 512);
	put_u32(file + COMMANDS_END + 12, 27);
}

static void append_chained_fixups_bad_version(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512, 1);
}

static void append_chained_fixups_bad_starts_offset(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 4, 0xfffffff8U);
}

static void append_chained_fixups_unaligned_starts_offset(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 4, 29);
}

static void append_chained_fixups_bad_import_extent(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 8, 68);
	put_u32(file + 512 + 16, 2);
	put_u32(file + 512 + 20, 1);
}

static void append_chained_fixups_imports_overlap_starts(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 8, 40);
	put_u32(file + 512 + 16, 1);
	put_u32(file + 512 + 20, 1);
}

static void append_chained_fixups_symbols_overlap_starts(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 12, 40);
}

static void append_chained_fixups_imports_overlap_symbols(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 8, 40);
	put_u32(file + 512 + 12, 42);
	put_u32(file + 512 + 16, 1);
	put_u32(file + 512 + 20, 1);
}

static void append_chained_fixups_bad_segment_count(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 28, 1);
}

static void append_chained_fixups_segment_offset_outside_blob(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 28 + 8, 64);
}

static void append_chained_fixups_starts_table_short(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 8, 36);
	put_u32(file + 512 + 12, 36);
}

static void append_chained_fixups_segment_offset_overlaps_table(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 28 + 8, 4);
}

static void append_chained_fixups_segment_size_short(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 28 + 12, 22);
}

static void append_chained_fixups_bad_page_size(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u16(file + 512 + 28 + 12 + 4, 0x2000);
}

static void append_chained_fixups_bad_pointer_format(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u16(file + 512 + 28 + 12 + 6, 0x7777);
}

static void append_chained_fixups_bad_segment_vm_offset(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u64(file + 512 + 28 + 12 + 8, 8);
}

static void append_chained_fixups_page_count_exceeds_segment(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 28 + 12, 28);
	put_u16(file + 512 + 28 + 12 + 20, 3);
}

static void append_chained_fixups_zero_page_count(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u32(file + 512 + 28 + 12, 22);
	put_u16(file + 512 + 28 + 12 + 20, 0);
}

static void append_chained_fixups_simple_start_outside_page(unsigned char *file)
{
	append_chained_fixups_valid(file);
	put_u16(file + 512 + 28 + 12 + 22, 0x0fff);
}

static void append_dummy_segments(unsigned char *file, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		unsigned char *command = file + COMMANDS_END + i * 72U;

		put_u32(command, LC_SEGMENT_64);
		put_u32(command + 4, 72);
		put_name(command + 8, "__DUMMY");
	}
	put_u32(file + 16, 3 + count);
	put_u32(file + 20, COMMAND_BYTES + count * 72U);
}

static void too_many_segments(unsigned char *file)
{
	append_dummy_segments(file, XNU_MACHO_X86_64_MAX_SEGMENTS - 1U);
}

static void append_dyld_info_valid(unsigned char *file)
{
	append_command(file, LC_DYLD_INFO_ONLY, 48);
	put_u32(file + COMMANDS_END + 8, 512);
	put_u32(file + COMMANDS_END + 12, 16);
}

static void append_dyld_info_outside_file(unsigned char *file)
{
	append_command(file, LC_DYLD_INFO_ONLY, 48);
	put_u32(file + COMMANDS_END + 16, FILE_BYTES - 8);
	put_u32(file + COMMANDS_END + 20, 32);
}

static void append_unknown_optional_command(unsigned char *file)
{
	append_command(file, 0x1234U, 8);
}

static void append_two_unaligned_optional_commands(unsigned char *file)
{
	put_u32(file + COMMANDS_END, 0x1234U);
	put_u32(file + COMMANDS_END + 4, 12);
	put_u32(file + COMMANDS_END + 12, 0x1235U);
	put_u32(file + COMMANDS_END + 16, 12);
	put_u32(file + 16, 5);
	put_u32(file + 20, COMMAND_BYTES + 24);
}

static void append_main_command(unsigned char *file)
{
	append_command(file, 0x80000028U, 24);
}

static void append_unknown_required_command(unsigned char *file)
{
	append_command(file, 0x80001234U, 8);
}

static int expect_metadata(const char *name, void (*mutate)(unsigned char *),
			   unsigned int expected_flags,
			   unsigned int expected_linkedit_count,
			   unsigned int expected_unclassified_count)
{
	unsigned char file[FILE_BYTES];
	unsigned char original[FILE_BYTES];
	struct xnu_macho_x86_64_info info;
	int result;

	make_valid_image(file, CPU_SUBTYPE_X86_64_ALL);
	mutate(file);
	memcpy(original, file, sizeof(file));
	memset(&info, 0xa5, sizeof(info));
	result = xnu_macho_x86_64_parse(file, FILE_BYTES, &info);
	if (result || info.inspection_flags != expected_flags ||
	    info.linkedit_data_command_count != expected_linkedit_count ||
	    info.unclassified_load_command_count != expected_unclassified_count ||
	    info.vm_low != 0x1000 || info.vm_high != 0x3000 || info.entry != 0x1100 ||
	    ((expected_flags & XNU_MACHO_X86_64_HAS_CHAINED_FIXUPS) &&
	     (info.chained_starts_segment_count != 2 ||
	      info.chained_starts_nonempty_segment_count != 1 ||
	      info.segments[1].chained_pointer_format != 11 ||
	      info.segments[1].chained_page_size != 0x1000 ||
	      !info.segments[1].chained_page_count)) ||
	    memcmp(file, original, sizeof(file))) {
		fprintf(stderr, "FAIL: %s (result=%d flags=%x linkedit=%u unknown=%u)\n",
			name, result, info.inspection_flags,
			info.linkedit_data_command_count,
			info.unclassified_load_command_count);
		return 1;
	}
	return 0;
}

static int expect_multi_chained_fixups_rejected(void)
{
	unsigned char file[FILE_BYTES];
	unsigned char original[FILE_BYTES];
	struct xnu_macho_x86_64_info info;
	int result;

	make_valid_image(file, CPU_SUBTYPE_X86_64_ALL);
	append_chained_fixups_multi_starts(file);
	memcpy(original, file, sizeof(file));
	result = xnu_macho_x86_64_parse(file, FILE_BYTES, &info);
	if (result != -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP || !info_is_zero(&info) ||
	    memcmp(file, original, sizeof(file))) {
		fprintf(stderr, "FAIL: unsupported x86_64 MULTI/LAST starts (result=%d)\n",
			result);
		return 1;
	}
	return 0;
}

static int expect_ctf_omission(const char *name,
			       void (*mutate)(unsigned char *))
{
	unsigned char file[FILE_BYTES];
	unsigned char original[FILE_BYTES];
	struct xnu_macho_x86_64_info info;
	int result;

	make_valid_image(file, CPU_SUBTYPE_X86_64_ALL);
	mutate(file);
	memcpy(original, file, sizeof(file));
	result = xnu_macho_x86_64_parse(file, FILE_BYTES, &info);
	if (result || info.segment_count != 4 || info.vm_low != 0x1000 ||
	    info.vm_high != 0x4000 || info.entry != 0x1100 ||
	    info.chained_starts_segment_count != 3 ||
	    info.chained_starts_nonempty_segment_count != 1 ||
	    info.segments[1].chained_pointer_format != 11 ||
	    info.segments[1].chained_page_count != 2 ||
	    info.segments[2].vmsize ||
	    memcmp(info.segments[3].name, "__LINKEDIT", 10) ||
	    memcmp(file, original, sizeof(file))) {
		fprintf(stderr, "FAIL: %s (result=%d starts=%u segments=%u)\n",
			name, result, info.chained_starts_segment_count,
			info.segment_count);
		return 1;
	}
	return 0;
}

static int expect_lower_segment_fixups(void)
{
	unsigned char file[FILE_BYTES];
	struct xnu_macho_x86_64_info info;
	int result;

	make_valid_image(file, CPU_SUBTYPE_X86_64_ALL);
	append_chained_fixups_lower_segment_valid(file);
	result = xnu_macho_x86_64_parse(file, FILE_BYTES, &info);
	if (result || info.vm_low != 0x800 || info.entry != 0x1100 ||
	    info.chained_starts_segment_count != 2 ||
	    info.chained_starts_nonempty_segment_count != 2 ||
	    info.segments[0].chained_pointer_format != 2 ||
	    info.segments[0].chained_page_count != 1 ||
	    info.segments[1].chained_pointer_format != 2 ||
	    info.segments[1].chained_page_count != 2) {
		fprintf(stderr, "FAIL: preferred-load-address segment offsets (result=%d)\n",
			result);
		return 1;
	}
	return 0;
}

static int expect_page_prefix_fixups(void)
{
	unsigned char file[FILE_BYTES];
	struct xnu_macho_x86_64_info info;
	int result;

	make_valid_image(file, CPU_SUBTYPE_X86_64_ALL);
	append_chained_fixups_prefix_valid(file);
	result = xnu_macho_x86_64_parse(file, FILE_BYTES, &info);
	if (result || info.chained_starts_segment_count != 2 ||
	    info.chained_starts_nonempty_segment_count != 1 ||
	    info.segments[1].chained_page_size != 0x1000 ||
	    info.segments[1].chained_page_count != 1) {
		fprintf(stderr, "FAIL: starts page prefix remains one page (result=%d count=%u)\n",
			result, info.segments[1].chained_page_count);
		return 1;
	}
	return 0;
}

static int expect_segment_plan_capacity(void)
{
	unsigned char file[FILE_BYTES];
	struct xnu_macho_x86_64_info info;
	int result;

	make_valid_image(file, CPU_SUBTYPE_X86_64_ALL);
	append_dummy_segments(file, XNU_MACHO_X86_64_MAX_SEGMENTS - 2U);
	result = xnu_macho_x86_64_parse(file, FILE_BYTES, &info);
	if (result || info.segment_count != XNU_MACHO_X86_64_MAX_SEGMENTS ||
	    memcmp(info.segments[XNU_MACHO_X86_64_MAX_SEGMENTS - 1U].name,
		   "__DUMMY", 7)) {
		fprintf(stderr, "FAIL: segment plan capacity (result=%d count=%u)\n",
			result, info.segment_count);
		return 1;
	}
	return 0;
}

static int expect_rejected(const char *name, int expected,
			   unsigned int file_size,
			   void (*mutate)(unsigned char *))
{
	unsigned char file[FILE_BYTES];
	unsigned char original[FILE_BYTES];
	struct xnu_macho_x86_64_info info;
	int result;

	make_valid_image(file, CPU_SUBTYPE_X86_64_ALL);
	if (mutate)
		mutate(file);
	memcpy(original, file, sizeof(file));
	memset(&info, 0xa5, sizeof(info));
	result = xnu_macho_x86_64_parse(file, file_size, &info);
	if (result != expected || !info_is_zero(&info) ||
	    memcmp(file, original, sizeof(file))) {
		fprintf(stderr, "FAIL: %s (result=%d expected=%d output_zero=%d input_unchanged=%d)\n",
			name, result, expected, info_is_zero(&info),
			!memcmp(file, original, sizeof(file)));
		return 1;
	}
	return 0;
}

static void wrong_cpu(unsigned char *f) { put_u32(f + 4, 0x0100000cU); }
static void unknown_subtype(unsigned char *f) { put_u32(f + 8, 2); }
static void subtype_capability(unsigned char *f) { put_u32(f + 8, 0x80000003U); }
static void fileset_type(unsigned char *f) { put_u32(f + 12, 12); }
static void nonzero_reserved(unsigned char *f) { put_u32(f + 28, 1); }
static void command_area_overflow(unsigned char *f) { put_u32(f + 20, 0xffffffffU); }
static void command_area_unaligned(unsigned char *f) { put_u32(f + 20, COMMAND_BYTES + 1); }
static void command_area_has_unclaimed_bytes(unsigned char *f) { put_u32(f + 20, COMMAND_BYTES + 8); }
static void command_count_too_high(unsigned char *f) { put_u32(f + 16, 4); }
static void pagezero_command_too_small(unsigned char *f) { put_u32(f + PAGEZERO_OFF + 4, 68); }
static void pagezero_command_too_large(unsigned char *f) { put_u32(f + PAGEZERO_OFF + 4, 0xffffffffU); }
static void text_nsects_too_high(unsigned char *f) { put_u32(f + TEXT_OFF + 64, 2); }
static void text_filesize_exceeds_vmsize(unsigned char *f) { put_u64(f + TEXT_OFF + 48, 0x3000); }
static void text_file_range_outside_image(unsigned char *f) { put_u64(f + TEXT_OFF + 40, 0x1000); }
static void text_vm_range_overflows(unsigned char *f) { put_u64(f + TEXT_OFF + 24, 0xfffffffffffff000ULL); }
static void invalid_protection(unsigned char *f) { put_u32(f + TEXT_OFF + 56, 1); }
static void section_vm_range_outside_segment(unsigned char *f)
{
	put_u64(f + SECTION_OFF + 32, 0x2ff0);
	put_u64(f + SECTION_OFF + 40, 0x20);
}
static void section_file_range_outside_segment(unsigned char *f)
{
	put_u32(f + SECTION_OFF + 48, 0x1f8);
}
static void section_file_vm_offsets_mismatch(unsigned char *f)
{
	put_u64(f + SECTION_OFF + 32, 0x1300);
	put_u32(f + SECTION_OFF + 48, 0x1d0);
}
static void section_name_mismatch(unsigned char *f) { put_name(f + SECTION_OFF + 16, "__DATA"); }
static void section_alignment_invalid(unsigned char *f) { put_u32(f + SECTION_OFF + 52, 64); }
static void section_reloc_range_invalid(unsigned char *f)
{
	put_u32(f + SECTION_OFF + 56, FILE_BYTES - 4);
	put_u32(f + SECTION_OFF + 60, 1);
}
static void thread_flavor_unsupported(unsigned char *f) { put_u32(f + THREAD_OFF + 8, 1); }
static void thread_count_wrong(unsigned char *f) { put_u32(f + THREAD_OFF + 12, 41); }
static void thread_cmdsize_short(unsigned char *f) { put_u32(f + THREAD_OFF + 4, THREAD_SIZE - 4); }
static void entry_not_in_text(unsigned char *f) { put_u64(f + RIP_OFFSET, 0x3100); }
static void entry_in_nonexec_segment(unsigned char *f)
{
	put_u32(f + TEXT_OFF + 56, 1);
	put_u32(f + TEXT_OFF + 60, 1);
}
static void pagezero_has_protection(unsigned char *f) { put_u32(f + PAGEZERO_OFF + 60, 1); }
static void drop_thread_count(unsigned char *f) { put_u32(f + 16, 2); }

static int expect_duplicate_thread_rejected(void)
{
	unsigned char file[FILE_BYTES];
	unsigned char original[FILE_BYTES];
	struct xnu_macho_x86_64_info info;
	int result;

	make_valid_image(file, CPU_SUBTYPE_X86_64_ALL);
	memcpy(file + COMMANDS_END, file + THREAD_OFF, THREAD_SIZE);
	put_u32(file + 16, 4);
	put_u32(file + 20, COMMAND_BYTES + THREAD_SIZE);
	memcpy(original, file, sizeof(file));
	memset(&info, 0xa5, sizeof(info));
	result = xnu_macho_x86_64_parse(file, sizeof(file), &info);
	if (result != -XNU_MACHO_X86_64_PARSE_ENOEXEC || !info_is_zero(&info) ||
	    memcmp(file, original, sizeof(file))) {
		fprintf(stderr, "FAIL: duplicate thread command (result=%d)\n", result);
		return 1;
	}
	return 0;
}

int main(void)
{
	int failures = 0;

	failures += expect_ok("x86_64 ALL", CPU_SUBTYPE_X86_64_ALL);
	failures += expect_ok("x86_64 H", CPU_SUBTYPE_X86_64_H);
	failures += expect_rejected("truncated header", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    HEADER_SIZE - 1, NULL);
	failures += expect_rejected("wrong CPU type", -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, wrong_cpu);
	failures += expect_rejected("unknown CPU subtype", -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, unknown_subtype);
	failures += expect_rejected("unsupported subtype capability bits", -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, subtype_capability);
	failures += expect_rejected("fileset filetype", -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, fileset_type);
	failures += expect_rejected("nonzero reserved header field", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, nonzero_reserved);
	failures += expect_rejected("command area overflow", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, command_area_overflow);
	failures += expect_rejected("unaligned command area", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, command_area_unaligned);
	failures += expect_rejected("unclaimed load command bytes", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, command_area_has_unclaimed_bytes);
	failures += expect_rejected("two unaligned commands in aligned command area",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_two_unaligned_optional_commands);
	failures += expect_rejected("command count exceeds area", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, command_count_too_high);
	failures += expect_rejected("undersized segment command", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, pagezero_command_too_small);
	failures += expect_rejected("segment command exceeds command area", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, pagezero_command_too_large);
	failures += expect_rejected("section table exceeds command", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, text_nsects_too_high);
	failures += expect_rejected("filesize exceeds vmsize", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, text_filesize_exceeds_vmsize);
	failures += expect_rejected("segment file range outside image", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, text_file_range_outside_image);
	failures += expect_rejected("segment VM range overflows", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, text_vm_range_overflows);
	failures += expect_rejected("initial protection exceeds maximum", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, invalid_protection);
	failures += expect_rejected("section VM range outside segment", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, section_vm_range_outside_segment);
	failures += expect_rejected("section file range outside segment", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, section_file_range_outside_segment);
	failures += expect_rejected("section file and VM offsets disagree",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, section_file_vm_offsets_mismatch);
	failures += expect_rejected("section segment name mismatch", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, section_name_mismatch);
	failures += expect_rejected("invalid section alignment", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, section_alignment_invalid);
	failures += expect_rejected("relocation range outside file", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, section_reloc_range_invalid);
	failures += expect_rejected("unsupported thread flavor", -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, thread_flavor_unsupported);
	failures += expect_rejected("wrong thread state count", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, thread_count_wrong);
	failures += expect_rejected("short thread command", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, thread_cmdsize_short);
	failures += expect_rejected("entry outside executable segment", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, entry_not_in_text);
	failures += expect_rejected("entry in non-executable segment", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, entry_in_nonexec_segment);
	failures += expect_rejected("PAGEZERO has protection", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, pagezero_has_protection);
	failures += expect_rejected("missing LC_UNIXTHREAD", -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, drop_thread_count);
	failures += expect_rejected("chained-fixup extent outside file",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_outside_file);
	failures += expect_rejected("truncated chained-fixups header",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_truncated_header);
	failures += expect_rejected("unsupported chained-fixups version",
				    -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, append_chained_fixups_bad_version);
	failures += expect_rejected("unsupported chained imports format",
				    -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, append_chained_fixups_bad_imports_format);
	failures += expect_rejected("unsupported chained symbols format",
				    -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, append_chained_fixups_bad_symbols_format);
	failures += expect_rejected("chained-fixups starts offset outside blob",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_bad_starts_offset);
	failures += expect_rejected("chained-fixups starts offset unaligned",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_unaligned_starts_offset);
	failures += expect_rejected("chained-fixups import table outside blob",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_bad_import_extent);
	failures += expect_rejected("chained imports overlap starts records",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_imports_overlap_starts);
	failures += expect_rejected("chained symbols overlap starts records",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_symbols_overlap_starts);
	failures += expect_rejected("chained imports overlap symbols",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_imports_overlap_symbols);
	failures += expect_rejected("imports require a symbols pool",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_import_without_symbols);
	failures += expect_rejected("imports require a nonempty symbols pool",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_import_with_empty_symbols);
	failures += expect_rejected("import symbols offset cannot point into header",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_import_symbols_offset_in_header);
	failures += expect_rejected("chained starts segment count mismatch",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_bad_segment_count);
	failures += expect_rejected("starts region is out of order with imports",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_starts_after_imports);
	failures += expect_rejected("symbols region is out of order with imports",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_symbols_before_imports);
	failures += expect_rejected("chained starts offset table truncated",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_starts_table_short);
	failures += expect_rejected("chained segment record overlaps offset table",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_segment_offset_overlaps_table);
	failures += expect_rejected("chained segment record offset outside blob",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_segment_offset_outside_blob);
	failures += expect_rejected("chained segment record too short",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_segment_size_short);
	failures += expect_rejected("unsupported chained page size",
				    -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, append_chained_fixups_bad_page_size);
	failures += expect_rejected("unsupported chained pointer format",
				    -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, append_chained_fixups_bad_pointer_format);
	failures += expect_rejected("chained segment VM offset mismatch",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_bad_segment_vm_offset);
	failures += expect_rejected("chained page count exceeds segment",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_page_count_exceeds_segment);
	failures += expect_rejected("chained starts has zero page count",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_zero_page_count);
	failures += expect_rejected("chained page start outside page",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_simple_start_outside_page);
	failures += expect_rejected("second-page start is not file-backed",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES,
				    append_chained_fixups_second_page_start_not_file_backed);
	failures += expect_rejected("mixed chained pointer formats",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_mixed_formats);
	failures += expect_rejected("conflicting chained max_valid_pointer values",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES,
				    append_chained_fixups_conflicting_max_valid_pointer);
	failures += expect_rejected("nonzero omitted segment before LINKEDIT",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_chained_fixups_ctf_nonzero);
	failures += expect_rejected("segment plan exceeds its bounded capacity",
				    -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, too_many_segments);
	failures += expect_rejected("dyld-info extent outside file",
				    -XNU_MACHO_X86_64_PARSE_ENOEXEC,
				    FILE_BYTES, append_dyld_info_outside_file);
	failures += expect_rejected("unknown required command",
				    -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, append_unknown_required_command);
	failures += expect_rejected("LC_MAIN is unsupported",
				    -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP,
				    FILE_BYTES, append_main_command);
	failures += expect_duplicate_thread_rejected();
	failures += expect_metadata("chained-fixup command is flagged",
				    append_chained_fixups_valid,
				    XNU_MACHO_X86_64_HAS_CHAINED_FIXUPS, 1, 0);
	failures += expect_metadata("chained-fixup import extent is bounded",
				    append_chained_fixups_with_import,
				    XNU_MACHO_X86_64_HAS_CHAINED_FIXUPS, 1, 0);
	failures += expect_multi_chained_fixups_rejected();
	failures += expect_page_prefix_fixups();
	failures += expect_lower_segment_fixups();
	failures += expect_ctf_omission("zero-sized CTF omission matches dyld",
					append_chained_fixups_ctf_omission);
	failures += expect_ctf_omission("zero-sized omitted name is not constrained",
					append_chained_fixups_zero_vm_other_name);
	failures += expect_segment_plan_capacity();
	failures += expect_metadata("dyld-info extents are flagged",
				    append_dyld_info_valid,
				    XNU_MACHO_X86_64_HAS_DYLD_INFO, 0, 0);
	failures += expect_metadata("unknown optional command is counted",
				    append_unknown_optional_command,
				    XNU_MACHO_X86_64_HAS_UNCLASSIFIED_COMMANDS, 0, 1);

	if (failures) {
		fprintf(stderr, "%d Mach-O parser case(s) failed\n", failures);
		return 1;
	}
	puts("x86_64 Mach-O parser fixtures passed");
	return 0;
}
