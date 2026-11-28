// SPDX-License-Identifier: GPL-2.0-only
#include "fki_x86_64_context.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HIGH_CANONICAL UINT64_C(0xffff888000000000)
#define VALIDATE fki_x86_context_validate_host_bound_shim

static struct fki_x86_host_constraints valid_host(void)
{
	return (struct fki_x86_host_constraints) {
		.cr4_allowed = FKI_X86_CR4_PAE | FKI_X86_CR4_OSFXSR |
				 FKI_X86_CR4_OSXMMEXCPT | FKI_X86_CR4_OSXSAVE,
		.efer_allowed = FKI_X86_EFER_SCE | FKI_X86_EFER_LME |
				FKI_X86_EFER_LMA | FKI_X86_EFER_NXE,
		.xcr0_supported = FKI_X86_XCR0_X87 | FKI_X86_XCR0_SSE |
				  FKI_X86_XCR0_AVX,
		.xsave_max_bytes = 4096,
		.xsave_avx_end = FKI_X86_XSAVE_AVX_MIN_BYTES,
		.physical_bits = 48,
	};
}

static struct fki_x86_context_control_v1 valid_state(void)
{
	struct fki_x86_context_control_v1 state = {
		.abi_version = FKI_X86_CONTEXT_ABI_VERSION,
		.control_version = FKI_X86_CONTEXT_CONTROL_VERSION,
		.state_size = FKI_X86_CONTEXT_V1_BYTES,
		.profile = FKI_X86_CONTEXT_PROFILE_HOST_BOUND_SHIM,
		.operation = FKI_X86_CONTEXT_OP_ENTER,
		.rsp = HIGH_CANONICAL + 0x8000,
		.rip = HIGH_CANONICAL + 0x1000,
		.rflags = FKI_X86_RFLAGS_FIXED | FKI_X86_RFLAGS_ID,
		.cs = 0x10,
		.ss = 0x18,
		.tr = 0x40,
		.fs_base = HIGH_CANONICAL + 0x20000,
		.gs_base = HIGH_CANONICAL + 0x30000,
		.kernel_gs_base = HIGH_CANONICAL + 0x40000,
		.cr0 = FKI_X86_CR0_PE | FKI_X86_CR0_ET | FKI_X86_CR0_NE |
		       FKI_X86_CR0_WP | FKI_X86_CR0_PG,
		.cr3 = 0x200000,
		.cr4 = FKI_X86_CR4_PAE | FKI_X86_CR4_OSFXSR |
		       FKI_X86_CR4_OSXMMEXCPT | FKI_X86_CR4_OSXSAVE,
		.efer = FKI_X86_EFER_SCE | FKI_X86_EFER_LME |
			FKI_X86_EFER_LMA | FKI_X86_EFER_NXE,
		.xcr0 = FKI_X86_XCR0_X87 | FKI_X86_XCR0_SSE | FKI_X86_XCR0_AVX,
		.gdt_base = HIGH_CANONICAL + 0x50000,
		.idt_base = HIGH_CANONICAL + 0x51000,
		.gdt_limit = 0xff,
		.idt_limit = 0xfff,
		.xsave_address = HIGH_CANONICAL + 0x100000,
		.xsave_bytes = 1024,
		.xsave_xstate_bv = FKI_X86_XCR0_X87 | FKI_X86_XCR0_SSE,
		.xsave_format = FKI_X86_XSAVE_FORMAT_STANDARD,
	};

	return state;
}

static void test_valid_baseline(void)
{
	struct fki_x86_context_control_v1 state = valid_state();
	struct fki_x86_host_constraints host = valid_host();

	assert(VALIDATE(&state, &host) == 0);
	assert(sizeof(state) == FKI_X86_CONTEXT_V1_BYTES);
}

static void test_metadata_rejections(void)
{
	struct fki_x86_context_control_v1 state = valid_state();
	struct fki_x86_host_constraints host = valid_host();

	assert(VALIDATE(NULL, &host) == -EINVAL);
	assert(VALIDATE(&state, NULL) == -EINVAL);
	{
		unsigned char misaligned[sizeof(state) + 1];

		memcpy(misaligned + 1, &state, sizeof(state));
		assert(VALIDATE((struct fki_x86_context_control_v1 *)(misaligned + 1),
				&host) == -EINVAL);
	}

	state.abi_version = 0;
	assert(VALIDATE(&state, &host) == -EOPNOTSUPP);
	state = valid_state();
	state.control_version = 2;
	assert(VALIDATE(&state, &host) == -EOPNOTSUPP);
	state = valid_state();
	state.state_size--;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.profile = FKI_X86_CONTEXT_PROFILE_XNU_PREBOOT_LONG;
	assert(VALIDATE(&state, &host) == -EOPNOTSUPP);
	state = valid_state();
	state.profile = FKI_X86_CONTEXT_PROFILE_XNU_COLD_BOOT;
	assert(VALIDATE(&state, &host) == -EOPNOTSUPP);
	state = valid_state();
	state.operation = FKI_X86_CONTEXT_OP_RESUME;
	assert(VALIDATE(&state, &host) == -EOPNOTSUPP);

	state = valid_state();
	state.flags = 1;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.stop_mask = 1;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.feature_bits = 1;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.boot_address = 0x1000;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.stop_reason = 1;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.reserved[12] = 1;
	assert(VALIDATE(&state, &host) == -EINVAL);
}

