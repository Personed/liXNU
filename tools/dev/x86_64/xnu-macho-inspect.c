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
#define LOAD_COMMAND_SIZE 8U
#define SEGMENT_COMMAND_64_SIZE 72U
#define SECTION_64_SIZE 80U
#define MH_MAGIC_64 0xfeedfacfU
#define MH_EXECUTE 2U
#define CPU_TYPE_X86_64 0x01000007U
#define CPU_SUBTYPE_X86_64_ALL 3U
#define CPU_SUBTYPE_X86_64_H 8U
#define LC_SEGMENT_64 0x19U
#define LC_UNIXTHREAD 0x5U
#define XNU_MACHO_HAS_SYMTAB (1U << 0)
#define XNU_MACHO_HAS_DYLD_INFO (1U << 1)
#define XNU_MACHO_HAS_CHAINED_FIXUPS (1U << 2)
#define XNU_MACHO_HAS_UNINTERPRETED_LINKEDIT (1U << 3)
#define VM_PROT_READ 1U
#define VM_PROT_WRITE 2U
#define VM_PROT_EXECUTE 4U
#define MAX_SEGMENTS 65536U
#define MAX_IMAGE_SIZE (1ULL << 30)

struct segment_record {
	unsigned char name[16];
	uint64_t vmaddr;
	uint64_t vmsize;
	uint64_t fileoff;
	uint64_t filesize;
	uint32_t maxprot;
	uint32_t initprot;
	uint32_t nsects;
	uint32_t flags;
};

struct inspection {
	struct xnu_macho_x86_64_info parsed;
	uint32_t cpusubtype;
	struct segment_record *segments;
	uint32_t segment_count;
	uint32_t mapped_segment_count;
	uint64_t vm_low;
	uint64_t vm_high;
};

