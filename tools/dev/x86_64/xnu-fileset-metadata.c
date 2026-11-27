// SPDX-License-Identifier: GPL-2.0-only
/* Read-only MH_FILESET metadata for the Intel Recovery audit. */
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../xnu-ko/macho_parse_x86_64.h"

#define MAX_COLLECTION_SIZE (1ULL << 30)

static void print_fileset(const struct xnu_macho_x86_64_info *info)
{
	printf("{\"status\":\"accepted\",\"parser_result\":0,"
	       "\"file_type\":%u,\"cpu_subtype\":%u,"
	       "\"load_command_count\":%u,\"load_command_bytes\":%u,"
	       "\"fileset_entry_count\":%u,"
	       "\"kernel\":{\"file_type\":%u,\"cpu_subtype\":%u,"
	       "\"load_command_count\":%u,\"load_command_bytes\":%u,"
	       "\"fileoff\":\"0x%016" PRIx64 "\",\"vmaddr\":\"0x%016" PRIx64 "\"}}\n",
	       info->file_type, info->cpu_subtype, info->load_command_count,
	       info->load_command_bytes, info->fileset_entry_count,
	       info->kernel_entry_filetype, info->kernel_entry_cpusubtype,
	       info->kernel_entry_ncmds, info->kernel_entry_sizeofcmds,
	       (uint64_t)info->kernel_entry_fileoff,
	       (uint64_t)info->kernel_entry_vmaddr);
}

int main(int argc, char **argv)
{
	static const unsigned char magic64_le[] = { 0xcf, 0xfa, 0xed, 0xfe };
	static const unsigned char fileset_le[] = { 0x0c, 0, 0, 0 };
	struct xnu_macho_x86_64_info info;
	struct stat st;
	const unsigned char *file;
	size_t size;
	int fd, err;

	if (argc != 2)
		return 2;
	fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
	if (fd < 0)
		return 2;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    (uintmax_t)st.st_size > MAX_COLLECTION_SIZE) {
		close(fd);
		return 2;
	}
	size = (size_t)st.st_size;
	file = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (file == MAP_FAILED)
		return 2;

	if (size < 16 || memcmp(file, magic64_le, sizeof(magic64_le)) ||
	    memcmp(file + 12, fileset_le, sizeof(fileset_le))) {
		puts("{\"status\":\"not_fileset\",\"parser_result\":null}");
	} else {
		err = xnu_macho_x86_64_parse(file, (unsigned long)size, &info);
		if (err)
			printf("{\"status\":\"parser_rejected\",\"parser_result\":%d}\n", err);
		else
			print_fileset(&info);
	}
	munmap((void *)file, size);
	return fflush(stdout) == EOF || ferror(stdout) ? 2 : 0;
}
