# Native x86_64 support

**Status:** Active design for the native x86_64 port. No x86_64 hosted-XNU runtime has passed end-to-end validation yet.

## Target platforms

| Linux host | Hosted system | Execution path | Current evidence | Evidence required for this port |
| --- | --- | --- | --- | --- |
| ARM64 with PAC and 4 KiB Linux pages | Existing macOS 27 ARM64 setup | Existing direct FKI/XNU modules | The README reports desktop boots on Apple Silicon and DGX Spark. CI builds the ARM64 Linux modules. | Keep CI green and repeat a documented desktop smoke run on a supported ARM64 setup. |
| Intel x86_64 | macOS Tahoe 26 x86_64 | Native FKI/XNU port on Linux | The FKI host-backend build merged in [#48](https://github.com/Personed/xnulinux/pull/48) passes; it advertises no XNU execution capability, and the fail-closed XNU stub merged in [#47](https://github.com/Personed/xnulinux/pull/47) does not execute XNU. | Build, boot, and pass host-integrity checks to the desktop on a documented Intel configuration. |
| AMD x86_64 | macOS Tahoe 26 x86_64 | Native FKI/XNU port on Linux | The FKI host-backend build merged in [#48](https://github.com/Personed/xnulinux/pull/48) passes; it advertises no XNU execution capability, and the fail-closed XNU stub merged in [#47](https://github.com/Personed/xnulinux/pull/47) does not execute XNU. | Build, boot, and pass host-integrity checks to the desktop on a documented AMD configuration. |

The user selected native x86_64 execution and the most recent macOS release with Intel binaries. Apple identifies Tahoe 26 as the final release supporting Intel Macs. The repository currently builds XNU `xnu-12377.121.6`; Apple’s public tag contains the `osfmk/i386` machine sources and `config/MASTER.x86_64`, so it is a concrete candidate for the Intel port. Its match to a supplied Tahoe system build, kernel collection and kext set still needs verification. Apple artifacts are supplied by the user. [Apple's macOS 26.4 release notes](https://developer.apple.com/documentation/macos-release-notes/macos-26_4-release-notes?changes=l_9)

The first x86_64 milestone targets Linux hosts and a usable desktop with software rendering. Hardware GPU acceleration can follow once the desktop works. Each validated host configuration defines a concrete support claim. The CPU feature floor and additional configurations remain open until measured.

## Execution model

Run x86_64 XNU and its userspace directly on Intel/AMD x86_64 Linux through architecture-matched project modules, following the existing ARM64 hosting model. A disposable Linux VM may contain early bring-up experiments; target acceptance runs on physical Intel and AMD systems.

The user selected a native x86_64 port. A QEMU/KVM macOS guest, a compile-only workflow, or a QEMU TCG ARM64 guest can provide complementary evidence but cannot substitute for the requested runtime. Keep those tools available as separate experiments.

## What needs to change

- `fki-ko/Makefile` selects the ARM64 or L4H backend. The existing interface mixes shared Linux services with ARM-specific state such as PAC keys, TTBR1 mappings and ARM user register frames. Keep reusable services on the existing FKI boundary and give x86_64 its own CPU context and capabilities.
- `xnu-ko/xnu_main.c` expects `_arm_init` and ARM boot arguments. `xnu-ko/world.S` implements the AArch64 context switch. The x86_64 path needs its own entry point, boot handoff, context switch and exception and syscall delivery. Its XNU machine patches should target the pinned Tahoe source independently of the ARM patch series.
- `xnu-ko/macho.c` reads the 64-bit Mach-O magic but interprets `LC_UNIXTHREAD` as an ARM register frame. `scripts/kclink/kextld.py` explicitly accepts ARM64 objects and emits ARM64 kexts. Select the correct CPU type, subtype, thread state and relocation and fixup formats for each architecture. Validate both the outer collection and its embedded kernel before mapping it for execution.
- Give each architecture its own kernel, patch set, kext and collection outputs. Check architecture and ABI version before entering XNU. Record the source revision and system build for each collection. Reuse the existing disk, console, network, input and display interfaces where the matching Tahoe kexts and userspace can use them.

Apple publishes XNU sources for both architectures. The feasibility work must confirm that the matching Tahoe tag builds with the available toolchain and produces the symbols and formats required by the Intel collection. [Apple XNU source tree](https://github.com/apple-oss-distributions/xnu)

## Milestones and exit criteria

1. **Prove the execution boundary.** Use a small, self-contained x86_64 payload in a disposable Linux VM. Enter and return repeatedly, exercise a recoverable page fault and a stop, and show that Linux retains its register, floating-point and memory state across the run. Demonstrate that Linux’s own exceptions and syscalls continue to work during and after the run. Establish an approach for x86_64 page tables, interrupt and syscall entry, and CPU state restoration. If a boundary check fails, refine the architecture-specific design and repeat the check; do not enter XNU until host-state integrity passes.
2. **Build one matched Tahoe target.** Pin the XNU tag, toolchain, host kernel headers and Tahoe system build. Produce an x86_64 kernel and collection whose architecture, entry layout and symbols agree with the loader. Validate rejection of mismatched architecture and ABI using small fixtures. Keep the existing ARM64 module build running in CI.
3. **Boot a minimal system.** Reach the serial console with one CPU and enter a minimal x86_64 userspace. Verify that it can make a syscall, recover a page fault, use disk I/O and shut down. Then validate timers, multiple CPUs, IPIs, page table changes and TLB invalidation. Record kernel logs and repeat the Linux host state checks with the hosted system running.
4. **Verify desktop compatibility.** Boot Tahoe 26 to the desktop on one physical Intel host and one physical AMD host. Verify disk I/O, keyboard and pointer input, networking, a visible desktop with software rendering and clean shutdown on each. Rebuild and run the macOS 27 desktop smoke check on a documented ARM64 host. Verify each platform independently before documenting it as supported.
5. **Make results reproducible.** Add module builds for both architectures to public CI. Keep checks that require Apple artifacts in a local test procedure. Record exact CPU model and required features, host kernel version and configuration, memory and page size, hosted system build, and how each build and boot result was obtained.

## Evidence available today

Public CI builds the ARM64 Linux modules. The existing `tests/host/run.sh` checks Linux FP/SIMD state and memory while a hosted system is running. It does not check macOS desktop boot. In this workspace, the complete ARM64 `fki.ko` and `xnu.ko` modules also cross-compiled against Debian 6.12.118 ARM64 headers with GCC 14; no ARM64 runtime smoke test was available. The current workspace is Linux x86_64 and does not provide `/dev/kvm`. A Debian 6.12.118 x86_64 Linux guest does boot under QEMU TCG, but that only validates the disposable test harness.

The Apple upstream XNU tag used by this repository, `xnu-12377.121.6`, includes an x86_64 machine configuration and i386 machine source tree. The project’s custom execution layer is still ARM-only: the x86_64 FKI build reaches modpost and fails because no x86 backend implements six architecture hooks; compiling `xnu_main.c` reaches the assembler and fails on ARM-specific register, atomic, PAC, cache, and barrier instructions; the world switch has only an AArch64 implementation. No x86_64 module or XNU code has run.

These results do not yet demonstrate that a same-kernel x86_64 port can safely preserve Linux exception, syscall, and address-space state. The execution boundary is the first implementation milestone: continue refining and testing it before running XNU. A QEMU/KVM guest is an available complementary experiment, but it does not satisfy the selected native x86_64 target. Physical Intel, AMD and ARM64 acceptance runs and matching system images are still required.
