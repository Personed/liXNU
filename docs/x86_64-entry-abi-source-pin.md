# x86_64 XNU entry ABI source pin

## Status and scope

This is a source-level record for the native x86_64 hosted-XNU port. It pins the public XNU entry protocol used by the x86_64 machine layer so review does not infer a handoff from ARM64 code or from `LC_UNIXTHREAD` alone. It does not load, patch, or execute XNU and does not confirm that a future Tahoe artifact was built from this configuration.

Pinned input: public XNU tag `xnu-12377.121.6`, commit `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`. The relevant files are `osfmk/x86_64/start.s`, `osfmk/x86_64/boot_pt.c`, `osfmk/i386/i386_init.c`, `osfmk/i386/pmap.h`, `osfmk/i386/seg.h`, and `pexpert/pexpert/i386/boot.h` in that commit.

## BSP entry protocol

The BSP entry symbols are `_start` and `pstart` in `osfmk/x86_64/start.s`. They label the same entry sequence in the pinned source.

The source-declared entry environment is:

- 32-bit protected mode;
- paging disabled;
- flat code/data/stack segments with base zero and a 4 GiB limit;
- `%eax` contains the kernbootstruct pointer. Early startup copies it to `%edi` and later passes it to `vstart()` as the first argument. The 32-bit value is the boot-argument physical/linear address used before paging is enabled; the source does not pin the upper 32 bits of the 64-bit destination register when the CPU enters long mode, so the host/loader must establish that state explicitly in the implementation contract;
- the initial `%esp` is the low 32 bits of `low_eintstack`, a boot stack in the kernel's `__HIB` data area. The value is not supplied by the caller;
- the pinned source does not state the incoming interrupt-enable, NMI, or debug-register state as part of this entry comment. Those inputs remain unpinned and must not be invented by the loader.

The entry sequence then:

1. loads `protected_mode_gdtr`, whose entries come from XNU's own `master_gdt` copy in `__HIB`;
2. rebases the static zero-based `BootPML4` and `BootPDPT` entries by the low address of `BootPML4`;
3. enables PAE in CR4, long mode and NXE in EFER, loads CR3 with `BootPML4`, enables paging and write-protect in CR0, and far-jumps to `KERNEL64_CS`;
4. clears SS/DS/ES/FS/GS after the long-mode transition;
5. initializes the stack canary, raises the boot stack into the kernel high window, aligns RSP, zeroes RBP, and calls `_vstart` with `%edi` still carrying the boot-argument physical address.

`KERNEL64_CS` is selector `0x08`; `KERNEL64_SS` is `0x10`; `KERNEL32_CS` is `0x50`; `KERNEL_DS` is `0x68`. `KERNEL_PML4_INDEX` is 511. `KERNEL_BASE` is the negative top-of-address-space constant derived from `NBPML4 * KERNEL_PML4_COUNT` in `osfmk/i386/pmap.h`; use the build's generated `assym.s` values for the exact artifact rather than copying a numeric constant from this note.

## Bootstrap page tables and memory layout

`osfmk/x86_64/boot_pt.c` defines static bootstrap tables in `__HIB,__bootPT`. The comment requires these tables at the base of the kernel and specifically at the base of `__HIB`.

The pinned table shape is:

- `BootPML4[0]` and `BootPML4[KERNEL_PML4_INDEX]` both point at `BootPDPT` before the physical-base rebase;
- `BootPDPT[0..3]` point at four page-directory pages;
- `BootPTD` contains 2048 large-page entries, each mapping a 2 MiB page, for a 4 GiB identity range;
- because PML4 slot 0 and slot 511 initially use the same PDPT chain, the low identity mapping and the top kernel-slot mapping initially mirror the same low physical memory.

This means the entry is not a normal Linux long-mode kernel call. A host implementation either has to reproduce the expected protected-mode/paging-disabled entry environment, or patch/replace this entry sequence with an explicitly reviewed long-mode handoff. Both choices affect control-register ownership, the GDT/IDT, the kernel image's physical placement, interrupt masking, rollback, and the exact interpretation of `boot_args`. Neither is authorized by this document.