static void test_rflags_rejections(void)
{
	struct fki_x86_host_constraints host = valid_host();
	struct fki_x86_context_control_v1 state;
	const fki_u64 rejected[] = {
		FKI_X86_RFLAGS_TF, FKI_X86_RFLAGS_IF, FKI_X86_RFLAGS_DF,
		1ULL << 12, FKI_X86_RFLAGS_NT, FKI_X86_RFLAGS_RF, FKI_X86_RFLAGS_VM,
		FKI_X86_RFLAGS_AC, FKI_X86_RFLAGS_VIF, FKI_X86_RFLAGS_VIP,
		1ULL << 22, 1ULL << 63,
	};
	unsigned int i;

	state = valid_state();
	state.rflags &= ~FKI_X86_RFLAGS_FIXED;
	assert(VALIDATE(&state, &host) == -EINVAL);
	for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
		state = valid_state();
		state.rflags |= rejected[i];
		assert(VALIDATE(&state, &host) == -EINVAL);
	}
}

static void test_control_register_rejections(void)
{
	struct fki_x86_host_constraints host = valid_host();
	struct fki_x86_context_control_v1 state;

	state = valid_state();
	state.cr0 &= ~FKI_X86_CR0_PE;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.cr0 &= ~FKI_X86_CR0_PG;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.cr0 &= ~FKI_X86_CR0_NE;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.cr0 |= 1ULL << 32;
	assert(VALIDATE(&state, &host) == -EINVAL);

	state = valid_state();
	state.cr4 = 0;
	assert(VALIDATE(&state, &host) == -EINVAL);
	for (int bit = 0; bit < 4; bit++) {
		const fki_u64 required[] = {
			FKI_X86_CR4_PAE, FKI_X86_CR4_OSFXSR,
			FKI_X86_CR4_OSXMMEXCPT, FKI_X86_CR4_OSXSAVE,
		};

		state = valid_state();
		state.cr4 &= ~required[bit];
		assert(VALIDATE(&state, &host) == -EINVAL);
	}
	state = valid_state();
	state.cr4 |= FKI_X86_CR4_LA57;
	host.cr4_allowed |= FKI_X86_CR4_LA57;
	assert(VALIDATE(&state, &host) == -EOPNOTSUPP);

	host = valid_host();
	state = valid_state();
	state.cr4 |= FKI_X86_CR4_PCIDE;
	assert(VALIDATE(&state, &host) == -EINVAL);
	host.cr4_allowed |= FKI_X86_CR4_PCIDE;
	assert(VALIDATE(&state, &host) == -EOPNOTSUPP);
	host.flags |= FKI_X86_HOST_CPU_HAS_PCID;
	state.cr3 |= 0xabc;
	assert(VALIDATE(&state, &host) == 0);

	state = valid_state();
	host = valid_host();
	state.efer &= ~FKI_X86_EFER_LME;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.efer &= ~FKI_X86_EFER_LMA;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.efer |= 1ULL << 16;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.efer |= FKI_X86_EFER_TCE;
	assert(VALIDATE(&state, &host) == -EINVAL);

	state = valid_state();
	state.xcr0 &= ~FKI_X86_XCR0_X87;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.xcr0 &= ~FKI_X86_XCR0_SSE;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.xcr0 |= 1ULL << 3;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	host.xcr0_supported &= ~FKI_X86_XCR0_AVX;
	assert(VALIDATE(&state, &host) == -EINVAL);
}

