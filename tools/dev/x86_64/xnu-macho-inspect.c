// SPDX-License-Identifier: GPL-2.0-only
/* Read-only inspector for the parse-only x86_64 Mach-O subset. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../xnu-ko/macho_parse_x86_64.h"

#define MACH_HEADER_64_SIZE 32U
#define MH_MAGIC_64 0xfeedfacfU
#define MH_EXECUTE 2U
#define CPU_SUBTYPE_X86_64_ALL 3U
#define VM_PROT_READ 1U
#define VM_PROT_WRITE 2U
#define VM_PROT_EXECUTE 4U
#define MAX_IMAGE_SIZE (1ULL << 30)

static uint32_t read_u32le(const unsigned char *p)
{
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static int segment_is_pagezero(const unsigned char name[16])
{
	static const unsigned char pagezero[16] = {
		'_', '_', 'P', 'A', 'G', 'E', 'Z', 'E', 'R', 'O'
	};

	return memcmp(name, pagezero, sizeof(pagezero)) == 0;
}

static int read_image(const char *path, unsigned char **data_out, size_t *size_out)
{
	struct stat path_st;
	struct stat st;
	unsigned char *data;
	size_t size, done = 0;
	int fd;

	if (stat(path, &path_st)) {
		fprintf(stderr, "xnu-macho-inspect: cannot stat '%s': %s\n", path,
			strerror(errno));
		return -1;
	}
	if (!S_ISREG(path_st.st_mode)) {
		fprintf(stderr, "xnu-macho-inspect: input must be a regular file\n");
		return -1;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "xnu-macho-inspect: cannot open '%s': %s\n", path,
			strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
	    (uintmax_t)st.st_size > MAX_IMAGE_SIZE ||
	    (uintmax_t)st.st_size > SIZE_MAX || (uintmax_t)st.st_size > ULONG_MAX) {
		fprintf(stderr, "xnu-macho-inspect: input must be a regular file no larger than 1 GiB\n");
		close(fd);
		return -1;
	}
	size = (size_t)st.st_size;
	if (!size) {
		fprintf(stderr, "xnu-macho-inspect: input file is empty\n");
		close(fd);
		return -1;
	}
	data = malloc(size);
	if (!data) {
		fprintf(stderr, "xnu-macho-inspect: cannot allocate %zu bytes\n", size);
		close(fd);
		return -1;
	}
	while (done < size) {
		ssize_t n = read(fd, data + done, size - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			fprintf(stderr, "xnu-macho-inspect: short read from '%s'\n", path);
			free(data);
			close(fd);
			return -1;
		}
		done += (size_t)n;
	}
	if (close(fd)) {
		fprintf(stderr, "xnu-macho-inspect: close failed for '%s': %s\n", path,
			strerror(errno));
		free(data);
		return -1;
	}
	*data_out = data;
	*size_out = size;
	return 0;
}

static int utf8_sequence_length(const unsigned char *p, size_t remaining)
{
	unsigned char second;

	if (!remaining)
		return 0;
	if (p[0] < 0x80)
		return 1;
	if (remaining >= 2 && p[0] >= 0xc2 && p[0] <= 0xdf &&
	    (p[1] & 0xc0) == 0x80)
		return 2;
	if (remaining < 3)
		return 0;
	second = p[1];
	if (p[2] < 0x80 || p[2] > 0xbf)
		return 0;
	if (p[0] == 0xe0)
		return second >= 0xa0 && second <= 0xbf ? 3 : 0;
	if ((p[0] >= 0xe1 && p[0] <= 0xec) ||
	    (p[0] >= 0xee && p[0] <= 0xef))
		return second >= 0x80 && second <= 0xbf ? 3 : 0;
	if (p[0] == 0xed)
		return second >= 0x80 && second <= 0x9f ? 3 : 0;
	if (remaining < 4 || p[3] < 0x80 || p[3] > 0xbf)
		return 0;
	if (p[0] == 0xf0)
		return second >= 0x90 && second <= 0xbf ? 4 : 0;
	if (p[0] >= 0xf1 && p[0] <= 0xf3)
		return second >= 0x80 && second <= 0xbf ? 4 : 0;
	if (p[0] == 0xf4)
		return second >= 0x80 && second <= 0x8f ? 4 : 0;
	return 0;
}

/* Invalid path/name bytes are represented as their U+00XX code points. */
static int json_bytes_string(const unsigned char *value, size_t length)
{
	size_t offset = 0;
	int valid_utf8 = 1;

	putchar('"');
	while (offset < length) {
		unsigned char byte = value[offset];
		int sequence_length;

		if (byte == '"' || byte == '\\') {
			putchar('\\');
			putchar(byte);
			offset++;
		} else if (byte < 0x20) {
			printf("\\u%04x", (unsigned int)byte);
			offset++;
		} else if (byte < 0x80) {
			putchar(byte);
			offset++;
		} else {
			sequence_length = utf8_sequence_length(value + offset, length - offset);
			if (sequence_length) {
				fwrite(value + offset, 1, (size_t)sequence_length, stdout);
				offset += (size_t)sequence_length;
			} else {
				printf("\\u%04x", (unsigned int)byte);
				valid_utf8 = 0;
				offset++;
			}
		}
	}
	putchar('"');
	return valid_utf8;
}