## Boot arguments consumed early

`pexpert/pexpert/i386/boot.h` defines a 4096-byte `boot_args`. Version 2 revision 0 is the base layout; revision 1 adds `KC_hdrs_vaddr` and the authenticated-root-volume fields. Early `vstart()` reads at least `Revision`, `Version`, `CommandLine`, the physical memory-map pointer/size/descriptor fields, `kaddr`, `ksize`, `efiRuntimeServices*`, `deviceTreeP`, `deviceTreeLength`, `kslide`, `PhysicalMemorySize`, and `KC_hdrs_vaddr` before or during platform and page-table initialization.

`vstart()` treats a nonzero first argument as the BSP boot-argument physical address, computes the initial physical-free point as `kaddr + ksize`, and later converts the argument pointer with `ml_static_ptovirt()`. A null first argument is the non-boot-CPU protocol. PR #108's validation-only `boot_args` layout work is relevant input, but it does not by itself establish the physical memory map, kernel placement, or handoff state required here.

The core placement preflight takes the contiguous physical reservation from the host. It requires disjoint boot arguments, EFI map, and ADT below the image and checks that the image and bootstrap budget fit in the reservation. The budget begins at the page-rounded image end, where `vstart()` starts allocating before it reads the EFI memory map. The placement check bounds the initial `fillkpt()` write by the selected build's `NKPT` capacity. It also checks the page table budget for the largest physmap randomization offset and the image's `__HIB` extent, within the table counts in `Idle_PTs_init()`. The loader must obtain `NKPT` and the `__HIB` size from the selected build, confirm this non-KASAN allocation sequence, and hold the reservation throughout XNU execution. Builds that move the allocation start before `Idle_PTs_init()` need their own placement calculation.

## AP entry protocol boundary

`slave_pstart` is the AP protected-mode entry. It explicitly clears `%edi`, uses `mp_slave_stack + PAGE_SIZE`, then joins the same long-mode transition and calls `vstart(0)`. It is reached through XNU's real-mode bootstrap installation path, not through the BSP contract. AP startup, ACPI topology, APIC ownership, IPI delivery, and Linux CPU hotplug interaction are separate design gates and are not pinned here.

## Consequences for the native port

- `LC_UNIXTHREAD.x86_THREAD_STATE64.RIP` remains insufficient evidence of the executable handoff. The actual artifact's symbols, Mach-O/fileset layout, link addresses, and build configuration must be compared with this source pin before any transition design is accepted.
- A correct implementation must pin whether it reproduces the documented protected-mode entry or uses a patched long-mode entry. Reproducing the original protocol requires a controlled CR0/CR4/EFER/GDT/CR3 transition away from Linux's long-mode environment. Patching the protocol requires an architecture-specific XNU patch and a new reviewed handoff contract.
- The FKI x86_64 context ABI must represent more than user-thread GPRs if it ever describes this transition: at minimum it must distinguish protected-mode and long-mode state and account for CR0, CR3/PCID, CR4, EFER, GDTR/IDTR, segment selectors/bases, RIP/RSP/RFLAGS, interrupt/preemption ownership, and enabled XSAVE state. That ABI is still undefined.
- Until the artifact, memory layout, exception ownership, and FKI context transaction are reviewed, `xnu.ko` must remain fail-closed and `fki_caps()` must remain zero on x86_64.

## Validation of this record

This document was checked against the pinned public source commit named above. It is not evidence that Tahoe 26.7.3 uses unchanged binary entry code, that a kernel collection has the required layout, or that XNU can execute under Linux. The next evidence needed is the successful Intel-native artifact hash/metadata required by `docs/x86_64-entry-return-contract.md`, plus a reviewed memory/control-register ownership design for the chosen entry strategy.
