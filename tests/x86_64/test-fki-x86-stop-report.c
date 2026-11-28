// SPDX-License-Identifier: GPL-2.0-only
#include "fki_x86_64_context.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HIGH_CANONICAL UINT64_C(0xffff888000000000)
#define NONCANONICAL UINT64_C(0x0000800000000000)
#define VALIDATE fki_x86_context_validate_host_bound_shim
#define VALIDATE_STOP fki_x86_validate_stop_report_v1

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

static struct fki_x86_context_control_v1 valid_entry(void)
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

static struct fki_x86_context_control_v1 valid_report(unsigned int reason)
{
	struct fki_x86_context_control_v1 state = valid_entry();

	state.stop_reason = reason;
	state.stop_rip = HIGH_CANONICAL + 0x23000;
	state.stop_rsp = HIGH_CANONICAL + 0x24000;
	return state;
}

static void test_valid_reports(void)
{
	struct fki_x86_context_control_v1 state;

	state = valid_report(FKI_X86_STOP_NORMAL_RETURN);
	assert(VALIDATE_STOP(&state) == 0);
	state.operation = FKI_X86_CONTEXT_OP_RESUME;
	assert(VALIDATE_STOP(&state) == 0);
	state = valid_report(FKI_X86_STOP_SOFTWARE_ABORT);
	assert(VALIDATE_STOP(&state) == 0);
	state.operation = FKI_X86_CONTEXT_OP_RESUME;
	assert(VALIDATE_STOP(&state) == 0);

	state = valid_report(FKI_X86_STOP_PAGE_FAULT);
	state.stop_vector = FKI_X86_STOP_VECTOR_PAGE_FAULT;
	state.stop_error_code = 0x7f | 0x8000;
	state.stop_cr2 = HIGH_CANONICAL + 0x30000;
	assert(VALIDATE_STOP(&state) == 0);
	state.operation = FKI_X86_CONTEXT_OP_RESUME;
	assert(VALIDATE_STOP(&state) == 0);
	state.operation = FKI_X86_CONTEXT_OP_ENTER;
	state.stop_error_code = 0;
	state.stop_cr2 = 0;
	assert(VALIDATE_STOP(&state) == 0);

	state = valid_report(FKI_X86_STOP_GENERAL_PROTECTION);
	state.stop_vector = FKI_X86_STOP_VECTOR_GENERAL_PROTECTION;
	state.stop_error_code = 0xfff8;
	assert(VALIDATE_STOP(&state) == 0);
	state.operation = FKI_X86_CONTEXT_OP_RESUME;
	assert(VALIDATE_STOP(&state) == 0);

	state = valid_report(FKI_X86_STOP_SHUTDOWN);
	state.stop_rip = 0;
	state.stop_rsp = 0;
	state.stop_aux0 = FKI_X86_STOP_SHUTDOWN_HALT;
	assert(VALIDATE_STOP(&state) == 0);
	state.stop_aux1 = FKI_X86_STOP_SHUTDOWN_RESTART_REQUESTED;
	assert(VALIDATE_STOP(&state) == 0);
	state.operation = FKI_X86_CONTEXT_OP_RESUME;
	assert(VALIDATE_STOP(&state) == 0);
	state = valid_report(FKI_X86_STOP_SHUTDOWN);
	state.stop_aux0 = FKI_X86_STOP_SHUTDOWN_TRIPLE_FAULT;
	assert(VALIDATE_STOP(&state) == 0);
	state.stop_aux0 = FKI_X86_STOP_SHUTDOWN_UNRECOVERABLE;
	assert(VALIDATE_STOP(&state) == 0);
}