static void json_hex_string(const unsigned char *value, size_t length)
{
	size_t i;

	putchar('"');
	for (i = 0; i < length; i++)
		printf("%02x", value[i]);
	putchar('"');
}

static void json_segment_name(const unsigned char name[16])
{
	size_t length = 0;

	while (length < 16 && name[length])
		length++;
	json_bytes_string(name, length);
}

static const char *subtype_name(uint32_t subtype)
{
	return subtype == CPU_SUBTYPE_X86_64_ALL ? "X86_64_ALL" : "X86_64_H";
}

static void print_report(const char *path, size_t file_size, uint32_t cpusubtype,
			 const struct xnu_macho_x86_64_info *parsed,
			 const xnu_macho_u64 *hib_entry_fileoff)
{
	uint32_t i;
	int path_utf8;

	puts("{");
	printf("  \"file\": ");
	path_utf8 = json_bytes_string((const unsigned char *)path, strlen(path));
	printf(",\n  \"file_path_encoding\": \"%s\",\n  \"file_path_bytes_hex\": ",
	       path_utf8 ? "UTF-8" : "invalid bytes escaped as U+00XX");
	json_hex_string((const unsigned char *)path, strlen(path));
	printf(",\n");
	printf("  \"file_size_bytes\": %zu,\n", file_size);
	printf("  \"format\": \"Mach-O 64-bit little-endian MH_EXECUTE\",\n");
	printf("  \"architecture\": \"x86_64\",\n");
	printf("  \"cpu_subtype\": \"%s\",\n", subtype_name(cpusubtype));
	printf("  \"entry\": {\"source\": \"LC_UNIXTHREAD.x86_THREAD_STATE64.RIP\", \"unslid\": \"0x%016" PRIx64 "\"},\n",
	       (uint64_t)parsed->entry);
	if (hib_entry_fileoff)
		printf("  \"hib_entry\": {\"section\": \"__HIB,__text\", \"file_offset\": \"0x%016" PRIx64 "\"},\n",
		       (uint64_t)*hib_entry_fileoff);
	printf("  \"vm_range\": {\"low\": \"0x%016" PRIx64 "\", \"high\": \"0x%016" PRIx64 "\"},\n",
	       (uint64_t)parsed->vm_low,
	       (uint64_t)parsed->vm_high);
	printf("  \"segment_count\": %u,\n", parsed->segment_count);
	printf("  \"mapped_segment_count\": %u,\n", parsed->mapped_segment_count);
	printf("  \"segments\": [\n");
	for (i = 0; i < parsed->segment_count; i++) {
		const struct xnu_macho_x86_64_segment *segment = &parsed->segments[i];

		printf("    {\"name\": ");
		json_segment_name(segment->name);
		printf(", \"name_bytes_hex\": ");
		json_hex_string(segment->name, sizeof(segment->name));
		printf(", \"vmaddr\": \"0x%016" PRIx64 "\", \"vmsize\": \"0x%016" PRIx64 "\", ",
		       (uint64_t)segment->vmaddr, (uint64_t)segment->vmsize);
		printf("\"fileoff\": \"0x%016" PRIx64 "\", \"filesize\": \"0x%016" PRIx64 "\", ",
		       (uint64_t)segment->fileoff, (uint64_t)segment->filesize);
		printf("\"maxprot\": \"%c%c%c\", \"initprot\": \"%c%c%c\", ",
		       (segment->maxprot & VM_PROT_READ) ? 'r' : '-',
		       (segment->maxprot & VM_PROT_WRITE) ? 'w' : '-',
		       (segment->maxprot & VM_PROT_EXECUTE) ? 'x' : '-',
		       (segment->initprot & VM_PROT_READ) ? 'r' : '-',
		       (segment->initprot & VM_PROT_WRITE) ? 'w' : '-',
		       (segment->initprot & VM_PROT_EXECUTE) ? 'x' : '-');
		printf("\"nsects\": %u, \"flags\": \"0x%08" PRIx32 "\", \"mapped\": %s, ",
		       segment->section_count, segment->flags,
		       (segment->vmsize && !segment_is_pagezero(segment->name)) ? "true" : "false");
		printf("\"chained_starts\": ");
		if (segment->chained_pointer_format)
			printf("{\"pointer_format\": %u, \"page_size\": %u, \"page_count\": %u}",
			       segment->chained_pointer_format,
			       (unsigned int)segment->chained_page_size,
			       (unsigned int)segment->chained_page_count);
		else
			printf("null");
		printf("}%s\n", i + 1 == parsed->segment_count ? "" : ",");
	}
	printf("  ],\n  \"inspection_flags\": %u,\n",
	       parsed->inspection_flags);
	printf("  \"linkedit_metadata\": {\"has_symtab\": %s, \"has_dyld_info\": %s, ",
	       (parsed->inspection_flags & XNU_MACHO_X86_64_HAS_SYMTAB) ? "true" : "false",
	       (parsed->inspection_flags & XNU_MACHO_X86_64_HAS_DYLD_INFO) ? "true" : "false");
	printf("\"has_chained_fixups\": %s, \"has_other_uninterpreted_linkedit\": %s, ",
	       (parsed->inspection_flags & XNU_MACHO_X86_64_HAS_CHAINED_FIXUPS) ? "true" : "false",
	       (parsed->inspection_flags & XNU_MACHO_X86_64_HAS_UNINTERPRETED_LINKEDIT) ? "true" : "false");
	printf("\"chained_starts_segment_count\": %u, \"chained_starts_nonempty_segment_count\": %u},\n",
	       parsed->chained_starts_segment_count,
	       parsed->chained_starts_nonempty_segment_count);
	printf("  \"unclassified_optional_commands\": %u,\n",
	       parsed->unclassified_load_command_count);
	printf("  \"linkedit_data_command_count\": %u,\n",
	       parsed->linkedit_data_command_count);
	printf("  \"load_status\": \"not_loaded\",\n");
	printf("  \"fixups_interpreted\": false,\n");
	printf("  \"execution_attempted\": false\n");
	puts("}");
}

