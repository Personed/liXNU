// SPDX-License-Identifier: GPL-2.0-only
#include "fki_x86_64_memory.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HIGH_CANONICAL UINT64_C(0xffff888000000000)
#define VALIDATE_MEMORY fki_x86_memory_validate_host_bound_shim
#define EXTENT_LINEAR_PHYSICAL \
	(FKI_X86_MEMORY_EXTENT_LINEAR | FKI_X86_MEMORY_EXTENT_PHYSICAL)
#define EXTENT_COUNT 6U

enum extent_index {
	EXTENT_TEXT,
	EXTENT_GDT,
	EXTENT_IDT,
	EXTENT_XSAVE,
	EXTENT_STACK,
	EXTENT_PAGE_TABLES,
};

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
	return (struct fki_x86_context_control_v1) {
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
}

static void valid_extents(struct fki_x86_memory_extent extents[EXTENT_COUNT])
{
	memset(extents, 0, sizeof(*extents) * EXTENT_COUNT);
	extents[EXTENT_TEXT] = (struct fki_x86_memory_extent) {
		.linear_start = HIGH_CANONICAL + 0x1000,
		.physical_start = 0x400000,
		.bytes = 0x1000,
		.flags = EXTENT_LINEAR_PHYSICAL,
		.owner_class = FKI_X86_MEMORY_OWNER_HOST,
		.use_class = FKI_X86_MEMORY_USE_EXECUTABLE_TEXT,
		.access = FKI_X86_MEMORY_ACCESS_READ |
			  FKI_X86_MEMORY_ACCESS_EXECUTE,
	};
	extents[EXTENT_GDT] = (struct fki_x86_memory_extent) {
		.linear_start = HIGH_CANONICAL + 0x50000,
		.physical_start = 0x500000,
		.bytes = 0x100,
		.flags = EXTENT_LINEAR_PHYSICAL,
		.owner_class = FKI_X86_MEMORY_OWNER_HOST,
		.use_class = FKI_X86_MEMORY_USE_DESCRIPTOR_TABLE,
		.access = FKI_X86_MEMORY_ACCESS_READ,
	};
	extents[EXTENT_IDT] = (struct fki_x86_memory_extent) {
		.linear_start = HIGH_CANONICAL + 0x51000,
		.physical_start = 0x501000,
		.bytes = 0x1000,
		.flags = EXTENT_LINEAR_PHYSICAL,
		.owner_class = FKI_X86_MEMORY_OWNER_HOST,
		.use_class = FKI_X86_MEMORY_USE_DESCRIPTOR_TABLE,
		.access = FKI_X86_MEMORY_ACCESS_READ,
	};
	extents[EXTENT_XSAVE] = (struct fki_x86_memory_extent) {
		.linear_start = HIGH_CANONICAL + 0x100000,
		.physical_start = 0x600000,
		.bytes = 0x1000,
		.flags = EXTENT_LINEAR_PHYSICAL,
		.owner_class = FKI_X86_MEMORY_OWNER_TRANSACTION_PRIVATE,
		.use_class = FKI_X86_MEMORY_USE_XSAVE,
		.access = FKI_X86_MEMORY_ACCESS_READ |
			  FKI_X86_MEMORY_ACCESS_WRITE,
	};
	extents[EXTENT_STACK] = (struct fki_x86_memory_extent) {
		.linear_start = HIGH_CANONICAL + 0x7000,
		.physical_start = 0x700000,
		.bytes = 0x2000,
		.flags = EXTENT_LINEAR_PHYSICAL,
		.owner_class = FKI_X86_MEMORY_OWNER_TRANSACTION_PRIVATE,
		.use_class = FKI_X86_MEMORY_USE_STACK,
		.access = FKI_X86_MEMORY_ACCESS_READ |
			  FKI_X86_MEMORY_ACCESS_WRITE,
	};
	extents[EXTENT_PAGE_TABLES] = (struct fki_x86_memory_extent) {
		.physical_start = 0x200000,
		.bytes = 0x1000,
		.flags = FKI_X86_MEMORY_EXTENT_PHYSICAL,
		.owner_class = FKI_X86_MEMORY_OWNER_HOST,
		.use_class = FKI_X86_MEMORY_USE_PAGE_TABLES,
		.access = FKI_X86_MEMORY_ACCESS_READ |
			  FKI_X86_MEMORY_ACCESS_WRITE,
	};
}

static void copy_valid(struct fki_x86_context_control_v1 *state,
		       struct fki_x86_host_constraints *host,
		       struct fki_x86_memory_extent extents[EXTENT_COUNT])
{
	*state = valid_state();
	*host = valid_host();
	valid_extents(extents);
}

static void test_valid_baseline(void)
{
	struct fki_x86_context_control_v1 state;
	struct fki_x86_host_constraints host;
	struct fki_x86_memory_extent extents[EXTENT_COUNT];

	copy_valid(&state, &host, extents);
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == 0);
	state.operation = FKI_X86_CONTEXT_OP_RESUME;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == 0);
	assert(sizeof(struct fki_x86_memory_extent) == 48);
}

