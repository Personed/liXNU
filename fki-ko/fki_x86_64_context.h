/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include "fki_x86_64_cr3.h"

#ifdef __KERNEL__
#include <linux/errno.h>
typedef u16 fki_u16;
typedef u32 fki_u32;
#else
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint16_t fki_u16;
typedef uint32_t fki_u32;
#endif

/*
 * Version-1 x86_64 execution-context control block.
 *
 * This is a validation-only host/foreign boundary description. It does not
 * create a transaction, enter foreign code, or expose an FKI capability.
 * Version 1 validates only HOST_BOUND_SHIM/ENTER input shaped like a
 * Linux long-mode host context; XNU profiles and resume transactions are
 * rejected until their ownership semantics are implemented.
 */
#define FKI_X86_CONTEXT_ABI_VERSION 1U
#define FKI_X86_CONTEXT_CONTROL_VERSION 1U
#define FKI_X86_CONTEXT_V1_BYTES 512U

#define FKI_X86_CONTEXT_PROFILE_HOST_BOUND_SHIM 1U
#define FKI_X86_CONTEXT_PROFILE_XNU_PREBOOT_LONG 2U
#define FKI_X86_CONTEXT_PROFILE_XNU_COLD_BOOT 3U

#define FKI_X86_CONTEXT_OP_ENTER 1U
#define FKI_X86_CONTEXT_OP_RESUME 2U

#define FKI_X86_XSAVE_FORMAT_STANDARD 1ULL

#define FKI_X86_HOST_CPU_HAS_PCID (1U << 0)
#define FKI_X86_HOST_LAM_ACTIVE (1U << 1)
#define FKI_X86_HOST_MEMORY_ENCRYPTION_ACTIVE (1U << 2)
#define FKI_X86_HOST_FLAGS_MASK \
	(FKI_X86_HOST_CPU_HAS_PCID | FKI_X86_HOST_LAM_ACTIVE | \
	 FKI_X86_HOST_MEMORY_ENCRYPTION_ACTIVE)

#define FKI_X86_CR0_PE (1ULL << 0)
#define FKI_X86_CR0_MP (1ULL << 1)
#define FKI_X86_CR0_EM (1ULL << 2)
#define FKI_X86_CR0_TS (1ULL << 3)
#define FKI_X86_CR0_ET (1ULL << 4)
#define FKI_X86_CR0_NE (1ULL << 5)
#define FKI_X86_CR0_WP (1ULL << 16)
#define FKI_X86_CR0_AM (1ULL << 18)
#define FKI_X86_CR0_NW (1ULL << 29)
#define FKI_X86_CR0_CD (1ULL << 30)
#define FKI_X86_CR0_PG (1ULL << 31)
#define FKI_X86_CR0_VALID_MASK \
	(FKI_X86_CR0_PE | FKI_X86_CR0_MP | FKI_X86_CR0_EM | \
	 FKI_X86_CR0_TS | FKI_X86_CR0_ET | FKI_X86_CR0_NE | \
	 FKI_X86_CR0_WP | FKI_X86_CR0_AM | FKI_X86_CR0_NW | \
	 FKI_X86_CR0_CD | FKI_X86_CR0_PG)

#define FKI_X86_CR4_PAE (1ULL << 5)
#define FKI_X86_CR4_OSFXSR (1ULL << 9)
#define FKI_X86_CR4_OSXMMEXCPT (1ULL << 10)
#define FKI_X86_CR4_LA57 (1ULL << 12)
#define FKI_X86_CR4_PCIDE (1ULL << 17)
#define FKI_X86_CR4_OSXSAVE (1ULL << 18)

