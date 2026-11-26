// SPDX-License-Identifier: APSL-2.0
/* Synthetic format fixtures only; no Apple kernel image is bundled. */
#include <stdio.h>
#include <string.h>

#include "../../xnu-ko/macho_parse_x86_64.h"

#define FILE_BYTES 1024U
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
	return !info->vm_low && !info->vm_high && !info->entry &&
	       !info->segment_count && !info->mapped_segment_count &&
	       !info->inspection_flags && !info->unclassified_load_command_count &&
	       !info->linkedit_data_command_count && !info->reserved;
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
	append_command(file, LC_DYLD_CHAINED_FIXUPS, 16);
	put_u32(file + COMMANDS_END + 8, 512);
	put_u32(file + COMMANDS_END + 12, 32);
}

static void append_chained_fixups_outside_file(unsigned char *file)
{
	append_command(file, LC_DYLD_CHAINED_FIXUPS, 16);
	put_u32(file + COMMANDS_END + 8, FILE_BYTES - 8);
	put_u32(file + COMMANDS_END + 12, 32);
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
	    memcmp(file, original, sizeof(file))) {
		fprintf(stderr, "FAIL: %s (result=%d flags=%x linkedit=%u unknown=%u)\n",
			name, result, info.inspection_flags,
			info.linkedit_data_command_count,
			info.unclassified_load_command_count);
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
static void command_area_has_unclaimed_bytes(unsigned char *f) { put_u32(f + 20, COMMAND_BYTES + 4); }
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