static void test_shape_rejections(void)
{
	struct fki_x86_context_control_v1 state;
	struct fki_x86_host_constraints host;
	struct fki_x86_memory_extent extents[EXTENT_COUNT];

	copy_valid(&state, &host, extents);
	assert(VALIDATE_MEMORY(&state, &host, NULL, EXTENT_COUNT) == -EINVAL);
	assert(VALIDATE_MEMORY(&state, &host, extents, 0) == -EINVAL);
	assert(VALIDATE_MEMORY(&state, &host, extents,
			       FKI_X86_MEMORY_MAX_EXTENTS + 1) == -EINVAL);

	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].bytes = 0;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].flags &= ~FKI_X86_MEMORY_EXTENT_PHYSICAL;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].flags |= 1U << 8;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].reserved[1] = 1;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].owner_class = 0xff;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].use_class = 0xff;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_PAGE_TABLES].linear_start = HIGH_CANONICAL + 0x200000;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].physical_start++;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].access = 0;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
}

static void test_policy_rejections(void)
{
	struct fki_x86_context_control_v1 state;
	struct fki_x86_host_constraints host;
	struct fki_x86_memory_extent extents[EXTENT_COUNT];

	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].owner_class = FKI_X86_MEMORY_OWNER_FOREIGN_OWNED;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EOPNOTSUPP);
	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].owner_class = FKI_X86_MEMORY_OWNER_SHARED;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EOPNOTSUPP);

	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].owner_class = FKI_X86_MEMORY_OWNER_HOST;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].access = FKI_X86_MEMORY_ACCESS_READ;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_TEXT].access |= FKI_X86_MEMORY_ACCESS_WRITE;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_TEXT].owner_class = FKI_X86_MEMORY_OWNER_TRANSACTION_PRIVATE;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_GDT].access |= FKI_X86_MEMORY_ACCESS_WRITE;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
	copy_valid(&state, &host, extents);
	extents[EXTENT_PAGE_TABLES].owner_class = FKI_X86_MEMORY_OWNER_TRANSACTION_PRIVATE;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EINVAL);
}

static void test_range_rejections(void)
{
	struct fki_x86_context_control_v1 state;
	struct fki_x86_host_constraints host;
	struct fki_x86_memory_extent extents[EXTENT_COUNT];

	copy_valid(&state, &host, extents);
	extents[EXTENT_TEXT].linear_start = 0x0000800000000000ULL;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -ERANGE);
	copy_valid(&state, &host, extents);
	extents[EXTENT_TEXT].physical_start = 1ULL << 48;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -ERANGE);
	copy_valid(&state, &host, extents);
	extents[EXTENT_TEXT].linear_start += 0x800;
	extents[EXTENT_TEXT].physical_start = (1ULL << 48) - 0x800;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -ERANGE);
}

static void test_overlap_and_alias_rejections(void)
{
	struct fki_x86_context_control_v1 state;
	struct fki_x86_host_constraints host;
	struct fki_x86_memory_extent extents[EXTENT_COUNT];

	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].linear_start = HIGH_CANONICAL + 0x7100;
	extents[EXTENT_XSAVE].physical_start = 0x600100;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EEXIST);

	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].physical_start = 0x701000;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EEXIST);

	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].physical_start = 0x700000;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EEXIST);

	copy_valid(&state, &host, extents);
	extents[EXTENT_PAGE_TABLES].physical_start = 0x600000;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EEXIST);

	copy_valid(&state, &host, extents);
	extents[EXTENT_IDT].linear_start = extents[EXTENT_GDT].linear_start + 0x100;
	extents[EXTENT_IDT].physical_start += 0x100;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EEXIST);

	copy_valid(&state, &host, extents);
	extents[EXTENT_IDT].linear_start += 0x100;
	extents[EXTENT_IDT].physical_start = extents[EXTENT_GDT].physical_start + 0x100;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -EEXIST);
}

static void test_reference_rejections(void)
{
	struct fki_x86_context_control_v1 state;
	struct fki_x86_host_constraints host;
	struct fki_x86_memory_extent extents[EXTENT_COUNT];

	copy_valid(&state, &host, extents);
	extents[EXTENT_TEXT].linear_start += 0x2000;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -ENOENT);
	copy_valid(&state, &host, extents);
	extents[EXTENT_XSAVE].bytes = 512;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -ENOENT);
	copy_valid(&state, &host, extents);
	extents[EXTENT_GDT].linear_start += 0x2000;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -ENOENT);
	copy_valid(&state, &host, extents);
	extents[EXTENT_IDT].linear_start += 0x2000;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -ENOENT);
	copy_valid(&state, &host, extents);
	extents[EXTENT_STACK].linear_start += 0x2000;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -ENOENT);
	copy_valid(&state, &host, extents);
	extents[EXTENT_PAGE_TABLES].physical_start = 0x300000;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -ENOENT);
}

static void test_stack_precall_slot(void)
{
	struct fki_x86_context_control_v1 state;
	struct fki_x86_host_constraints host;
	struct fki_x86_memory_extent extents[EXTENT_COUNT];

	copy_valid(&state, &host, extents);
	extents[EXTENT_STACK].linear_start = state.rsp - 8;
	extents[EXTENT_STACK].physical_start += 0xff8;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == 0);

	copy_valid(&state, &host, extents);
	extents[EXTENT_STACK].linear_start = state.rsp - 7;
	extents[EXTENT_STACK].physical_start += 0xff9;
	assert(VALIDATE_MEMORY(&state, &host, extents, EXTENT_COUNT) == -ERANGE);
}

int main(void)
{
	test_valid_baseline();
	test_shape_rejections();
	test_policy_rejections();
	test_range_rejections();
	test_overlap_and_alias_rejections();
	test_reference_rejections();
	test_stack_precall_slot();
	puts("FKI x86 memory ownership fixtures passed");
	return 0;
}
