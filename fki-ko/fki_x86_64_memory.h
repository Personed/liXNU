/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include "fki_x86_64_context.h"

/*
 * Validation-only memory-reference model for the x86_64 context contract.
 *
 * The structures here are not part of the fixed v1 control-block ABI and do
 * not pin, reserve, or take ownership of real memory.  They describe an
 * abstract caller-supplied ownership map so fixtures can validate that every
 * memory reference in a HOST_BOUND_SHIM control block has a coherent class,
 * does not overlap another reference, and has no undisclosed physical alias.
 * Version 1 is conservative: all linear extents must carry contiguous
 * physical backing information with matching page offsets, and linear or
 * physical backing overlap is rejected at page granularity even when the two
 * byte ranges are distinct.
 */
#define FKI_X86_MEMORY_MAX_EXTENTS 32U

#define FKI_X86_MEMORY_EXTENT_LINEAR (1U << 0)
#define FKI_X86_MEMORY_EXTENT_PHYSICAL (1U << 1)
#define FKI_X86_MEMORY_EXTENT_FLAGS_MASK \
	(FKI_X86_MEMORY_EXTENT_LINEAR | FKI_X86_MEMORY_EXTENT_PHYSICAL)

#define FKI_X86_MEMORY_ACCESS_READ (1U << 0)
#define FKI_X86_MEMORY_ACCESS_WRITE (1U << 1)
#define FKI_X86_MEMORY_ACCESS_EXECUTE (1U << 2)
#define FKI_X86_MEMORY_ACCESS_MASK \
	(FKI_X86_MEMORY_ACCESS_READ | FKI_X86_MEMORY_ACCESS_WRITE | \
	 FKI_X86_MEMORY_ACCESS_EXECUTE)

#define FKI_X86_MEMORY_OWNER_HOST 1U
#define FKI_X86_MEMORY_OWNER_TRANSACTION_PRIVATE 2U
#define FKI_X86_MEMORY_OWNER_FOREIGN_OWNED 3U
#define FKI_X86_MEMORY_OWNER_SHARED 4U

#define FKI_X86_MEMORY_USE_PAGE_TABLES 1U
#define FKI_X86_MEMORY_USE_DESCRIPTOR_TABLE 2U
#define FKI_X86_MEMORY_USE_XSAVE 3U
#define FKI_X86_MEMORY_USE_STACK 4U
#define FKI_X86_MEMORY_USE_EXECUTABLE_TEXT 5U

struct fki_x86_memory_extent {
	fki_u64 linear_start;
	fki_u64 physical_start;
	fki_u64 bytes;
	fki_u32 flags;
	fki_u32 owner_class;
	fki_u32 use_class;
	fki_u32 access;
	fki_u32 reserved[2];
};

static inline bool fki_x86_memory_range_valid(fki_u64 start, fki_u64 bytes)
{
	return bytes && bytes <= ~(fki_u64)0 - start;
}

static inline bool fki_x86_memory_range_contains(fki_u64 outer_start,
						 fki_u64 outer_bytes,
						 fki_u64 inner_start,
						 fki_u64 inner_bytes)
{
	return inner_start >= outer_start &&
	       inner_start - outer_start <= outer_bytes &&
	       inner_bytes <= outer_bytes - (inner_start - outer_start);
}

static inline bool fki_x86_memory_page_ranges_overlap(
	fki_u64 left_start, fki_u64 left_bytes,
	fki_u64 right_start, fki_u64 right_bytes)
{
	fki_u64 left_first = left_start >> 12;
	fki_u64 left_last = (left_start + left_bytes - 1) >> 12;
	fki_u64 right_first = right_start >> 12;
	fki_u64 right_last = (right_start + right_bytes - 1) >> 12;

	return left_first <= right_last && right_first <= left_last;
}

static inline bool fki_x86_memory_physical_range_valid(
	fki_u64 start, fki_u64 bytes, unsigned int physical_bits)
{
	fki_u64 limit;

	if (!fki_x86_memory_range_valid(start, bytes) ||
	    !physical_bits || physical_bits > 52)
		return false;
	limit = 1ULL << physical_bits;
	return start < limit && bytes <= limit - start;
}