static void test_metadata_rejections(void)
{
	struct fki_x86_context_control_v1 state = valid_report(FKI_X86_STOP_NORMAL_RETURN);

	assert(VALIDATE_STOP(NULL) == -EINVAL);
	{
		unsigned char misaligned[sizeof(state) + 1];

		memcpy(misaligned + 1, &state, sizeof(state));
		assert(VALIDATE_STOP((struct fki_x86_context_control_v1 *)(misaligned + 1)) == -EINVAL);
	}
	state.abi_version = 0;
	assert(VALIDATE_STOP(&state) == -EOPNOTSUPP);
	state = valid_report(FKI_X86_STOP_NORMAL_RETURN);
	state.control_version = 2;
	assert(VALIDATE_STOP(&state) == -EOPNOTSUPP);
	state = valid_report(FKI_X86_STOP_NORMAL_RETURN);
	state.state_size--;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_NORMAL_RETURN);
	state.profile = FKI_X86_CONTEXT_PROFILE_XNU_PREBOOT_LONG;
	assert(VALIDATE_STOP(&state) == -EOPNOTSUPP);
	state = valid_report(FKI_X86_STOP_NORMAL_RETURN);
	state.operation = 0;
	assert(VALIDATE_STOP(&state) == -EOPNOTSUPP);
	state.operation = 3;
	assert(VALIDATE_STOP(&state) == -EOPNOTSUPP);
	state = valid_report(FKI_X86_STOP_NORMAL_RETURN);
	state.stop_reason = 0;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state.stop_reason = 6;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_NORMAL_RETURN);
	state.flags = 1;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_NORMAL_RETURN);
	state.stop_mask = 1;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_NORMAL_RETURN);
	state.boot_address = 1;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_NORMAL_RETURN);
	state.reserved[12] = 1;
	assert(VALIDATE_STOP(&state) == -EINVAL);
}

static void test_normal_and_abort_rejections(void)
{
	struct fki_x86_context_control_v1 state;
	unsigned int reasons[] = {
		FKI_X86_STOP_NORMAL_RETURN,
		FKI_X86_STOP_SOFTWARE_ABORT,
	};
	unsigned int i;

	for (i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
		state = valid_report(reasons[i]);
		state.stop_vector = FKI_X86_STOP_VECTOR_PAGE_FAULT;
		assert(VALIDATE_STOP(&state) == -EINVAL);
		state = valid_report(reasons[i]);
		state.stop_error_code = 1;
		assert(VALIDATE_STOP(&state) == -EINVAL);
		state = valid_report(reasons[i]);
		state.stop_cr2 = 0x1000;
		assert(VALIDATE_STOP(&state) == -EINVAL);
		state = valid_report(reasons[i]);
		state.stop_aux0 = 1;
		assert(VALIDATE_STOP(&state) == -EINVAL);
		state = valid_report(reasons[i]);
		state.stop_aux1 = 1;
		assert(VALIDATE_STOP(&state) == -EINVAL);
		state = valid_report(reasons[i]);
		state.stop_rip = 0;
		assert(VALIDATE_STOP(&state) == -ERANGE);
		state = valid_report(reasons[i]);
		state.stop_rsp = 0;
		assert(VALIDATE_STOP(&state) == -ERANGE);
		state = valid_report(reasons[i]);
		state.stop_rip = NONCANONICAL;
		assert(VALIDATE_STOP(&state) == -ERANGE);
		state = valid_report(reasons[i]);
		state.stop_rsp = NONCANONICAL;
		assert(VALIDATE_STOP(&state) == -ERANGE);
	}
}

static void test_page_fault_rejections(void)
{
	struct fki_x86_context_control_v1 state = valid_report(FKI_X86_STOP_PAGE_FAULT);

	state.stop_vector = FKI_X86_STOP_VECTOR_GENERAL_PROTECTION;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_PAGE_FAULT);
	state.stop_vector = FKI_X86_STOP_VECTOR_PAGE_FAULT;
	state.stop_error_code = 1ULL << 7;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state.stop_error_code = 1ULL << 16;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state.stop_error_code = 1ULL << 32;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_PAGE_FAULT);
	state.stop_vector = FKI_X86_STOP_VECTOR_PAGE_FAULT;
	state.stop_cr2 = NONCANONICAL;
	assert(VALIDATE_STOP(&state) == -ERANGE);
	state = valid_report(FKI_X86_STOP_PAGE_FAULT);
	state.stop_vector = FKI_X86_STOP_VECTOR_PAGE_FAULT;
	state.stop_rip = 0;
	assert(VALIDATE_STOP(&state) == -ERANGE);
	state = valid_report(FKI_X86_STOP_PAGE_FAULT);
	state.stop_vector = FKI_X86_STOP_VECTOR_PAGE_FAULT;
	state.stop_rsp = 0;
	assert(VALIDATE_STOP(&state) == -ERANGE);
	state = valid_report(FKI_X86_STOP_PAGE_FAULT);
	state.stop_vector = FKI_X86_STOP_VECTOR_PAGE_FAULT;
	state.stop_rip = NONCANONICAL;
	assert(VALIDATE_STOP(&state) == -ERANGE);
	state = valid_report(FKI_X86_STOP_PAGE_FAULT);
	state.stop_vector = FKI_X86_STOP_VECTOR_PAGE_FAULT;
	state.stop_aux0 = 1;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_PAGE_FAULT);
	state.stop_vector = FKI_X86_STOP_VECTOR_PAGE_FAULT;
	state.stop_aux1 = 1;
	assert(VALIDATE_STOP(&state) == -EINVAL);
}