#define FKI_X86_EFER_SCE (1ULL << 0)
#define FKI_X86_EFER_LME (1ULL << 8)
#define FKI_X86_EFER_LMA (1ULL << 10)
#define FKI_X86_EFER_NXE (1ULL << 11)
#define FKI_X86_EFER_SVME (1ULL << 12)
#define FKI_X86_EFER_LMSLE (1ULL << 13)
#define FKI_X86_EFER_FFXSR (1ULL << 14)
#define FKI_X86_EFER_TCE (1ULL << 15)
#define FKI_X86_EFER_VALID_MASK \
	(FKI_X86_EFER_SCE | FKI_X86_EFER_LME | FKI_X86_EFER_LMA | \
	 FKI_X86_EFER_NXE | FKI_X86_EFER_SVME | FKI_X86_EFER_LMSLE | \
	 FKI_X86_EFER_FFXSR | FKI_X86_EFER_TCE)

#define FKI_X86_XCR0_X87 (1ULL << 0)
#define FKI_X86_XCR0_SSE (1ULL << 1)
#define FKI_X86_XCR0_AVX (1ULL << 2)
#define FKI_X86_XCR0_V1_MASK \
	(FKI_X86_XCR0_X87 | FKI_X86_XCR0_SSE | FKI_X86_XCR0_AVX)

#define FKI_X86_RFLAGS_FIXED (1ULL << 1)
#define FKI_X86_RFLAGS_CF (1ULL << 0)
#define FKI_X86_RFLAGS_PF (1ULL << 2)
#define FKI_X86_RFLAGS_AF (1ULL << 4)
#define FKI_X86_RFLAGS_ZF (1ULL << 6)
#define FKI_X86_RFLAGS_SF (1ULL << 7)
#define FKI_X86_RFLAGS_DF (1ULL << 10)
#define FKI_X86_RFLAGS_OF (1ULL << 11)
#define FKI_X86_RFLAGS_ID (1ULL << 21)
#define FKI_X86_RFLAGS_TF (1ULL << 8)
#define FKI_X86_RFLAGS_IF (1ULL << 9)
#define FKI_X86_RFLAGS_IOPL (3ULL << 12)
#define FKI_X86_RFLAGS_NT (1ULL << 14)
#define FKI_X86_RFLAGS_RF (1ULL << 16)
#define FKI_X86_RFLAGS_VM (1ULL << 17)
#define FKI_X86_RFLAGS_AC (1ULL << 18)
#define FKI_X86_RFLAGS_VIF (1ULL << 19)
#define FKI_X86_RFLAGS_VIP (1ULL << 20)
#define FKI_X86_RFLAGS_V1_ALLOWED \
	(FKI_X86_RFLAGS_FIXED | FKI_X86_RFLAGS_CF | FKI_X86_RFLAGS_PF | \
	 FKI_X86_RFLAGS_AF | FKI_X86_RFLAGS_ZF | FKI_X86_RFLAGS_SF | \
	 FKI_X86_RFLAGS_OF | FKI_X86_RFLAGS_ID)
#define FKI_X86_RFLAGS_V1_REJECTED \
	(FKI_X86_RFLAGS_TF | FKI_X86_RFLAGS_IF | FKI_X86_RFLAGS_DF | \
	 FKI_X86_RFLAGS_IOPL | FKI_X86_RFLAGS_NT | FKI_X86_RFLAGS_RF | \
	 FKI_X86_RFLAGS_VM | FKI_X86_RFLAGS_AC | FKI_X86_RFLAGS_VIF | \
	 FKI_X86_RFLAGS_VIP)

#define FKI_X86_XSAVE_MIN_BYTES 576ULL
#define FKI_X86_XSAVE_AVX_MIN_BYTES 832ULL
#define FKI_X86_XSAVE_ALIGNMENT 64ULL

struct fki_x86_host_constraints {
	fki_u64 cr4_allowed;
	fki_u64 efer_allowed;
	fki_u64 xcr0_supported;
	fki_u64 xsave_max_bytes;
	/* First byte after the AVX component, as enumerated by the host. */
	fki_u64 xsave_avx_end;
	fki_u32 physical_bits;
	fki_u32 flags;
};

struct fki_x86_context_control_v1 {
	fki_u32 abi_version;
	fki_u32 control_version;
	fki_u32 state_size;
	fki_u32 profile;
	fki_u32 operation;
	fki_u32 flags;
	fki_u64 stop_mask;
	fki_u64 feature_bits;