static inline int fki_x86_memory_validate_extent(
	const struct fki_x86_memory_extent *extent,
	const struct fki_x86_host_constraints *host)
{
	fki_u32 expected_access;
	bool needs_linear = true;
	bool has_linear = extent->flags & FKI_X86_MEMORY_EXTENT_LINEAR;

	if (!extent->bytes || extent->reserved[0] || extent->reserved[1] ||
	    (extent->flags & ~FKI_X86_MEMORY_EXTENT_FLAGS_MASK) ||
	    !(extent->flags & FKI_X86_MEMORY_EXTENT_PHYSICAL) ||
	    (!has_linear && extent->linear_start) ||
	    !fki_x86_memory_range_valid(extent->linear_start, extent->bytes) ||
	    !fki_x86_memory_range_valid(extent->physical_start, extent->bytes) ||
	    !extent->access || (extent->access & ~FKI_X86_MEMORY_ACCESS_MASK))
		return -EINVAL;

	switch (extent->owner_class) {
	case FKI_X86_MEMORY_OWNER_HOST:
	case FKI_X86_MEMORY_OWNER_TRANSACTION_PRIVATE:
		break;
	case FKI_X86_MEMORY_OWNER_FOREIGN_OWNED:
	case FKI_X86_MEMORY_OWNER_SHARED:
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}

	switch (extent->use_class) {
	case FKI_X86_MEMORY_USE_PAGE_TABLES:
		/* Hardware page walks may write accessed/dirty bits. */
		expected_access = FKI_X86_MEMORY_ACCESS_READ |
				  FKI_X86_MEMORY_ACCESS_WRITE;
		needs_linear = false;
		if (extent->owner_class != FKI_X86_MEMORY_OWNER_HOST)
			return -EINVAL;
		break;
	case FKI_X86_MEMORY_USE_DESCRIPTOR_TABLE:
		expected_access = FKI_X86_MEMORY_ACCESS_READ;
		if (extent->owner_class != FKI_X86_MEMORY_OWNER_HOST)
			return -EINVAL;
		break;
	case FKI_X86_MEMORY_USE_XSAVE:
	case FKI_X86_MEMORY_USE_STACK:
		expected_access = FKI_X86_MEMORY_ACCESS_READ |
				  FKI_X86_MEMORY_ACCESS_WRITE;
		if (extent->owner_class != FKI_X86_MEMORY_OWNER_TRANSACTION_PRIVATE)
			return -EINVAL;
		break;
	case FKI_X86_MEMORY_USE_EXECUTABLE_TEXT:
		expected_access = FKI_X86_MEMORY_ACCESS_READ |
				  FKI_X86_MEMORY_ACCESS_EXECUTE;
		if (extent->owner_class != FKI_X86_MEMORY_OWNER_HOST)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	if (extent->access != expected_access ||
	    (needs_linear && !(extent->flags & FKI_X86_MEMORY_EXTENT_LINEAR)))
		return -EINVAL;
	if ((extent->flags & FKI_X86_MEMORY_EXTENT_LINEAR) &&
	    ((extent->linear_start ^ extent->physical_start) & 0xfff))
		return -EINVAL;
	if ((extent->flags & FKI_X86_MEMORY_EXTENT_LINEAR) &&
	    !fki_x86_canonical_range48(extent->linear_start, extent->bytes))
		return -ERANGE;
	if (!fki_x86_memory_physical_range_valid(extent->physical_start,
						 extent->bytes,
						 host->physical_bits))
		return -ERANGE;
	return 0;
}

static inline const struct fki_x86_memory_extent *
fki_x86_memory_find_linear(const struct fki_x86_memory_extent *extents,
			   unsigned int count, fki_u32 use_class,
			   fki_u64 start, fki_u64 bytes)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		const struct fki_x86_memory_extent *extent = &extents[i];

		if (extent->use_class == use_class &&
		    (extent->flags & FKI_X86_MEMORY_EXTENT_LINEAR) &&
		    fki_x86_memory_range_contains(extent->linear_start,
						  extent->bytes, start, bytes))
			return extent;
	}
	return 0;
}

