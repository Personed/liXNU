// SPDX-License-Identifier: APSL-2.0
#include <assert.h>
#include <stdio.h>
#include <i386/linux_hosted_boot_memory.h>

int main(void)
{
	struct linux_hosted_boot_memory range;
	const uint64_t leaf = LINUX_HOSTED_LARGE_PAGE;
	const uint64_t limit = LINUX_HOSTED_PA_LIMIT;

	assert(!linux_hosted_boot_memory_init(&range, 0, leaf));
	assert(!linux_hosted_boot_memory_init(&range, leaf, 0));
	assert(!linux_hosted_boot_memory_init(&range, leaf + 4096, leaf));
	assert(!linux_hosted_boot_memory_init(&range, leaf, UINT64_MAX & ~(leaf - 1)));
	assert(!linux_hosted_boot_memory_init(&range, limit - leaf, 2 * leaf));

	assert(linux_hosted_boot_memory_init(&range, 4 * leaf, 2 * leaf));
	assert(linux_hosted_boot_contains(&range, 4 * leaf, leaf));
	assert(linux_hosted_boot_contains(&range, 5 * leaf, leaf));
	assert(!linux_hosted_boot_contains(&range, 3 * leaf, leaf));
	assert(!linux_hosted_boot_contains(&range, 6 * leaf - 4096, leaf));
	assert(!linux_hosted_boot_contains(&range, 6 * leaf, 4096));
	assert(!linux_hosted_boot_contains(&range, 4 * leaf, 0));
	assert(linux_hosted_boot_allocate(&range, 6 * leaf - 4096, 4096));

	assert(linux_hosted_boot_memory_init(&range, limit - leaf, leaf));
	assert(linux_hosted_boot_contains(&range, limit - leaf, leaf));
	assert(!linux_hosted_boot_allocate(&range, limit - 4096, 4096));
	assert(linux_hosted_boot_allocate(&range, limit - 8192, 4096));
	puts("xnu x86 hosted pmap range fixtures passed");
	return 0;
}