static uint32_t read_u32le(const unsigned char *p)
{
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint64_t read_u64le(const unsigned char *p)
{
	return (uint64_t)read_u32le(p) |
	       ((uint64_t)read_u32le(p + 4) << 32);
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
	struct stat st;
	unsigned char *data;
	size_t size, done = 0;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
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

/*
 * The merged parser validates every command and segment. This bounded second
 * pass only collects printable segment fields; the cross-checks make an
 * unexpected parser/inspection disagreement a hard failure.
 */
static int collect_segments(const unsigned char *file, size_t file_size,
			    struct inspection *inspection)
{
	uint32_t ncmds, sizeofcmds, i;
	size_t off, end;
	int have_vm_range = 0;

	if (file_size < MACH_HEADER_64_SIZE || read_u32le(file) != MH_MAGIC_64 ||
	    read_u32le(file + 4) != CPU_TYPE_X86_64 ||
	    read_u32le(file + 12) != MH_EXECUTE)
		return -1;
	inspection->cpusubtype = read_u32le(file + 8);
	if (inspection->cpusubtype != CPU_SUBTYPE_X86_64_ALL &&
	    inspection->cpusubtype != CPU_SUBTYPE_X86_64_H)
		return -1;
	ncmds = read_u32le(file + 16);
	sizeofcmds = read_u32le(file + 20);
	if (sizeofcmds > file_size - MACH_HEADER_64_SIZE)
		return -1;
	off = MACH_HEADER_64_SIZE;
	end = off + sizeofcmds;
	if (inspection->parsed.segment_count > MAX_SEGMENTS)
		return -1;
	inspection->segments = calloc(inspection->parsed.segment_count,
				      sizeof(*inspection->segments));
	if (!inspection->segments)
		return -1;

	for (i = 0; i < ncmds; i++) {
		uint32_t command, command_size;

		if (end - off < LOAD_COMMAND_SIZE)
			return -1;
		command = read_u32le(file + off);
		command_size = read_u32le(file + off + 4);
		if (command_size < LOAD_COMMAND_SIZE || (command_size & 7U) ||
		    command_size > end - off)
			return -1;
		if (command == LC_SEGMENT_64) {
			struct segment_record *segment;
			uint64_t vm_end;
			size_t section_bytes;

			if (command_size < SEGMENT_COMMAND_64_SIZE ||
			    inspection->segment_count >= inspection->parsed.segment_count)
				return -1;
			segment = &inspection->segments[inspection->segment_count];
			memcpy(segment->name, file + off + 8, sizeof(segment->name));
			segment->vmaddr = read_u64le(file + off + 24);
			segment->vmsize = read_u64le(file + off + 32);
			segment->fileoff = read_u64le(file + off + 40);
			segment->filesize = read_u64le(file + off + 48);
			segment->maxprot = read_u32le(file + off + 56);
			segment->initprot = read_u32le(file + off + 60);
			segment->nsects = read_u32le(file + off + 64);
			segment->flags = read_u32le(file + off + 68);
			if (segment->nsects >
			    (command_size - SEGMENT_COMMAND_64_SIZE) / SECTION_64_SIZE)
				return -1;
			section_bytes = (size_t)segment->nsects * SECTION_64_SIZE;
			if (section_bytes != command_size - SEGMENT_COMMAND_64_SIZE ||
			    segment->vmsize > UINT64_MAX - segment->vmaddr)
				return -1;
			vm_end = segment->vmaddr + segment->vmsize;
			inspection->segment_count++;
			if (segment->vmsize && !segment_is_pagezero(segment->name)) {
				if (!have_vm_range || segment->vmaddr < inspection->vm_low)
					inspection->vm_low = segment->vmaddr;
				if (!have_vm_range || vm_end > inspection->vm_high)
					inspection->vm_high = vm_end;
				have_vm_range = 1;
				inspection->mapped_segment_count++;
			}
		}
		off += command_size;
	}
	if (off != end || inspection->segment_count != inspection->parsed.segment_count ||
	    inspection->mapped_segment_count != inspection->parsed.mapped_segment_count ||
	    !have_vm_range || inspection->vm_low != inspection->parsed.vm_low ||
	    inspection->vm_high != inspection->parsed.vm_high)
		return -1;
	return 0;
}

static void json_string(const char *value)
{
	const unsigned char *p = (const unsigned char *)value;
	putchar('"');
	while (*p) {
		if (*p == '"' || *p == '\\') {
			putchar('\\');
			putchar(*p);
		} else if (*p < 0x20 || *p >= 0x7f) {
			printf("\\u%04x", (unsigned int)*p);
		} else {
			putchar(*p);
		}
		p++;
	}
	putchar('"');
}

static void json_segment_name(const unsigned char name[16])
{
	char bounded[17];

	memcpy(bounded, name, 16);
	bounded[16] = '\0';
	json_string(bounded);
}

static const char *subtype_name(uint32_t subtype)
{
	return subtype == CPU_SUBTYPE_X86_64_ALL ? "X86_64_ALL" : "X86_64_H";
}

static void print_report(const char *path, size_t file_size,
			const struct inspection *inspection)
{
	uint32_t i;

	puts("{");
	printf("  \"file\": ");
	json_string(path);
	printf(",\n  \"file_size_bytes\": %zu,\n", file_size);
	printf("  \"format\": \"Mach-O 64-bit little-endian MH_EXECUTE\",\n");
	printf("  \"architecture\": \"x86_64\",\n");
	printf("  \"cpu_subtype\": \"%s\",\n", subtype_name(inspection->cpusubtype));
	printf("  \"entry\": {\"source\": \"LC_UNIXTHREAD.x86_THREAD_STATE64.RIP\", \"unslid\": \"0x%016" PRIx64 "\"},\n",
	       (uint64_t)inspection->parsed.entry);
	printf("  \"vm_range\": {\"low\": \"0x%016" PRIx64 "\", \"high\": \"0x%016" PRIx64 "\"},\n",
	       (uint64_t)inspection->parsed.vm_low,
	       (uint64_t)inspection->parsed.vm_high);
	printf("  \"segment_count\": %u,\n", inspection->segment_count);
	printf("  \"mapped_segment_count\": %u,\n", inspection->mapped_segment_count);
	printf("  \"segments\": [\n");
	for (i = 0; i < inspection->segment_count; i++) {
		const struct segment_record *segment = &inspection->segments[i];

		printf("    {\"name\": ");
		json_segment_name(segment->name);
		printf(", \"vmaddr\": \"0x%016" PRIx64 "\", \"vmsize\": \"0x%016" PRIx64 "\", ",
		       segment->vmaddr, segment->vmsize);
		printf("\"fileoff\": \"0x%016" PRIx64 "\", \"filesize\": \"0x%016" PRIx64 "\", ",
		       segment->fileoff, segment->filesize);
		printf("\"maxprot\": \"%c%c%c\", \"initprot\": \"%c%c%c\", ",
		       (segment->maxprot & VM_PROT_READ) ? 'r' : '-',
		       (segment->maxprot & VM_PROT_WRITE) ? 'w' : '-',
		       (segment->maxprot & VM_PROT_EXECUTE) ? 'x' : '-',
		       (segment->initprot & VM_PROT_READ) ? 'r' : '-',
		       (segment->initprot & VM_PROT_WRITE) ? 'w' : '-',
		       (segment->initprot & VM_PROT_EXECUTE) ? 'x' : '-');
		printf("\"nsects\": %u, \"flags\": \"0x%08" PRIx32 "\", \"mapped\": %s}%s\n",
		       segment->nsects, segment->flags,
		       (segment->vmsize && !segment_is_pagezero(segment->name)) ? "true" : "false",
		       i + 1 == inspection->segment_count ? "" : ",");
	}
	printf("  ],\n  \"inspection_flags\": %u,\n",
	       inspection->parsed.inspection_flags);
	printf("  \"linkedit_metadata\": {\"has_symtab\": %s, \"has_dyld_info\": %s, ",
	       (inspection->parsed.inspection_flags & XNU_MACHO_HAS_SYMTAB) ? "true" : "false",
	       (inspection->parsed.inspection_flags & XNU_MACHO_HAS_DYLD_INFO) ? "true" : "false");
	printf("\"has_chained_fixups\": %s, \"has_other_uninterpreted_linkedit\": %s},\n",
	       (inspection->parsed.inspection_flags & XNU_MACHO_HAS_CHAINED_FIXUPS) ? "true" : "false",
	       (inspection->parsed.inspection_flags & XNU_MACHO_HAS_UNINTERPRETED_LINKEDIT) ? "true" : "false");
	printf("  \"unclassified_optional_commands\": %u,\n",
	       inspection->parsed.unclassified_load_command_count);
	printf("  \"linkedit_data_command_count\": %u,\n",
	       inspection->parsed.linkedit_data_command_count);
	printf("  \"load_status\": \"not_loaded\",\n");
	printf("  \"fixups_interpreted\": false,\n");
	printf("  \"execution_attempted\": false\n");
	puts("}");
}

static int inspect_file(const char *path)
{
	unsigned char *file = NULL;
	size_t file_size = 0;
	struct inspection inspection = { 0 };
	int err, status = 1;

	if (read_image(path, &file, &file_size))
		return 2;
	err = xnu_macho_x86_64_parse(file, (unsigned long)file_size,
				     &inspection.parsed);
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
	if (collect_segments(file, file_size, &inspection)) {
		fprintf(stderr, "xnu-macho-inspect: parser/metadata walk disagreement; refusing report\n");
		status = 5;
		goto out;
	}
	print_report(path, file_size, &inspection);
	status = 0;
out:
	free(inspection.segments);
	free(file);
	return status;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s kernel.development\n", argv[0]);
		return 2;
	}
	return inspect_file(argv[1]);
}