static inline const struct fki_x86_memory_extent *
fki_x86_memory_find_physical(const struct fki_x86_memory_extent *extents,
			     unsigned int count, fki_u32 use_class,
			     fki_u64 start, fki_u64 bytes)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		const struct fki_x86_memory_extent *extent = &extents[i];

		if (extent->use_class == use_class &&
		    fki_x86_memory_range_contains(extent->physical_start,
						  extent->bytes, start, bytes))
			return extent;
	}
	return 0;
}

static inline int fki_x86_memory_validate_no_overlap(
	const struct fki_x86_memory_extent *extents, unsigned int count)
{
	unsigned int i, j;

	for (i = 0; i < count; i++) {
		for (j = i + 1; j < count; j++) {
			const struct fki_x86_memory_extent *left = &extents[i];
			const struct fki_x86_memory_extent *right = &extents[j];

			if ((left->flags & FKI_X86_MEMORY_EXTENT_LINEAR) &&
			    (right->flags & FKI_X86_MEMORY_EXTENT_LINEAR) &&
			    fki_x86_memory_page_ranges_overlap(left->linear_start,
							  left->bytes,
							  right->linear_start,
							  right->bytes))
				return -EEXIST;
			if (fki_x86_memory_page_ranges_overlap(left->physical_start,
							  left->bytes,
							  right->physical_start,
							  right->bytes))
				return -EEXIST;
		}
	}
	return 0;
}

/*
 * Validate the abstract ownership map for a HOST_BOUND_SHIM control block.
 * The caller must still establish real pinning, translation ownership, and
 * lifetime outside this helper.  RSP is interpreted as the custom pre-call
 * entry ABI stack pointer, so at least one return-address slot below it must
 * be inside the transaction-private stack extent.
 */
static inline int fki_x86_memory_validate_host_bound_shim(
	const struct fki_x86_context_control_v1 *state,
	const struct fki_x86_host_constraints *host,
	const struct fki_x86_memory_extent *extents,
	unsigned int count)
{
	const struct fki_x86_memory_extent *stack;
	struct fki_x86_cr3 decoded_cr3;
	unsigned int i;
	int err;

	err = fki_x86_context_validate_host_bound_shim(state, host);
	if (err)
		return err;
	if (!extents || !count || count > FKI_X86_MEMORY_MAX_EXTENTS)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		err = fki_x86_memory_validate_extent(&extents[i], host);
		if (err)
			return err;
	}
	err = fki_x86_memory_validate_no_overlap(extents, count);
	if (err)
		return err;

	if (!fki_x86_memory_find_linear(extents, count,
					FKI_X86_MEMORY_USE_EXECUTABLE_TEXT,
					state->rip, 1) ||
	    !fki_x86_memory_find_linear(extents, count,
					FKI_X86_MEMORY_USE_XSAVE,
					state->xsave_address,
					state->xsave_bytes) ||
	    !fki_x86_memory_find_linear(extents, count,
					FKI_X86_MEMORY_USE_DESCRIPTOR_TABLE,
					state->gdt_base,
					(fki_u64)state->gdt_limit + 1) ||
	    !fki_x86_memory_find_linear(extents, count,
					FKI_X86_MEMORY_USE_DESCRIPTOR_TABLE,
					state->idt_base,
					(fki_u64)state->idt_limit + 1))
		return -ENOENT;

	stack = fki_x86_memory_find_linear(extents, count,
					   FKI_X86_MEMORY_USE_STACK,
					   state->rsp, 1);
	if (!stack)
		return -ENOENT;
	if (state->rsp - stack->linear_start < 8)
		return -ERANGE;

	err = fki_x86_decode_cr3(state->cr3, host->physical_bits,
				 state->cr4 & FKI_X86_CR4_PCIDE,
				 host->flags & FKI_X86_HOST_CPU_HAS_PCID,
				 &decoded_cr3);
	if (err)
		return err;
	if (!fki_x86_memory_find_physical(extents, count,
					  FKI_X86_MEMORY_USE_PAGE_TABLES,
					  decoded_cr3.root_pa,
					  FKI_X86_CR3_PAGE_SIZE))
		return -ENOENT;
	return 0;
}
