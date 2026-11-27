# SPDX-License-Identifier: APSL-2.0
# xnu.ko: the APSL side. Out-of-tree, non-GPL module that only uses fki.h.
obj-m += xnu.o
ifeq ($(CONFIG_ARM64),y)
xnu-y := xnu_main.o macho.o adt.o world.o
else ifeq ($(CONFIG_X86_64),y)
# The x86_64 module refuses to load until its native world switch is implemented.
xnu-y := xnu_x86_64_stub.o x86_64_cpu_compat.o
endif
ccflags-y += -Wno-declaration-after-statement