static void test_gp_rejections(void)
{
	struct fki_x86_context_control_v1 state = valid_report(FKI_X86_STOP_GENERAL_PROTECTION);

	state.stop_vector = FKI_X86_STOP_VECTOR_PAGE_FAULT;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_GENERAL_PROTECTION);
	state.stop_vector = FKI_X86_STOP_VECTOR_GENERAL_PROTECTION;
	state.stop_error_code = 1ULL << 16;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_GENERAL_PROTECTION);
	state.stop_vector = FKI_X86_STOP_VECTOR_GENERAL_PROTECTION;
	state.stop_cr2 = 0x1000;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_GENERAL_PROTECTION);
	state.stop_vector = FKI_X86_STOP_VECTOR_GENERAL_PROTECTION;
	state.stop_aux0 = 1;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_GENERAL_PROTECTION);
	state.stop_vector = FKI_X86_STOP_VECTOR_GENERAL_PROTECTION;
	state.stop_aux1 = 1;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_GENERAL_PROTECTION);
	state.stop_vector = FKI_X86_STOP_VECTOR_GENERAL_PROTECTION;
	state.stop_rip = 0;
	assert(VALIDATE_STOP(&state) == -ERANGE);
	state = valid_report(FKI_X86_STOP_GENERAL_PROTECTION);
	state.stop_vector = FKI_X86_STOP_VECTOR_GENERAL_PROTECTION;
	state.stop_rsp = NONCANONICAL;
	assert(VALIDATE_STOP(&state) == -ERANGE);
}

static void test_shutdown_rejections(void)
{
	struct fki_x86_context_control_v1 state = valid_report(FKI_X86_STOP_SHUTDOWN);

	state.stop_aux0 = 0;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state.stop_aux0 = 4;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_SHUTDOWN);
	state.stop_aux0 = FKI_X86_STOP_SHUTDOWN_HALT;
	state.stop_vector = 8;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_SHUTDOWN);
	state.stop_aux0 = FKI_X86_STOP_SHUTDOWN_HALT;
	state.stop_error_code = 1;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_SHUTDOWN);
	state.stop_aux0 = FKI_X86_STOP_SHUTDOWN_HALT;
	state.stop_cr2 = 0x1000;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_SHUTDOWN);
	state.stop_aux0 = FKI_X86_STOP_SHUTDOWN_HALT;
	state.stop_aux1 = FKI_X86_STOP_SHUTDOWN_RESTART_REQUESTED << 1;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_SHUTDOWN);
	state.stop_aux0 = FKI_X86_STOP_SHUTDOWN_TRIPLE_FAULT;
	state.stop_aux1 = FKI_X86_STOP_SHUTDOWN_RESTART_REQUESTED;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_SHUTDOWN);
	state.stop_aux0 = FKI_X86_STOP_SHUTDOWN_UNRECOVERABLE;
	state.stop_aux1 = FKI_X86_STOP_SHUTDOWN_RESTART_REQUESTED;
	assert(VALIDATE_STOP(&state) == -EINVAL);
	state = valid_report(FKI_X86_STOP_SHUTDOWN);
	state.stop_aux0 = FKI_X86_STOP_SHUTDOWN_HALT;
	state.stop_rsp = 0;
	assert(VALIDATE_STOP(&state) == -ERANGE);
	state = valid_report(FKI_X86_STOP_SHUTDOWN);
	state.stop_aux0 = FKI_X86_STOP_SHUTDOWN_HALT;
	state.stop_rip = NONCANONICAL;
	assert(VALIDATE_STOP(&state) == -ERANGE);
}

static void test_entry_still_rejects_reports(void)
{
	struct fki_x86_context_control_v1 state = valid_report(FKI_X86_STOP_NORMAL_RETURN);
	struct fki_x86_host_constraints host = valid_host();

	assert(VALIDATE(&state, &host) == -EINVAL);
	state = valid_entry();
	assert(VALIDATE(&state, &host) == 0);
	assert(VALIDATE_STOP(&state) == -EINVAL);
}

int main(void)
{
	test_valid_reports();
	test_metadata_rejections();
	test_normal_and_abort_rejections();
	test_page_fault_rejections();
	test_gp_rejections();
	test_shutdown_rejections();
	test_entry_still_rejects_reports();
	puts("FKI x86 stop-report fixtures passed");
	return 0;
}
