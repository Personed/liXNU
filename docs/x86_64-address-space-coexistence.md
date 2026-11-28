# x86_64 address-space coexistence evidence

## Status and scope

This is the final validation-only micro-table in the entry-architecture decomposition chain. It compares pinned XNU and Linux x86_64 address-space layout facts and records proof obligations for any future pmap/CR3 composition. It does not select a route, prove viability or production safety, authorize a patch, define an ABI/handoff contract, or authorize entry. `XNU_PREBOOT_LONG` remains rejected/unmodeled; `_start+0x5c` remains only a structural anchor.

The helper `fki-ko/fki_x86_64_address_space_coexistence.h` exposes the same table to userspace fixtures. Classifications are deliberately negative: disjoint under current evidence, potential overlap, layout conflict unresolved, or hard/unresolved proof obligation. Nothing is accepted or resolved.

## Source facts

Pinned XNU `xnu-12377.121.6` facts:

- `VM_MIN_KERNEL_ADDRESS` is `0xffffff8000000000` and `VM_MAX_KERNEL_ADDRESS` is `0xffffffffffffefff` (`osfmk/mach/i386/vm_param.h`). The kernel uses PML4 index 511; `KERNEL_BASEMENT` is one PML4 slot below it, index 510 (`osfmk/i386/pmap.h`).
- The physmap uses dynamically computed PML4 indexes ending below the basement: `kernPhysPML4Index = 510 - kernPhysPML4EntryCount`; each index covers 512 GiB and the maximum count is 14 (`osfmk/i386/i386_init.c`, `pmap.h`).
- `KERNEL_DBLMAP_PML4_INDEX` is 493 and KASAN indexes are 494-495 in XNU's layout (`pmap.h`). XNU copies kernel, basement, physmap, dblmap, and conditional KASAN high PML4 entries into host pmaps (`osfmk/x86_64/pmap.c`).

Linux v6.8 documented facts:

- `Documentation/arch/x86/x86_64/mm.rst` documents index-510 space as `%esp` fixup stacks and places EFI, kernel text, modules, fixmap, and legacy vsyscall high in index 511. It marks the lower part of index 511 around `0xffffff8000000000` as an unused hole in the current Linux layout.
- The same document places KASAN shadow memory across lower high-memory PML4 indexes; depending on size, that documented range covers indexes overlapping XNU's 493-495 region.
- `arch/x86/entry/entry_64.S` switches to the kernel CR3 during syscall/exception entry under KPTI. The CPU entry area and entry mappings must therefore exist in the relevant user/transition page tables, independently of XNU's pmap design.
- `CONFIG_X86_ESPFIX64` is `def_bool y` when `X86_16BIT && X86_64`; whether the reservation is actively needed in a target configuration still requires verification, so the index-510 overlap is evidence of unresolved layout conflict, not proof of absolute impossibility.

## Matrix

| Item | Classification | Bounded interpretation |
| --- | --- | --- |
| XNU low kernel load range vs Linux index-511 unused hole | disjoint under current evidence | The initial XNU kernel area begins in a range Linux currently documents as unused. This is only a fact about that low range, not evidence that the full XNU kernel virtual range composes with Linux. |
| XNU full kernel virtual range vs Linux high index-511 reservations | layout conflict unresolved | XNU's maximum kernel range extends through index 511, where Linux reserves EFI, module, kernel text, fixmap, and legacy-vsyscall space. Actual allocation overlap is not proven; ownership/composition of the overlapping span is unresolved. |
| XNU `KERNEL_BASEMENT` vs Linux `%esp` fixup index 510 | layout conflict unresolved | Both layouts use/reserve PML4 index 510 by construction. Because espfixup usage may depend on configuration and legacy 16-bit support, this is unresolved evidence rather than absolute impossibility. |
| Small-memory XNU physmap vs neighboring Linux high mappings | disjoint under current evidence | With one physmap PML4 index, XNU uses index 509, while the documented Linux neighbors are indexes 508, 510, and 511. This is memory-size-dependent and not a general layout contract. |
| Multi-entry XNU physmap vs Linux CPU entry area | potential overlap | If enough physical memory plus XNU's 4 GiB MMIO allowance requires more than one 512 GiB physmap index, the physmap can reach index 508, documented by Linux as CPU-entry-area space. This is conditional potential overlap, not a universal hard blocker. |
| XNU DBLMAP/KASAN 493-495 vs Linux KASAN range | potential overlap | XNU's DBLMAP index 493 and KASAN indexes 494-495 lie within the broad KASAN shadow range documented for Linux. Linux KASAN is configuration-dependent, so this is conditional overlap evidence. |
| KPTI user-CR3 entry mappings | hard/unresolved proof obligation | Any future composition must preserve the Linux syscall/exception entry mappings and CR3 transition rules in user/transition page tables. XNU copying its own high PML4 entries is not sufficient evidence of Linux entry safety. |
| Full kernel CR3 composition | hard/unresolved proof obligation | Linux and XNU cannot simply use two independent kernel halves as on ARM64. A future design must define which CR3 contents exist during Linux entry, XNU execution, upcalls, faults, and rollback. |
| PCID/TLB namespace | hard/unresolved proof obligation | Linux PCID/KPTI semantics and XNU PCID/TLB invalidation state cannot share a CPU without a namespace and invalidation contract. No such protocol exists. |

## Maximum conclusion

There is no automatic composition of Linux and XNU x86 address-space layouts. Some low/small-memory ranges are disjoint under current evidence, but index 510 is a layout conflict unresolved by construction, index 511 has unresolved overlap across the full XNU span, and larger physmap/KASAN cases introduce additional conditional overlaps. KPTI user CR3, full-kernel CR3 composition, and PCID/TLB semantics remain hard/unresolved proof obligations.

Any pmap, layout, memory or CR3 solution remains required-but-undesigned or unresolved. This document authorizes no runtime, XNU execution, loader, patch, entry, CPU/hotplug operation, real MSR/IDT access, userland, ABI/layout change, transaction change, `fki_caps`, VM harness change, real ownership, or `#216` work.
