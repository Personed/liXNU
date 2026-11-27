/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef X86_EXCEPTION_RECOVERY_ASM_H
#define X86_EXCEPTION_RECOVERY_ASM_H

#define ER_ARM_TOKEN                 0x45584350524f4245
#define ER_GP_MSR_INDEX              0x80e
#define ER_SENTINEL_R11              0x4a11f00d4a11f00d
#define ER_SENTINEL_R12              0x4a12f00d4a12f00d
#define ER_TEST_STACK_TOP_OFFSET      16368
#define ER_TEST_STACK_HIGH_CANARY     16376

#define ER_OFF_ARMED                 0
#define ER_OFF_EXPECTED_VECTOR       8
#define ER_OFF_EXPECTED_RIP          16
#define ER_OFF_RECOVERY_RIP          24
#define ER_OFF_ENVELOPE_RSP          32
#define ER_OFF_SAVED_RBX             40
#define ER_OFF_SAVED_RBP             48
#define ER_OFF_SAVED_R12             56
#define ER_OFF_SAVED_R13             64
#define ER_OFF_SAVED_R14             72
#define ER_OFF_SAVED_R15             80
#define ER_OFF_FAULT_WINDOW_RSP      88
#define ER_OFF_FAULT_STACK_BASE      96
#define ER_OFF_FAULT_STACK_TOP       104
#define ER_OFF_RECOVERY_STACK_BASE   112
#define ER_OFF_RECOVERY_STACK_TOP    120
#define ER_OFF_RECOVERY_COUNT        128
#define ER_OFF_CAPTURED_VECTOR       136
#define ER_OFF_ERROR_CODE            144
#define ER_OFF_CR2                   152
#define ER_OFF_FAULT_RIP             160
#define ER_OFF_FAULT_CS              168
#define ER_OFF_FAULT_RFLAGS          176
#define ER_OFF_FAULT_RSP             184
#define ER_OFF_FAULT_SS              192
#define ER_OFF_ORIGINAL_IDTR         200
#define ER_OFF_VERIFY_IDTR           216
#define ER_OFF_SHADOW_IDTR           232
#define ER_OFF_IDTR_PROOF            248
#define ER_OFF_TERMINAL_CODE         256
#define ER_OFF_RESULT_FLAGS          264
#define ER_OFF_FAULT_ADDRESS         272
#define ER_OFF_NO_FAULT              280
#define ER_OFF_RESULT_VECTOR         288
#define ER_OFF_RESULT_ERROR_CODE     296
#define ER_OFF_RESULT_CR2            304
#define ER_OFF_RESULT_RIP            312
#define ER_OFF_RESULT_CS             320
#define ER_OFF_RESULT_RFLAGS         328
#define ER_OFF_RESULT_RSP            336
#define ER_OFF_RESULT_SS             344

#define ER_IDTR_LIMIT_OFF             0
#define ER_IDTR_BASE_OFF              2

#define ER_SCRATCH_RAX                0
#define ER_SCRATCH_RBX                8
#define ER_SCRATCH_RCX               16
#define ER_SCRATCH_RDX               24
#define ER_SCRATCH_RSI               32
#define ER_SCRATCH_RDI               40
#define ER_SCRATCH_RBP               48
#define ER_SCRATCH_R8                56
#define ER_SCRATCH_R9                64
#define ER_SCRATCH_R10               72
#define ER_SCRATCH_R11               80
#define ER_SCRATCH_R12               88
#define ER_SCRATCH_R13               96
#define ER_SCRATCH_R14              104
#define ER_SCRATCH_R15              112
#define ER_SCRATCH_FLAGS            120
#define ER_SCRATCH_ENVELOPE_RSP     128

#endif
