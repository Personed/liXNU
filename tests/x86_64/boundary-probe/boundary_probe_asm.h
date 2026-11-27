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

#define BP_MODE_EXCEPTION_PF        2
#define BP_MODE_EXCEPTION_GP        3

#define BPX_OFF_MAGIC               0
#define BPX_OFF_STATE               8
#define BPX_OFF_EXPECTED_VECTOR    16
#define BPX_OFF_EXPECTED_RIP       24
#define BPX_OFF_POST_INJECTION_RIP 32
#define BPX_OFF_RECOVERY_STACK_TOP 40
#define BPX_OFF_RECOVERY_LOW_CANARY_PTR 48
#define BPX_OFF_RECOVERY_HIGH_CANARY_PTR 56
#define BPX_OFF_ENVELOPE_RSP       64
#define BPX_OFF_FINAL_RSP          72
#define BPX_OFF_ORIGINAL_DESC      80
#define BPX_OFF_SHADOW_DESC        90
#define BPX_OFF_READBACK_DESC     100
#define BPX_OFF_TERMINAL_CODE     112
#define BPX_OFF_RECOVERY_COUNT    120
#define BPX_OFF_FAULT_VECTOR      128
#define BPX_OFF_FAULT_ERROR       136
#define BPX_OFF_FAULT_RIP         144
#define BPX_OFF_FAULT_RFLAGS      152
#define BPX_OFF_FAULT_RSP         160
#define BPX_OFF_FAULT_CS          168
#define BPX_OFF_FAULT_SS          176
#define BPX_OFF_FAULT_CR2         184
#define BPX_OFF_PROOF_FLAGS       192
#define BPX_OFF_FAULT_SCRATCH_RAX 200
#define BPX_OFF_FAULT_SCRATCH_RBX 208
#define BPX_OFF_FAULT_SCRATCH_RCX 216
#define BPX_OFF_FAULT_SCRATCH_RDX 224
#define BPX_OFF_REC_SCRATCH_RAX   232
#define BPX_OFF_REC_SCRATCH_RBX   240
#define BPX_OFF_REC_SCRATCH_RCX   248
#define BPX_OFF_REC_SCRATCH_RDX   256
#define BPX_SIZE                  264

#define BPX_MAGIC                 0x4250584552434f4eULL
#define BPX_STATE_ARMED             1
#define BPX_STATE_DISARMED          0
#define BPX_PROOF_CANARIES          1
#define BPX_PROOF_IDTR              2
#define BPX_PROOF_RSP               4

#define BPX_RECOVERY_LOW_CANARY  0x4c4f5743414e4152ULL
#define BPX_RECOVERY_HIGH_CANARY 0x4849474843414e52ULL

#endif
