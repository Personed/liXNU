// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>

#include "../../fki-ko/fki_x86_64_cpu_quiescence_decision.h"

static void expect_entry(enum fki_x86_cpu_quiescence_route route,
			 enum fki_x86_cpu_quiescence_class expected_class,
			 unsigned int expected_flags)
{
	enum fki_x86_cpu_quiescence_class class;
	unsigned int flags;

	assert(fki_x86_cpu_quiescence_lookup(route, &class, &flags) == 0);
	assert(class == expected_class);
	assert(flags == expected_flags);
	assert(fki_x86_cpu_quiescence_validate_entry(route) == 0);
}

int main(void)
{
	enum fki_x86_cpu_quiescence_class class;
	unsigned int flags;
	unsigned int common = FKI_X86_QUIESCENCE_FLAG_NO_OWNERSHIP_TRANSFER |
		FKI_X86_QUIESCENCE_FLAG_CONTROL_MEMORY_UNDECIDED |
		FKI_X86_QUIESCENCE_ASYNC_UNSUPPORTED |
		FKI_X86_QUIESCENCE_FLAG_INSUFFICIENT_ALONE;
	unsigned int prerequisite = FKI_X86_QUIESCENCE_FLAG_POTENTIAL_PREREQUISITE |
		FKI_X86_QUIESCENCE_FLAG_REQUIRES_STRONGER_PROTOCOL;
	enum fki_x86_cpu_quiescence_route route;

	for (route = 0; route < FKI_X86_QUIESCENCE_ROUTE_COUNT; route++)
		assert(fki_x86_cpu_quiescence_validate_entry(route) == 0);

	expect_entry(FKI_X86_QUIESCENCE_LIVE_LINUX_CPU,
		     FKI_X86_QUIESCENCE_CLASS_FORBIDDEN_PRODUCTION,
		     common);
	expect_entry(FKI_X86_QUIESCENCE_STOP_MACHINE,
		     FKI_X86_QUIESCENCE_CLASS_POTENTIALLY_NECESSARY_BUT_INSUFFICIENT,
		     common | FKI_X86_QUIESCENCE_FLAG_TEMPORARY_ONLY | prerequisite);
	expect_entry(FKI_X86_QUIESCENCE_LOGICAL_OFFLINE_HOTPLUG,
		     FKI_X86_QUIESCENCE_CLASS_POTENTIALLY_NECESSARY_BUT_INSUFFICIENT,
		     common | FKI_X86_QUIESCENCE_FLAG_REONLINE_WAKE_RESET_UNDECIDED |
		     prerequisite);
	expect_entry(FKI_X86_QUIESCENCE_ROLLBACK_REONLINE,
		     FKI_X86_QUIESCENCE_CLASS_UNRESOLVED_STRONGER_PROTOCOL,
		     common | FKI_X86_QUIESCENCE_FLAG_REONLINE_WAKE_RESET_UNDECIDED |
		     FKI_X86_QUIESCENCE_FLAG_REQUIRES_STRONGER_PROTOCOL);

	assert(fki_x86_cpu_quiescence_lookup(FKI_X86_QUIESCENCE_ROUTE_COUNT,
					       &class, &flags) == -EINVAL);
	assert(fki_x86_cpu_quiescence_lookup(FKI_X86_QUIESCENCE_STOP_MACHINE,
					       NULL, &flags) == -EINVAL);
	assert(fki_x86_cpu_quiescence_lookup(FKI_X86_QUIESCENCE_STOP_MACHINE,
					       &class, NULL) == -EINVAL);

	assert(fki_x86_cpu_quiescence_validate_rules(
		FKI_X86_QUIESCENCE_CLASS_UNRESOLVED_STRONGER_PROTOCOL,
		FKI_X86_QUIESCENCE_ASYNC_UNSUPPORTED) == -EUCLEAN);
	assert(fki_x86_cpu_quiescence_validate_rules(
		FKI_X86_QUIESCENCE_CLASS_POTENTIALLY_NECESSARY_BUT_INSUFFICIENT,
		FKI_X86_QUIESCENCE_ASYNC_UNSUPPORTED) == -EUCLEAN);
	assert(fki_x86_cpu_quiescence_validate_rules(
		FKI_X86_QUIESCENCE_CLASS_FORBIDDEN_PRODUCTION,
		common | FKI_X86_QUIESCENCE_FLAG_POTENTIAL_PREREQUISITE) == -EUCLEAN);
	assert(fki_x86_cpu_quiescence_validate_rules(
		FKI_X86_QUIESCENCE_CLASS_FORBIDDEN_PRODUCTION,
		common & ~FKI_X86_QUIESCENCE_FLAG_NMI_UNSUPPORTED) == -EUCLEAN);
	assert(fki_x86_cpu_quiescence_validate_rules(
		FKI_X86_QUIESCENCE_CLASS_FORBIDDEN_PRODUCTION,
		common & ~FKI_X86_QUIESCENCE_FLAG_IRQ_UNSUPPORTED) == -EUCLEAN);
	assert(fki_x86_cpu_quiescence_validate_rules(
		FKI_X86_QUIESCENCE_CLASS_FORBIDDEN_PRODUCTION,
		common & ~FKI_X86_QUIESCENCE_FLAG_MACHINE_CHECK_UNSUPPORTED) == -EUCLEAN);
	assert(fki_x86_cpu_quiescence_validate_rules(
		FKI_X86_QUIESCENCE_CLASS_FORBIDDEN_PRODUCTION,
		common & ~FKI_X86_QUIESCENCE_FLAG_NO_OWNERSHIP_TRANSFER) == -EUCLEAN);
	assert(fki_x86_cpu_quiescence_validate_rules(
		FKI_X86_QUIESCENCE_CLASS_FORBIDDEN_PRODUCTION,
		common & ~FKI_X86_QUIESCENCE_FLAG_CONTROL_MEMORY_UNDECIDED) == -EUCLEAN);
	assert(fki_x86_cpu_quiescence_validate_rules(
		FKI_X86_QUIESCENCE_CLASS_FORBIDDEN_PRODUCTION,
		common & ~FKI_X86_QUIESCENCE_FLAG_INSUFFICIENT_ALONE) == -EUCLEAN);
	assert(fki_x86_cpu_quiescence_validate_rules(
		FKI_X86_QUIESCENCE_CLASS_FORBIDDEN_PRODUCTION,
		common | (1U << 31)) == -EINVAL);
	assert(fki_x86_cpu_quiescence_validate_rules(~0U, 0) == -EINVAL);

	puts("x86 CPU-quiescence decision fixtures passed");
	return 0;
}
