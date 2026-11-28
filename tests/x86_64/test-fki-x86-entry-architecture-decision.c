// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>

#include "../../fki-ko/fki_x86_64_entry_architecture_decision.h"

static void expect_entry(enum fki_x86_entry_architecture_route route,
			 enum fki_x86_entry_architecture_class expected_class,
			 unsigned int expected_flags)
{
	enum fki_x86_entry_architecture_class class;
	unsigned int flags;

	assert(fki_x86_entry_architecture_lookup(route, &class, &flags) == 0);
	assert(class == expected_class);
	assert(flags == expected_flags);
	assert(fki_x86_entry_architecture_validate_entry(route) == 0);
}

int main(void)
{
	enum fki_x86_entry_architecture_class class;
	unsigned int flags;
	unsigned int unresolved = FKI_X86_ENTRY_ARCH_COMMON_UNRESOLVED;
	unsigned int blocked = FKI_X86_ENTRY_ARCH_FLAG_REQUIRES_PHYSICAL_CPU_POSSESSION |
		FKI_X86_ENTRY_ARCH_FLAG_BLOCKED_BY_INJECTION_GAP |
		FKI_X86_ENTRY_ARCH_FLAG_REQUIRES_MODULE_API_OR_KERNEL_COOP;
	unsigned int cooperative = FKI_X86_ENTRY_ARCH_FLAG_AVOIDS_PHYSICAL_CPU_POSSESSION |
		FKI_X86_ENTRY_ARCH_FLAG_REQUIRES_XNU_PATCH |
		FKI_X86_ENTRY_ARCH_FLAG_REQUIRES_ABI_HANDOFF_CONTRACT |
		FKI_X86_ENTRY_ARCH_FLAG_ARM64_ANALOGY_ONLY |
		FKI_X86_ENTRY_ARCH_FLAG_XNU_PREBOOT_LONG_NOT_VALIDATED;
	enum fki_x86_entry_architecture_route route;

	for (route = 0; route < FKI_X86_ENTRY_ARCH_ROUTE_COUNT; route++)
		assert(fki_x86_entry_architecture_validate_entry(route) == 0);

	expect_entry(FKI_X86_ENTRY_ARCH_PROTECTED_UNMODIFIED_PHYSICAL_CPU,
		     FKI_X86_ENTRY_ARCH_BLOCKED_UNRESOLVED_INJECTION_OWNERSHIP,
		     blocked | unresolved);
	expect_entry(FKI_X86_ENTRY_ARCH_INIT_SIPI_MANUAL_OFFLINE_INJECTION,
		     FKI_X86_ENTRY_ARCH_BLOCKED_UNRESOLVED_INJECTION_OWNERSHIP,
		     blocked | unresolved |
		     FKI_X86_ENTRY_ARCH_FLAG_INIT_SIPI_REIMPLEMENTATION_FORBIDDEN);
	expect_entry(FKI_X86_ENTRY_ARCH_COOPERATIVE_PATCHED_LINUX_OWNERSHIP,
		     FKI_X86_ENTRY_ARCH_UNRESOLVED_NOT_BLOCKED_BY_INJECTION_GAP,
		     cooperative | unresolved);

	assert(fki_x86_entry_architecture_lookup(FKI_X86_ENTRY_ARCH_ROUTE_COUNT,
						 &class, &flags) == -EINVAL);
	assert(fki_x86_entry_architecture_lookup(FKI_X86_ENTRY_ARCH_INIT_SIPI_MANUAL_OFFLINE_INJECTION,
						 NULL, &flags) == -EINVAL);
	assert(fki_x86_entry_architecture_lookup(FKI_X86_ENTRY_ARCH_INIT_SIPI_MANUAL_OFFLINE_INJECTION,
						 &class, NULL) == -EINVAL);

	assert(fki_x86_entry_architecture_validate_rules(
		FKI_X86_ENTRY_ARCH_BLOCKED_UNRESOLVED_INJECTION_OWNERSHIP,
		unresolved) == -EUCLEAN);
	assert(fki_x86_entry_architecture_validate_rules(
		FKI_X86_ENTRY_ARCH_UNRESOLVED_NOT_BLOCKED_BY_INJECTION_GAP,
		unresolved | FKI_X86_ENTRY_ARCH_FLAG_AVOIDS_PHYSICAL_CPU_POSSESSION) == -EUCLEAN);
	assert(fki_x86_entry_architecture_validate_rules(
		FKI_X86_ENTRY_ARCH_UNRESOLVED_NOT_BLOCKED_BY_INJECTION_GAP,
		(cooperative | unresolved) &
		~FKI_X86_ENTRY_ARCH_FLAG_MEMORY_OWNERSHIP_UNRESOLVED) == -EUCLEAN);
	assert(fki_x86_entry_architecture_validate_rules(
		FKI_X86_ENTRY_ARCH_UNRESOLVED_NOT_BLOCKED_BY_INJECTION_GAP,
		cooperative | unresolved |
		FKI_X86_ENTRY_ARCH_FLAG_REQUIRES_PHYSICAL_CPU_POSSESSION) == -EUCLEAN);
	assert(fki_x86_entry_architecture_validate_rules(
		FKI_X86_ENTRY_ARCH_UNRESOLVED_NOT_BLOCKED_BY_INJECTION_GAP,
		cooperative | unresolved |
		FKI_X86_ENTRY_ARCH_FLAG_BLOCKED_BY_INJECTION_GAP) == -EUCLEAN);
	assert(fki_x86_entry_architecture_validate_rules(
		FKI_X86_ENTRY_ARCH_BLOCKED_UNRESOLVED_INJECTION_OWNERSHIP,
		blocked | unresolved | (1U << 31)) == -EINVAL);
	assert(fki_x86_entry_architecture_validate_rules(~0U, 0) == -EINVAL);

	puts("x86 entry-architecture decision fixtures passed");
	return 0;
}
