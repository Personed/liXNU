# SPDX-License-Identifier: GPL-2.0-only
obj-m += x86-boundary-probe.o
x86-boundary-probe-y := boundary_probe.o boundary_probe_asm.o xstate_clobber.o \
	task_continuation_probe.o task_continuation_probe_asm.o
ccflags-y += -I$(src)/../../../fki-ko
