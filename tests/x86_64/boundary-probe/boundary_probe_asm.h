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
#define BP_OFF_COOP_GENERATION    224
#define BP_OFF_COOP_TOKEN         232
#define BP_OFF_COOP_REPLY         240
#define BP_OFF_COOP_RESUME_COUNT  248
#define BP_OFF_COOP_STATE         256
#define BP_OFF_COOP_RESUME_RSP    264
#define BP_OFF_STOP_RIP           272
#define BP_OFF_STOP_RSP           280

#define BP_COOP_MODE_STOP           2
#define BP_COOP_MODE_RESUME         3
#define BP_COOP_STATE_PENDING       1
#define BP_COOP_STATE_REPLYING      2
#define BP_COOP_STATE_REPLIED       3
#define BP_COOP_STATE_RESUMED       5
#define BP_COOP_STATE_CANCELLED     6
#define BP_COOP_TOKEN_XOR 0x584e55434f4f5031
#define BP_COOP_REPLY_XOR 0x52504c59434f4f31

#define BP_GPR_RBP                  0
#define BP_GPR_RBX                  8
#define BP_GPR_R12                 16
#define BP_GPR_R13                 24
#define BP_GPR_R14                 32
#define BP_GPR_R15                 40

#endif