static int inspect_file(const char *path, int inspect_hib)
{
	unsigned char *file = NULL;
	size_t file_size = 0;
	xnu_macho_u64 hib_entry_fileoff;
	struct xnu_macho_x86_64_info parsed;
	int err, status = 1;

	if (read_image(path, &file, &file_size))
		return 2;
	/* Keep the CLI's MH_EXECUTE contract if the parser adds more file types. */
	if (file_size >= MACH_HEADER_64_SIZE && read_u32le(file) == MH_MAGIC_64 &&
	    read_u32le(file + 12) != MH_EXECUTE)
		err = -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP;
	else if (inspect_hib)
		err = xnu_macho_x86_64_inspect_hib_entry(file, (unsigned long)file_size,
						       &parsed, &hib_entry_fileoff);
	else
		err = xnu_macho_x86_64_parse(file, (unsigned long)file_size, &parsed);
	if (err) {
		if (err == -XNU_MACHO_X86_64_PARSE_EOPNOTSUPP) {
			fprintf(stderr, "xnu-macho-inspect: unsupported Mach-O ABI or required load command (-%d)\n",
				XNU_MACHO_X86_64_PARSE_EOPNOTSUPP);
			status = 4;
		} else {
			fprintf(stderr, "xnu-macho-inspect: malformed or invalid Mach-O input (parser result %d)\n",
				err);
			status = 3;
		}
		goto out;
	}
	print_report(path, file_size, read_u32le(file + 8), &parsed,
		     inspect_hib ? &hib_entry_fileoff : NULL);
	if (fflush(stdout) == EOF || ferror(stdout)) {
		fprintf(stderr, "xnu-macho-inspect: failed to write JSON report: %s\n",
			errno ? strerror(errno) : "I/O error");
		status = 6;
		goto out;
	}
	status = 0;
out:
	free(file);
	return status;
}

int main(int argc, char **argv)
{
	if (argc == 2)
		return inspect_file(argv[1], 0);
	if (argc == 3 && !strcmp(argv[1], "--hib-entry-metadata"))
		return inspect_file(argv[2], 1);
	fprintf(stderr, "usage: %s [--hib-entry-metadata] kernel.development\n", argv[0]);
	return 2;
}