static void test_canonical_and_table_rejections(void)
{
	struct fki_x86_host_constraints host = valid_host();
	struct fki_x86_context_control_v1 state;

	assert(fki_x86_canonical48(0x00007fffffffffffULL));
	assert(fki_x86_canonical48(0xffff800000000000ULL));
	assert(!fki_x86_canonical48(0x0000800000000000ULL));
	assert(fki_x86_canonical_range48(0xfffffffffffff000ULL, 0x1000));
	assert(!fki_x86_canonical_range48(0xfffffffffffff000ULL, 0x1001));
	assert(!fki_x86_canonical_range48(0x00007ffffffff000ULL, 0x2000));
	assert(!fki_x86_canonical_range48(0x0000000000001000ULL,
					 0xffff800000000000ULL));

	state = valid_state();
	state.rip = 0x0000800000000000ULL;
	assert(VALIDATE(&state, &host) == -ERANGE);
	state = valid_state();
	state.rsp = 0x0000800000000000ULL;
	assert(VALIDATE(&state, &host) == -ERANGE);
	state = valid_state();
	state.rsp &= ~0xfULL;
	state.rsp += 8;
	assert(VALIDATE(&state, &host) == -ERANGE);
	state = valid_state();
	state.fs_base = 0x0000800000000000ULL;
	assert(VALIDATE(&state, &host) == -ERANGE);

	state = valid_state();
	state.cs |= 1;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.ss |= 4;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.cs = 0;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.ss = 0;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.tr = 0;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.ldtr = 0x20;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.gdt_limit = 0x47;
	assert(VALIDATE(&state, &host) == -ERANGE);
	state = valid_state();
	state.gdt_limit = 0x3f;
	assert(VALIDATE(&state, &host) == -ERANGE);
	state = valid_state();
	state.gdt_base = 0x0000800000000000ULL;
	assert(VALIDATE(&state, &host) == -ERANGE);
	state = valid_state();
	state.gdt_base = 0x00007fffffffff80ULL;
	assert(VALIDATE(&state, &host) == -ERANGE);
	state = valid_state();
	state.idt_limit = 0;
	assert(VALIDATE(&state, &host) == -ERANGE);
}

static void test_xsave_rejections(void)
{
	struct fki_x86_host_constraints host = valid_host();
	struct fki_x86_context_control_v1 state;

	state = valid_state();
	state.xsave_format = 0;
	assert(VALIDATE(&state, &host) == -EOPNOTSUPP);
	state = valid_state();
	state.xsave_format = 2;
	assert(VALIDATE(&state, &host) == -EOPNOTSUPP);
	state = valid_state();
	state.xsave_address += 8;
	assert(VALIDATE(&state, &host) == -ERANGE);
	state = valid_state();
	state.xsave_address = 0x0000800000000000ULL;
	assert(VALIDATE(&state, &host) == -ERANGE);
	state = valid_state();
	state.xsave_bytes = FKI_X86_XSAVE_MIN_BYTES - 1;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.xsave_bytes = FKI_X86_XSAVE_AVX_MIN_BYTES - 1;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.xsave_bytes = FKI_X86_XSAVE_AVX_MIN_BYTES;
	assert(VALIDATE(&state, &host) == 0);
	state = valid_state();
	state.xsave_bytes = 4097;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.xsave_bytes = 4096;
	host.xsave_max_bytes = 2048;
	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_state();
	state.xsave_xstate_bv |= FKI_X86_XCR0_AVX;
	state.xcr0 &= ~FKI_X86_XCR0_AVX;
	assert(VALIDATE(&state, &host) == -EINVAL);
}

static void test_host_constraint_rejections(void)
{
	struct fki_x86_context_control_v1 state = valid_state();
	struct fki_x86_host_constraints host = valid_host();

	host.physical_bits = 31;
	assert(VALIDATE(&state, &host) == -EINVAL);
	host = valid_host();
	host.physical_bits = 53;
	assert(VALIDATE(&state, &host) == -EINVAL);
	host = valid_host();
	host.flags |= 1U << 3;
	assert(VALIDATE(&state, &host) == -EINVAL);
	host = valid_host();
	host.flags |= FKI_X86_HOST_LAM_ACTIVE;
	assert(VALIDATE(&state, &host) == -EOPNOTSUPP);
	host = valid_host();
	host.flags |= FKI_X86_HOST_MEMORY_ENCRYPTION_ACTIVE;
	assert(VALIDATE(&state, &host) == -EOPNOTSUPP);
	host = valid_host();
	host.xsave_avx_end = FKI_X86_XSAVE_MIN_BYTES;
	assert(VALIDATE(&state, &host) == -EINVAL);
	host = valid_host();
	host.xsave_avx_end = 4097;
	assert(VALIDATE(&state, &host) == -EINVAL);
	host = valid_host();
	host.xsave_max_bytes = 0;
	assert(VALIDATE(&state, &host) == -EINVAL);
}

int main(void)
{
	test_valid_baseline();
	test_metadata_rejections();
	test_rflags_rejections();
	test_control_register_rejections();
	test_canonical_and_table_rejections();
	test_xsave_rejections();
	test_host_constraint_rejections();
	puts("FKI x86 context fixtures passed");
	return 0;
}