	fki_u64 rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
	fki_u64 r8, r9, r10, r11, r12, r13, r14, r15, rip;
	fki_u64 rflags;

	fki_u16 cs, ss, ds, es, fs, gs, tr, ldtr;
	fki_u64 fs_base, gs_base, kernel_gs_base;

	fki_u64 cr0, cr3, cr4, efer, xcr0;
	fki_u64 gdt_base, idt_base;
	fki_u16 gdt_limit, idt_limit;
	fki_u32 descriptor_reserved;

	fki_u64 xsave_address;
	fki_u64 xsave_bytes;
	fki_u64 xsave_xstate_bv;
	fki_u64 xsave_format;

	fki_u64 boot_address;
	fki_u64 boot_bytes;
	fki_u64 boot_identity;

	fki_u64 stop_reason;
	fki_u64 stop_vector;
	fki_u64 stop_error_code;
	fki_u64 stop_cr2;
	fki_u64 stop_rip;
	fki_u64 stop_rsp;
	fki_u64 stop_aux0;
	fki_u64 stop_aux1;

	fki_u64 reserved[13];
};

_Static_assert(sizeof(struct fki_x86_context_control_v1) == FKI_X86_CONTEXT_V1_BYTES,
	       "x86 context control block size");
_Static_assert(__builtin_offsetof(struct fki_x86_context_control_v1, rax) == 40,
	       "x86 context GPR offset");
_Static_assert(__builtin_offsetof(struct fki_x86_context_control_v1, cs) == 184,
	       "x86 context selector offset");
_Static_assert(__builtin_offsetof(struct fki_x86_context_control_v1, cr0) == 224,
	       "x86 context control-register offset");
_Static_assert(__builtin_offsetof(struct fki_x86_context_control_v1, xsave_address) == 288,
	       "x86 context XSAVE offset");
_Static_assert(__builtin_offsetof(struct fki_x86_context_control_v1, stop_reason) == 344,
	       "x86 context stop-report offset");
_Static_assert(__builtin_offsetof(struct fki_x86_context_control_v1, reserved) == 408,
	       "x86 context reserved offset");

static inline bool fki_x86_canonical48(fki_u64 address)
{
	fki_u64 top = address >> 47;

	return top == 0 || top == 0x1ffff;
}

static inline bool fki_x86_canonical_range48(fki_u64 address, fki_u64 bytes)
{
	fki_u64 top = address >> 47;

	return bytes && bytes - 1 <= ~(fki_u64)0 - address &&
	       (top == 0 || top == 0x1ffff) &&
	       (address + bytes - 1) >> 47 == top;
}

static inline bool fki_x86_zeroed(const fki_u64 *values, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (values[i])
			return false;
	return true;
}

static inline int fki_x86_validate_rflags_v1(fki_u64 rflags)
{
	if (!(rflags & FKI_X86_RFLAGS_FIXED) ||
	    (rflags & ~(fki_u64)FKI_X86_RFLAGS_V1_ALLOWED))
		return -EINVAL;
	if (rflags & FKI_X86_RFLAGS_V1_REJECTED)
		return -EINVAL;
	return 0;
}

static inline int fki_x86_validate_cr0_v1(fki_u64 cr0)
{
	if (cr0 & ~(fki_u64)FKI_X86_CR0_VALID_MASK)
		return -EINVAL;
	if ((cr0 & (FKI_X86_CR0_PE | FKI_X86_CR0_ET |
		    FKI_X86_CR0_NE | FKI_X86_CR0_PG)) !=
	    (FKI_X86_CR0_PE | FKI_X86_CR0_ET |
	     FKI_X86_CR0_NE | FKI_X86_CR0_PG))
		return -EINVAL;
	return 0;
}

