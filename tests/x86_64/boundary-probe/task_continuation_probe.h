/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef X86_TASK_CONTINUATION_PROBE_H
#define X86_TASK_CONTINUATION_PROBE_H

#include <linux/types.h>

struct bp_continuation_test;

int bp_task_continuation_probe_run(unsigned int owner_cpu,
				   unsigned int immediate_responder_cpu);
void bp_task_continuation_body(struct bp_continuation_test *test);
void bp_task_continuation_switch_stack(unsigned long stack_top,
				       struct bp_continuation_test *test);

#endif
