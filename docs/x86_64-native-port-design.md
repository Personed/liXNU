# Native x86_64 hosted-XNU port

## Target and current boundary

The active target is to run the latest Intel-compatible macOS (Tahoe 26) through a native x86_64 FKI/XNU path on Linux PCs, while retaining the existing direct ARM64 path for macOS 27. Intel and AMD are separate acceptance profiles. The QEMU/KVM macOS guest launcher is useful separately; its results do not validate this native port.

PR #48 adds the first x86_64 Linux-side FKI build hooks, with `fki_caps() == 0`. PR #47 adds an x86_64 `xnu.ko` target that refuses to load. Neither PR executes XNU. Keep both gates fail-closed until the architecture boundary below is implemented and verified.

The pinned XNU source tag used by the current build scripts contains `osfmk/i386` and `mach-o/x86_64` sources, but this repository's LINUX64 machine patches and loader target ARM64. An x86_64 Tahoe kernel collection and matching entry ABI must be built and checked; the presence of i386 sources alone does not prove that target.

## Concrete architecture dependencies

| Area | Current ARM64 dependency | Work needed for x86_64 |
| --- | --- | --- |
| XNU loader and ABI | `xnu_main.c` looks up `_arm_init`, `_linux64_hook`, and ARM64 LINUX64 symbols; `linux64_abi.h` uses AArch64 registers and is copied into `osfmk/arm64`. | Define the x86_64 XNU entry point, boot data, and host-call ABI. Use the x86_64 SysV/kernel calling conventions intentionally, with an ABI version and symbol checks. |
| World switch | `world.S` uses AArch64 GPRs, DAIF, TPIDR, TTBR0/TCR, and ELR/SPSR/ERET; `struct xnu_world` has those register layouts. | Add a distinct x86_64 per-CPU save area and entry/return assembly. Specify and test stack, GPR, RFLAGS, FS/GS base, CR3/PCID, FPU/XSAVE, preemption, and interrupt state. Preserve Linux's current task and per-CPU state on every exit path. |
| Privileged host state | `fki_enter/leave` switches ARM pointer-authentication keys and IRQ/preemption state; `xnu_init` currently requires `FKI_CAP_PAC`. | Split generic execution/context capabilities from the optional ARM PAC capability. Do not report PAC on x86 or add a no-op PAC backend. The x86 XNU path must establish its own supported security and context contract before enabling execution. |
| Memory and translation | The ARM64 path allocates a physically contiguous region, shares TTBR1, switches TTBR0/TCR, installs ARM table entries, and flushes the ARM TLB. | Specify the x86_64 kernel/user page-table model, Linux mappings needed by host callbacks, isolation of XNU mappings, physical memory accounting, TLB/PCID invalidation, and kernel/user boundary. |
| Exceptions and faults | FKI probes ARM64 Linux fault handlers and converts FAR/ESR into an XNU fault trampoline that restores registers and returns with ERET. | Design x86 #PF and other exception delivery, fault-address/error-code/register mapping, recoverable XNU faults, fatal return to Linux, and nested-fault behavior. Linux IDT/IRQ ownership must remain valid; no global vector replacement without a proved per-CPU protocol. |
| Boot and CPUs | `boot_args_v2`, device tree, MPIDR slots, ARM CPU entry, and LINUX64 AP hooks are ARM64-specific. | Build x86_64 boot arguments and memory map from the target XNU/i386 interfaces; integrate ACPI/topology, AP startup, APIC/IPIs, timers, and stable Linux-to-XNU CPU mapping. |
| User execution | XNU's ARM64 LINUX64 layer defines EL0, PAC operations, ARM syscall/fault state, and ARM FPU structures. | Port x86_64 XNU user entry/return and syscall/exception handling, per-thread FS/GS state, and the enabled XSAVE state. Do not reuse ARM UTCB register or PAC layouts. |
| XNU source patches | All current `xnu-patches/*.patch` machine-layer changes target `osfmk/arm64`; the build flow prepares an ARM64 kernel and ARM64 collection. | Add a separate x86_64/i386 machine layer and patch set for Tahoe's x86_64 kernel; keep the ARM64 patch application and build unchanged. |

## Implementation order and acceptance gates

1. **Pin the x86 build input.** Select the exact Tahoe-compatible XNU source/configuration, toolchain, and symbol-bearing x86_64 kernel collection. Record the architecture and source revision in CI artifacts. A successful ARM64 build or generic Mach-O parse is not sufficient.
2. **Keep architecture selection explicit.** Build separate ARM64 and x86_64 module object lists and machine-layer patches. Make FKI capabilities describe generic context execution separately from PAC and other optional features. Keep the x86 module fail-closed until all later gates pass.
3. **Prove host-state entry/return in isolation.** Add a disposable x86_64 test for the assembly boundary before entering XNU. Exercise normal return and abort paths, stack alignment, saved registers/flags, Linux FS/GS and page-table state, IRQ/preemption state, and enabled XSAVE state. Verify the Linux task resumes correctly. Test both Intel and AMD CPU profiles; QEMU/KVM can provide early repeatable checks but not final hardware acceptance.
4. **Prove memory and exception routing.** Add a bounded XNU address-space test and verify page-table isolation, TLB/PCID behavior, Linux host mappings, #PF recovery, and fatal-fault return. Test SMP and CPU hotplug only after the one-CPU case is stable.
5. **Boot XNU with one CPU.** Supply x86 boot arguments and host calls, reach the firmware/console boundary, then a PureDarwin shell and clean halt. Record each stage separately; reaching the XNU entry is not a PureDarwin boot.
6. **Validate the Tahoe desktop on hardware.** Boot a user-provided x86_64 Tahoe disk through the native module path on physical Intel and AMD Linux PCs separately. Record CPU model/features, Linux kernel/config, RAM, source and disk hashes, display/input/network/disk results, clean shutdown, and Linux host-integrity checks. Keep the ARM64 macOS 27 build and runtime smoke as a regression requirement.

A gate passes only with its named evidence. Until the native XNU boot and host-integrity gates pass, describe x86_64 as in development and keep `fki_caps()` at zero. No Apple macOS image, firmware, or locally supplied SMC key belongs in this repository or its logs.