static inline int fki_x86_validate_cr4_v1(fki_u64 cr4,
					  const struct fki_x86_host_constraints *host)
{
	if (cr4 & ~host->cr4_allowed)
		return -EINVAL;
	if ((cr4 & (FKI_X86_CR4_PAE | FKI_X86_CR4_OSFXSR |
		    FKI_X86_CR4_OSXMMEXCPT | FKI_X86_CR4_OSXSAVE)) !=
	    (FKI_X86_CR4_PAE | FKI_X86_CR4_OSFXSR |
	     FKI_X86_CR4_OSXMMEXCPT | FKI_X86_CR4_OSXSAVE))
		return -EINVAL;
	if (cr4 & FKI_X86_CR4_LA57)
		return -EOPNOTSUPP;
	if ((cr4 & FKI_X86_CR4_PCIDE) &&
	    !(host->flags & FKI_X86_HOST_CPU_HAS_PCID))
		return -EOPNOTSUPP;
	return 0;
}

static inline int fki_x86_validate_efer_v1(fki_u64 efer,
					   const struct fki_x86_host_constraints *host)
{
	if (efer & ~(fki_u64)FKI_X86_EFER_VALID_MASK)
		return -EINVAL;
	if (efer & ~host->efer_allowed)
		return -EINVAL;
	if ((efer & (FKI_X86_EFER_LME | FKI_X86_EFER_LMA)) !=
	    (FKI_X86_EFER_LME | FKI_X86_EFER_LMA))
		return -EINVAL;
	return 0;
}

static inline int fki_x86_validate_xcr0_v1(fki_u64 xcr0,
					   const struct fki_x86_host_constraints *host)
{
	if (!(xcr0 & FKI_X86_XCR0_X87) ||
	    (xcr0 & ~(fki_u64)FKI_X86_XCR0_V1_MASK))
		return -EINVAL;
	if (xcr0 & ~host->xcr0_supported)
		return -EINVAL;
	if ((xcr0 & FKI_X86_XCR0_AVX) && !(xcr0 & FKI_X86_XCR0_SSE))
		return -EINVAL;
	return 0;
}

static inline bool fki_x86_selector_ring0_gdt(fki_u16 selector)
{
	return !(selector & 0x7);
}

static inline int fki_x86_validate_tables_v1(
	const struct fki_x86_context_control_v1 *state)
{
	fki_u16 selectors[] = {
		state->cs, state->ss, state->ds, state->es,
		state->fs, state->gs, state->tr,
	};
	unsigned int i;

	if (!state->cs || !state->ss || !state->tr || state->ldtr ||
	    state->descriptor_reserved)
		return -EINVAL;
	for (i = 0; i < sizeof(selectors) / sizeof(selectors[0]); i++) {
		/* TR is seventh here; its long-mode system descriptor is 16 bytes. */
		fki_u64 descriptor_bytes = i == 6 ? 16 : 8;

		if (!fki_x86_selector_ring0_gdt(selectors[i]))
			return -EINVAL;
		if (selectors[i] &&
		    selectors[i] + descriptor_bytes - 1 > state->gdt_limit)
			return -ERANGE;
	}
	if (state->idt_limit < 15 ||
	    !fki_x86_canonical_range48(state->gdt_base,
				       (fki_u64)state->gdt_limit + 1) ||
	    !fki_x86_canonical_range48(state->idt_base,
				       (fki_u64)state->idt_limit + 1))
		return -ERANGE;
	return 0;
}

static inline int fki_x86_validate_xsave_v1(
	const struct fki_x86_context_control_v1 *state,
	const struct fki_x86_host_constraints *host)
{
	if (state->xsave_format != FKI_X86_XSAVE_FORMAT_STANDARD)
		return -EOPNOTSUPP;
	if (!host->xsave_max_bytes || host->xsave_max_bytes < FKI_X86_XSAVE_MIN_BYTES)
		return -EINVAL;
	if (host->xcr0_supported & FKI_X86_XCR0_AVX) {
		if (host->xsave_avx_end < FKI_X86_XSAVE_AVX_MIN_BYTES ||
		    host->xsave_avx_end > host->xsave_max_bytes)
			return -EINVAL;
	} else if (host->xsave_avx_end) {
		return -EINVAL;
	}
	if (!state->xsave_address ||
	    (state->xsave_address & (FKI_X86_XSAVE_ALIGNMENT - 1)) ||
	    !fki_x86_canonical_range48(state->xsave_address, state->xsave_bytes))
		return -ERANGE;
	if (state->xsave_bytes < ((state->xcr0 & FKI_X86_XCR0_AVX) ?
				    host->xsave_avx_end : FKI_X86_XSAVE_MIN_BYTES) ||
	    state->xsave_bytes > host->xsave_max_bytes)
		return -EINVAL;
	if (state->xsave_xstate_bv & ~state->xcr0)
		return -EINVAL;
	return 0;
}

