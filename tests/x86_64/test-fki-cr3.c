// SPDX-License-Identifier: GPL-2.0-only
#include "fki_x86_64_cr3.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define PAGE_BYTES UINT64_C(4096)

static void test_legacy_cache_flags(void)
{
	struct fki_x86_cr3 decoded;
	fki_u64 root = 0x12345000;

	assert(fki_x86_decode_cr3(root | FKI_X86_CR3_PCD | FKI_X86_CR3_PWT,
				  36, false, false, &decoded) == 0);
	assert(decoded.root_pa == root && decoded.pcid == 0);
	assert(decoded.cache_flags == (FKI_X86_CR3_PCD | FKI_X86_CR3_PWT));
	assert(fki_x86_decode_cr3(root | 1, 36, false, true, &decoded) == -EINVAL);
}

static void test_pcid_and_width(void)
{
	struct fki_x86_cr3 decoded;
	fki_u64 root = (1ULL << 47) | 0x2000;
	fki_u64 saved = root | 0xabc;

	assert(fki_x86_decode_cr3(saved, 48, true, true, &decoded) == 0);
	assert(decoded.root_pa == root && decoded.pcid == 0xabc);
	assert(decoded.cache_flags == 0);
	assert(fki_x86_decode_cr3(saved, 48, true, false, &decoded) == -EOPNOTSUPP);
	assert(fki_x86_decode_cr3(saved, 47, true, true, &decoded) == -EINVAL);
	assert(fki_x86_decode_cr3(saved | (1ULL << 63), 52, true, true, &decoded) == -EINVAL);
	assert(fki_x86_decode_cr3(saved | (1ULL << 62), 52, true, true, &decoded) == -EINVAL);
	assert(fki_x86_decode_cr3(saved | (1ULL << 61), 52, true, true, &decoded) == -EINVAL);
	assert(fki_x86_decode_cr3(saved | (1ULL << 52), 52, true, true, &decoded) == -EINVAL);
	assert(fki_x86_decode_cr3(0x1000, 31, true, true, &decoded) == -EINVAL);
	assert(fki_x86_decode_cr3(0x1000, 53, true, true, &decoded) == -EINVAL);
	assert(fki_x86_decode_cr3(0x1000, 48, true, true, NULL) == -EINVAL);
}

static void test_root_ownership(void)
{
	struct fki_x86_cr3 decoded = { .root_pa = 0x3000 };

	assert(fki_x86_cr3_check_root(&decoded, 0x2000, 2 * PAGE_BYTES) == 0);
	assert(fki_x86_cr3_check_root(&decoded, 0x3000, PAGE_BYTES) == 0);
	assert(fki_x86_cr3_check_root(&decoded, 0x3001, 2 * PAGE_BYTES) == -ERANGE);
	assert(fki_x86_cr3_check_root(&decoded, 0x2000, 2 * PAGE_BYTES - 1) == -ERANGE);
	assert(fki_x86_cr3_check_root(&decoded, 0x2000, UINT64_MAX) == -EINVAL);
	decoded.root_pa++;
	assert(fki_x86_cr3_check_root(&decoded, 0x2000, 3 * PAGE_BYTES) == -EINVAL);
}

int main(void)
{
	test_legacy_cache_flags();
	test_pcid_and_width();
	test_root_ownership();
	puts("FKI x86 CR3 fixtures passed");
	return 0;
}
