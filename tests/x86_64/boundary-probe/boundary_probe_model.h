/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef X86_BOUNDARY_PROBE_MODEL_H
#define X86_BOUNDARY_PROBE_MODEL_H

#include <linux/types.h>

#include "fki_x86_64_context.h"
#include "fki_x86_64_cr3.h"

struct bp_host_snapshot {
	int cpu;
	struct task_struct *task;
	unsigned long stack_page;
	unsigned long stack_end;
	unsigned long rsp;
	unsigned long rflags;
	unsigned long cr0;
	unsigned long cr4;
	struct fki_x86_cr3 cr3;
	u64 efer;
	u64 fs_base;
	u64 gs_base;
	u64 kernel_gs_base;
	u64 xcr0;
	unsigned long gdt_base;
	unsigned long idt_base;
	unsigned int gdt_limit;
	unsigned int idt_limit;
	unsigned int ldt;
	unsigned int tr;
	unsigned int preempt_count;
	bool irq_disabled;
};

int bp_probe_snapshot(struct bp_host_snapshot *snapshot);
bool bp_probe_snapshots_match(const struct bp_host_snapshot *before,
			      const struct bp_host_snapshot *after);
int bp_probe_xsave_constraints(u64 *bytes, u64 *avx_end,
			       u64 *supported_xcr0);
int bp_probe_prepare_control_model(const struct bp_host_snapshot *snapshot,
				   unsigned long xsave_address,
				   u64 xsave_bytes, u64 xsave_avx_end,
				   u64 supported_xcr0, unsigned long entry_rsp,
				   unsigned long entry_rip,
				   struct fki_x86_context_control_v1 *control,
				   struct fki_x86_host_constraints *host);

#endif
