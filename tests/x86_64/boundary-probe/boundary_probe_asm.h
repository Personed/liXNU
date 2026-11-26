/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef X86_BOUNDARY_PROBE_ASM_H
#define X86_BOUNDARY_PROBE_ASM_H

#define BP_OFF_HOST_ENTRY_RSP       0
#define BP_OFF_HOST_RETURN_RSP      8
#define BP_OFF_ALT_SWITCH_RSP      16
#define BP_OFF_ALT_ENTRY_RSP       24
#define BP_OFF_ALT_RETURN_RSP      32
#define BP_OFF_RFLAGS_ENTRY        40
#define BP_OFF_RFLAGS_DURING       48
#define BP_OFF_RFLAGS_INNER_RETURN 56
#define BP_OFF_RFLAGS_RETURN       64
#define BP_OFF_STATUS              72
#define BP_OFF_GPRS_ENTRY          80
#define BP_OFF_GPRS_DURING        128
#define BP_OFF_GPRS_RETURN        176

#define BP_GPR_RBP                  0
#define BP_GPR_RBX                  8
#define BP_GPR_R12                 16
#define BP_GPR_R13                 24
#define BP_GPR_R14                 32
#define BP_GPR_R15                 40

#endif
