/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include "fki.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#include <stdbool.h>
#endif

#define FKI_X86_CR3_PAGE_SIZE (1ULL << 12)
#define FKI_X86_CR3_LOW_MASK (FKI_X86_CR3_PAGE_SIZE - 1)
#define FKI_X86_CR3_PWT (1U << 3)
#define FKI_X86_CR3_PCD (1U << 4)

struct fki_x86_cr3 {
	fki_u64 root_pa;
	unsigned int pcid;
	unsigned int cache_flags;
};

/*
 * Decode a saved CR3 value in long mode for a plain-physical-address
 * profile. The caller must establish that LAM and physical address
 * attributes such as memory encryption are inactive. The no-flush bit
 * belongs to a MOV-to-CR3 operand and is not part of saved state.
 */
static inline int fki_x86_decode_cr3(fki_u64 saved_cr3, unsigned int phys_bits,
				     bool pcide, bool cpu_has_pcid,
				     struct fki_x86_cr3 *decoded)
{
	fki_u64 root_mask, low_mask;

	if (!decoded || phys_bits < 32 || phys_bits > 52)
		return -EINVAL;
	if (pcide && !cpu_has_pcid)
		return -EOPNOTSUPP;

	root_mask = ((1ULL << phys_bits) - 1) & ~FKI_X86_CR3_LOW_MASK;
	low_mask = pcide ? FKI_X86_CR3_LOW_MASK :
		   FKI_X86_CR3_PWT | FKI_X86_CR3_PCD;
	if (saved_cr3 & ~(root_mask | low_mask))
		return -EINVAL;

	decoded->root_pa = saved_cr3 & root_mask;
	decoded->pcid = pcide ? saved_cr3 & low_mask : 0;
	decoded->cache_flags = pcide ? 0 : saved_cr3 & low_mask;
	return 0;
}

/*
 * The caller supplies a live, pinned physical region already owned by
 * this context. A full root page must fit. This does not walk translations
 * or reserve a PCID, both of which are required before entry.
 */
static inline int fki_x86_cr3_check_root(const struct fki_x86_cr3 *decoded,
					fki_u64 owned_pa, fki_u64 owned_bytes)
{
	fki_u64 offset;

	if (!decoded || (decoded->root_pa & FKI_X86_CR3_LOW_MASK) ||
	    owned_bytes < FKI_X86_CR3_PAGE_SIZE ||
	    owned_bytes > ~(fki_u64)0 - owned_pa)
		return -EINVAL;
	if (decoded->root_pa < owned_pa)
		return -ERANGE;
	offset = decoded->root_pa - owned_pa;
	if (offset > owned_bytes - FKI_X86_CR3_PAGE_SIZE)
		return -ERANGE;
	return 0;
}