/*
 * Validate the fixed shape and architectural invariants of a HOST_BOUND_SHIM
 * ENTER control block. Ownership of CR3, XSAVE memory, GDT/IDT memory, stack,
 * and executable text is deliberately outside this helper; a transaction
 * implementation must validate those references separately before arming.
 */
static inline int fki_x86_context_validate_host_bound_shim(
	const struct fki_x86_context_control_v1 *state,
	const struct fki_x86_host_constraints *host)
{
	struct fki_x86_cr3 decoded_cr3;
	int err;

	if (!state || !host || (unsigned long)state & 7)
		return -EINVAL;
	if (state->abi_version != FKI_X86_CONTEXT_ABI_VERSION ||
	    state->control_version != FKI_X86_CONTEXT_CONTROL_VERSION)
		return -EOPNOTSUPP;
	if (state->state_size != FKI_X86_CONTEXT_V1_BYTES)
		return -EINVAL;
	if (state->profile != FKI_X86_CONTEXT_PROFILE_HOST_BOUND_SHIM ||
	    state->operation != FKI_X86_CONTEXT_OP_ENTER)
		return -EOPNOTSUPP;
	if (state->flags || state->stop_mask || state->feature_bits ||
	    state->boot_address || state->boot_bytes || state->boot_identity ||
	    state->stop_reason || state->stop_vector || state->stop_error_code ||
	    state->stop_cr2 || state->stop_rip || state->stop_rsp ||
	    state->stop_aux0 || state->stop_aux1 ||
	    !fki_x86_zeroed(state->reserved,
			    sizeof(state->reserved) / sizeof(state->reserved[0])))
		return -EINVAL;
	if (!host->physical_bits || host->physical_bits < 32 ||
	    host->physical_bits > 52 || (host->flags & ~FKI_X86_HOST_FLAGS_MASK))
		return -EINVAL;
	if (host->flags & (FKI_X86_HOST_LAM_ACTIVE |
			   FKI_X86_HOST_MEMORY_ENCRYPTION_ACTIVE))
		return -EOPNOTSUPP;

	err = fki_x86_validate_rflags_v1(state->rflags);
	if (err)
		return err;
	err = fki_x86_validate_cr0_v1(state->cr0);
	if (err)
		return err;
	err = fki_x86_validate_cr4_v1(state->cr4, host);
	if (err)
		return err;
	err = fki_x86_validate_efer_v1(state->efer, host);
	if (err)
		return err;
	err = fki_x86_validate_xcr0_v1(state->xcr0, host);
	if (err)
		return err;
	err = fki_x86_decode_cr3(state->cr3, host->physical_bits,
				 state->cr4 & FKI_X86_CR4_PCIDE,
				 host->flags & FKI_X86_HOST_CPU_HAS_PCID,
				 &decoded_cr3);
	if (err)
		return err;
	if (!fki_x86_canonical48(state->rip) ||
	    !fki_x86_canonical48(state->rsp) ||
	    !state->rsp || (state->rsp & 0xf) ||
	    !fki_x86_canonical48(state->fs_base) ||
	    !fki_x86_canonical48(state->gs_base) ||
	    !fki_x86_canonical48(state->kernel_gs_base))
		return -ERANGE;
	err = fki_x86_validate_tables_v1(state);
	if (err)
		return err;
	return fki_x86_validate_xsave_v1(state, host);
}
