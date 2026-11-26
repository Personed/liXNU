// SPDX-License-Identifier: GPL-2.0-only
/*
 * Foreign kernel interface: x86_64 host services and stack boundary.
 *
 * This backend does not yet enter or execute XNU. It supplies the first
 * Linux-side hooks for the x86_64 FKI port; the XNU world switch, address-space
 * isolation, and fault delivery are separate work.
 */
#include "fki.h"
#include "fki_internal.h"
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/smp.h>
#include <linux/timekeeping.h>
#include <asm/fpu/api.h>

#define FKI_ALT_STACK_ORDER get_order(2 * THREAD_SIZE)
#define FKI_ALT_STACK_SIZE (PAGE_SIZE << FKI_ALT_STACK_ORDER)

static void *fki_alt_stack[NR_CPUS];

unsigned long fki_caps(void)
{
	/* No x86_64 XNU execution or isolated user-context capability is exposed yet. */
	return 0;
}
EXPORT_SYMBOL(fki_caps);

int fki_cpu_id(void)
{
	return smp_processor_id();
}
EXPORT_SYMBOL(fki_cpu_id);

fki_u64 fki_counter_frequency(void)
{
	/* fki_counter() is CLOCK_MONOTONIC_RAW nanoseconds. */
	return 1000000000ULL;
}
EXPORT_SYMBOL(fki_counter_frequency);

fki_u64 fki_counter(void)
{
	return ktime_get_raw_ns();
}
EXPORT_SYMBOL(fki_counter);

void fki_fpsimd_forget(void)
{
	/* Save the Linux task's xstate before a future foreign context may use FPU/SIMD. */
	kernel_fpu_begin();
	kernel_fpu_end();
}

static bool fki_stack_contains(unsigned long sp, const void *base, unsigned long size)
{
	unsigned long lo = (unsigned long)base;

	return base && sp >= lo && sp - lo < size;
}

void fki_run_on_stack(void (*fn)(void *), void *arg, void *top);

void fki_arch_linux_runs(void (*fn)(void *), void *arg)
{
	unsigned long sp;
	int cpu = smp_processor_id();
	void *task_stack = task_stack_page(current);

	asm volatile("mov %%rsp, %0" : "=r"(sp));
	if (fki_stack_contains(sp, task_stack, THREAD_SIZE) ||
	    fki_stack_contains(sp, fki_alt_stack[cpu], FKI_ALT_STACK_SIZE)) {
		fn(arg);
		return;
	}
	/* fki_arch_init() guarantees one alternate Linux stack per possible CPU. */
	BUG_ON(!fki_alt_stack[cpu]);
	fki_run_on_stack(fn, arg, (char *)fki_alt_stack[cpu] + FKI_ALT_STACK_SIZE);
}

static void fki_free_alt_stacks(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (fki_alt_stack[cpu])
			free_pages((unsigned long)fki_alt_stack[cpu], FKI_ALT_STACK_ORDER);
		fki_alt_stack[cpu] = NULL;
	}
}

int fki_arch_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct page *pg = alloc_pages(GFP_KERNEL | __GFP_ZERO, FKI_ALT_STACK_ORDER);

		if (!pg) {
			fki_free_alt_stacks();
			return -ENOMEM;
		}
		fki_alt_stack[cpu] = page_address(pg);
	}
	return 0;
}

void fki_arch_exit(void)
{
	fki_free_alt_stacks();
}
