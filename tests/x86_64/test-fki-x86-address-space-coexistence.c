// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>

#include "../../fki-ko/fki_x86_64_address_space_coexistence.h"

static void expect(enum fki_x86_addrspace_item item,
		   enum fki_x86_addrspace_class class, unsigned int flags)
{
	struct fki_x86_addrspace_entry entry;
	assert(fki_x86_addrspace_lookup(item, &entry) == 0);
	assert(entry.class == class);
	assert(entry.flags == flags);
	assert(fki_x86_addrspace_validate_entry(item) == 0);
}

int main(void)
{
	enum fki_x86_addrspace_item item;
	struct fki_x86_addrspace_entry entry;
	unsigned int facts = FKI_X86_AS_FACT_FLAGS;
	unsigned int infer = FKI_X86_AS_FLAG_INFERENCE_NOT_PROOF;
	unsigned int pmap = FKI_X86_AS_FLAG_PMAP_DESIGN_REQUIRED;
	unsigned int cr3 = FKI_X86_AS_FLAG_CR3_MEDIATION_REQUIRED;
	unsigned int cond = FKI_X86_AS_FLAG_CONDITIONAL_CONFIG;
	unsigned int memdep = FKI_X86_AS_FLAG_MEMORY_SIZE_DEPENDENT;

	for (item = 0; item < FKI_X86_AS_ITEM_COUNT; item++)
		assert(fki_x86_addrspace_validate_entry(item) == 0);

	expect(FKI_X86_AS_XNU_LOW_KERNEL_VS_LINUX_INDEX511_HOLE,
	       FKI_X86_AS_CLASS_DISJOINT_UNDER_CURRENT_EVIDENCE, facts | infer);
	expect(FKI_X86_AS_XNU_FULL_KERNEL_RANGE_VS_LINUX_HIGH_INDEX511,
	       FKI_X86_AS_CLASS_LAYOUT_CONFLICT_UNRESOLVED, facts | pmap | cr3);
	expect(FKI_X86_AS_XNU_BASEMENT_VS_LINUX_ESPFIX_INDEX510,
	       FKI_X86_AS_CLASS_LAYOUT_CONFLICT_UNRESOLVED, facts | cond | infer | pmap);
	expect(FKI_X86_AS_XNU_SMALL_PHYSMAP_VS_LINUX_NEIGHBORS,
	       FKI_X86_AS_CLASS_DISJOINT_UNDER_CURRENT_EVIDENCE, facts | memdep | infer);
	expect(FKI_X86_AS_XNU_LARGE_PHYSMAP_VS_LINUX_CPU_ENTRY_AREA,
	       FKI_X86_AS_CLASS_POTENTIAL_OVERLAP, facts | memdep | pmap);
	expect(FKI_X86_AS_XNU_DBLMAP_KASAN_VS_LINUX_KASAN_RANGE,
	       FKI_X86_AS_CLASS_POTENTIAL_OVERLAP, facts | cond | infer | pmap);
	expect(FKI_X86_AS_KPTI_USER_CR3_ENTRY_MAPPINGS,
	       FKI_X86_AS_CLASS_HARD_UNRESOLVED_PROOF_OBLIGATION,
	       FKI_X86_AS_FLAG_LINUX_V68_DOCUMENTED_FACT |
	       FKI_X86_AS_FLAG_KPTI_ENTRY_INPUT | cr3 | pmap |
	       FKI_X86_AS_FLAG_NO_PRODUCTION_SAFETY);
	expect(FKI_X86_AS_FULL_KERNEL_CR3_COMPOSITION,
	       FKI_X86_AS_CLASS_HARD_UNRESOLVED_PROOF_OBLIGATION, facts | cr3 | pmap);
	expect(FKI_X86_AS_PCID_TLB_NAMESPACE,
	       FKI_X86_AS_CLASS_HARD_UNRESOLVED_PROOF_OBLIGATION,
	       facts | FKI_X86_AS_FLAG_PCID_INPUT | cr3 | pmap);

	assert(fki_x86_addrspace_lookup(FKI_X86_AS_ITEM_COUNT, &entry) == -EINVAL);
	assert(fki_x86_addrspace_lookup(FKI_X86_AS_PCID_TLB_NAMESPACE, 0) == -EINVAL);
	assert(fki_x86_addrspace_validate_rules(FKI_X86_AS_CLASS_HARD_UNRESOLVED_PROOF_OBLIGATION,
						 facts | pmap) == -EUCLEAN);
	assert(fki_x86_addrspace_validate_rules(FKI_X86_AS_CLASS_LAYOUT_CONFLICT_UNRESOLVED,
						 facts) == -EUCLEAN);
	assert(fki_x86_addrspace_validate_rules(FKI_X86_AS_CLASS_POTENTIAL_OVERLAP,
						 facts & ~FKI_X86_AS_FLAG_NO_PRODUCTION_SAFETY) == -EUCLEAN);
	assert(fki_x86_addrspace_validate_rules(FKI_X86_AS_CLASS_POTENTIAL_OVERLAP,
						 facts | (1U << 31)) == -EINVAL);
	assert(fki_x86_addrspace_validate_rules(~0U, facts) == -EINVAL);

	puts("x86 address-space coexistence fixtures passed");
	return 0;
}
