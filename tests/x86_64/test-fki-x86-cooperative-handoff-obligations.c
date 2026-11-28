// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>

#include "../../fki-ko/fki_x86_64_cooperative_handoff_obligations.h"

static void expect_entry(enum fki_x86_coop_obligation obligation,
			 enum fki_x86_coop_obligation_class expected_class,
			 unsigned int expected_flags)
{
	struct fki_x86_coop_obligation_entry entry;

	assert(fki_x86_coop_obligation_lookup(obligation, &entry) == 0);
	assert(entry.class == expected_class);
	assert(entry.flags == expected_flags);
	assert(fki_x86_coop_obligation_validate_entry(obligation) == 0);
}

int main(void)
{
	struct fki_x86_coop_obligation_entry entry;
	enum fki_x86_coop_obligation obligation;
	unsigned int common = FKI_X86_COOP_COMMON_FLAGS;
	unsigned int shared = FKI_X86_COOP_FLAG_SHARED_SINGLE_RESOURCE_CONFLICT;
	unsigned int control = FKI_X86_COOP_FLAG_CONTROL_STATE_INPUT;
	unsigned int memory = FKI_X86_COOP_FLAG_MEMORY_OWNERSHIP_INPUT;
	unsigned int exception = FKI_X86_COOP_FLAG_EXCEPTION_INTERRUPT_INPUT;
	unsigned int patch = FKI_X86_COOP_FLAG_REQUIRES_XNU_PATCH_OR_CONTRACT;

	for (obligation = 0; obligation < FKI_X86_COOP_OBLIGATION_COUNT; obligation++)
		assert(fki_x86_coop_obligation_validate_entry(obligation) == 0);

	expect_entry(FKI_X86_COOP_CR3_KPTI_PCID_ADDRESS_SPACE_SPLIT,
		     FKI_X86_COOP_CLASS_STRUCTURAL_MISMATCH,
		     common | shared | control | memory);
	expect_entry(FKI_X86_COOP_GS_SWAPGS_CPU_DATA_CONFLICT,
		     FKI_X86_COOP_CLASS_STRUCTURAL_MISMATCH,
		     common | shared | control | patch);
	expect_entry(FKI_X86_COOP_SYSCALL_MSR_STAR_LSTAR_SFMASK,
		     FKI_X86_COOP_CLASS_FORBIDDEN_AS_CURRENT_CLAIM,
		     common | shared | control | patch);
	expect_entry(FKI_X86_COOP_CONTROL_REGISTER_MEDIATION,
		     FKI_X86_COOP_CLASS_UNRESOLVED_STRONGER_PROTOCOL,
		     common | control);
	expect_entry(FKI_X86_COOP_LAPIC_TPR_IPI_COEXISTENCE,
		     FKI_X86_COOP_CLASS_REQUIRED_BUT_UNDESIGNED,
		     common | control | exception);
	expect_entry(FKI_X86_COOP_TSS_IST_STACK_OWNERSHIP,
		     FKI_X86_COOP_CLASS_STRUCTURAL_MISMATCH,
		     common | shared | control | exception);
	expect_entry(FKI_X86_COOP_XSAVE_AVX_FPU_STATE,
		     FKI_X86_COOP_CLASS_REQUIRED_BUT_UNDESIGNED,
		     common | FKI_X86_COOP_FLAG_USER_STATE_CONTEXT_INPUT);
	expect_entry(FKI_X86_COOP_PHYSICAL_MEMORY_RESERVATION,
		     FKI_X86_COOP_CLASS_REQUIRED_BUT_UNDESIGNED,
		     common | memory);
	expect_entry(FKI_X86_COOP_PMAP_ADDRESS_SPACE_INTEGRATION,
		     FKI_X86_COOP_CLASS_UNRESOLVED_STRONGER_PROTOCOL,
		     common | memory | control);
	expect_entry(FKI_X86_COOP_EXCEPTION_ENTRY_FORWARDING,
		     FKI_X86_COOP_CLASS_REQUIRED_BUT_UNDESIGNED,
		     common | exception | patch);
	expect_entry(FKI_X86_COOP_INTERRUPT_PREEMPTION_COEXISTENCE,
		     FKI_X86_COOP_CLASS_UNRESOLVED_STRONGER_PROTOCOL,
		     common | exception);
	expect_entry(FKI_X86_COOP_ROLLBACK_CLEANUP,
		     FKI_X86_COOP_CLASS_REQUIRED_BUT_UNDESIGNED,
		     common | FKI_X86_COOP_FLAG_ROLLBACK_INPUT);

	assert(fki_x86_coop_obligation_lookup(FKI_X86_COOP_OBLIGATION_COUNT,
					      &entry) == -EINVAL);
	assert(fki_x86_coop_obligation_lookup(FKI_X86_COOP_GS_SWAPGS_CPU_DATA_CONFLICT,
					      NULL) == -EINVAL);

	assert(fki_x86_coop_validate_rules(FKI_X86_COOP_CLASS_STRUCTURAL_MISMATCH,
					   common | control) == -EUCLEAN);
	assert(fki_x86_coop_validate_rules(FKI_X86_COOP_CLASS_FORBIDDEN_AS_CURRENT_CLAIM,
					   common | control) == -EUCLEAN);
	assert(fki_x86_coop_validate_rules(FKI_X86_COOP_CLASS_REQUIRED_BUT_UNDESIGNED,
					   common) == -EUCLEAN);
	assert(fki_x86_coop_validate_rules(FKI_X86_COOP_CLASS_REQUIRED_BUT_UNDESIGNED,
					   control) == -EUCLEAN);
	assert(fki_x86_coop_validate_rules(FKI_X86_COOP_CLASS_UNRESOLVED_STRONGER_PROTOCOL,
					   common | control | (1U << 31)) == -EINVAL);
	assert(fki_x86_coop_validate_rules(~0U, common | control) == -EINVAL);

	puts("x86 cooperative handoff proof-obligation fixtures passed");
	return 0;
}
