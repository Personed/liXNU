# x86_64 runtime on QEMU/KVM

**Status:** Implementation direction selected to unblock PC support. ARM64 remains on the existing direct XNU-in-Linux path.

## Intent

Run a user-provided, bootable macOS Tahoe 26 x86_64 disk on Intel and AMD Linux PCs, while preserving the existing ARM64/macOS 27 runtime. On x86_64, QEMU/KVM supplies the machine boundary and executes guest instructions on the host CPU. On ARM64, `fki.ko` and `xnu.ko` continue to run directly in Linux as they do today.

The user asked that technical blockers be resolved autonomously and the work continue. This design records the resulting implementation call; it is not a claim that the x86_64 runtime is complete.

## Decisions made to close design questions

- On x86_64, project compatibility means the project launcher starts a macOS Tahoe guest through QEMU/KVM. The current `xnu.ko` and `fki.ko` are not built or loaded on x86_64. This is an intentional runtime change so the guest has hardware-enforced CPU, memory, exception, and syscall isolation.
- The launcher requires a user-prepared, bootable Tahoe 26 x86_64 disk whose EFI partition contains a working OpenCore/EFI boot setup. The project will not install Apple software or mutate a source disk in its first x86 milestone. It accepts raw or qcow2 images, creates a persistent qcow2 overlay backed by the source disk, and keeps writable OVMF variables in a separate per-VM state file.
- Support for Intel and AMD remains a separate acceptance claim for each vendor. KVM uses QEMU's `host` CPU passthrough by default so the guest sees the actual Intel or AMD model and supported features; AMD disks must already include the appropriate OpenCore AMD kernel patches. Actual compatibility is not claimed until it boots and passes host-integrity checks on physical hardware from that vendor.

## Approaches considered

1. **Extend the current same-kernel module to x86_64.** This would preserve the ARM hosting model on PCs, but the x86 FKI has no world backend, `xnu_main.c` and `world.S` are AArch64-specific, and no safe host exception, syscall, or address-space boundary has been demonstrated. Do not build a backend from no-op stubs.
2. **QEMU with KVM acceleration on x86_64 — selected.** This isolates guest page tables, exceptions, and syscalls while running x86 instructions directly on Intel VT-x or AMD-V. It preserves the existing ARM path. The trade-off is that the x86 runtime boots a VM rather than loading `xnu.ko` into Linux; it needs QEMU, KVM, UEFI firmware, Apple SMC emulation, and a user-provided bootable Tahoe disk.
3. **QEMU TCG on x86_64.** This needs no hardware virtualization and is useful for automated smoke tests, but it translates guest instructions and is too slow to claim PC runtime support. Keep it as an explicit development mode only.

## Targets and boundaries

| Host | Hosted system | Execution path | Claim required |
|---|---|---|---|
| ARM64 Linux, 4 KiB pages and PAC | macOS 27 ARM64 | Existing `fki.ko` / `xnu.ko` modules | Existing module builds plus a repeatable desktop smoke run |
| Intel x86_64 Linux with VT-x/KVM and the required x86_64h features | macOS Tahoe 26 x86_64 | QEMU/KVM guest | Boot, desktop, host integrity and shutdown on physical Intel hardware |
| AMD x86_64 Linux with AMD-V/KVM and the required x86_64h features | macOS Tahoe 26 x86_64 | QEMU/KVM guest with OpenCore AMD kernel patches | Same checks on physical AMD hardware |

The x86_64 launch profile uses QEMU's Q35 machine, OVMF UEFI, Apple SMC device, AHCI storage, an Intel E1000 network device, USB keyboard/tablet, and a basic VGA display. A prepared disk must contain a bootable EFI/OpenCore setup compatible with Tahoe 26 x86_64. Guest writes go to a persistent qcow2 overlay in the VM state directory; the source disk is opened as the immutable backing image. A sidecar stores the source path identity, format, metadata and SHA-256; if file metadata changes, identical bytes are accepted and changed bytes are rejected before QEMU starts. Hashing reads the entire source image at initialization or after metadata changes, then skips the read while metadata remains stable. The SMC key is supplied locally, written to a mode-0600 temporary QEMU config, excluded from QEMU's argv and environment, and removed at exit. It is never stored in the repository or printed by the launcher. KVM is required for a support claim; TCG is opt-in for test and development.

The default KVM guest CPU profile is QEMU `host` passthrough; TCG defaults to `max`. A user may override it with `CPU_MODEL`. The host must provide the x86_64h feature subset listed in Apple XNU `osfmk/i386/cpuid.h`: SSE4.2, AVX, FMA, MOVBE, POPCNT, LZCNT, BMI1, AVX2 and BMI2. A missing host feature is reported before QEMU starts. AMD Tahoe images need AMD OpenCore kernel patches, and AMD support remains experimental until validated on physical hardware.

Initial display support is software framebuffer output through QEMU's display backend. Hardware GPU acceleration, passthrough, and reuse of the ARM host-call kexts are outside the first x86 milestone. Guest disk, network, USB input, and display use QEMU virtual devices and macOS guest drivers.

The x86_64 host path does not build or load the current ARM-only `xnu.ko`/`fki.ko`. It runs a user-provided x86_64 macOS guest through QEMU/KVM. The launcher serializes access to a VM state directory so two QEMU processes cannot write the same OVMF variables and disk overlay at once. The ARM module path and its CI remain intact. The feature matrix must describe these two execution paths accurately.

## Implementation outline

- Add an x86_64 launcher that validates QEMU and qemu-img, `/dev/kvm` for KVM mode, OVMF firmware, the boot disk and CPU features before starting the guest; it writes to a persistent qcow2 overlay to leave the source image untouched.
- Keep KVM as the default. Allow `ACCEL=tcg` explicitly for development; report clearly that TCG is not a supported-performance mode.
- Keep VM state and writable OVMF variables outside the repository. Pass the system image as read/write only when explicitly configured; support raw and qcow2 formats.
- Add command-construction checks using a fake QEMU executable, plus a QEMU TCG smoke boot of a disposable Linux guest. Neither substitutes for a Tahoe boot.
- Update README and host requirements to show ARM64 and x86_64 execution paths separately, with evidence-gated support status.
- Verify Tahoe 26 on physical Intel and AMD PCs, and rerun the ARM64 desktop smoke procedure before claiming the combined goal complete.

## Current evidence and limits

- The direct x86 FKI module fails modpost due to six missing architecture hooks, and `xnu_main.c` fails assembly on AArch64 instructions.
- Apple XNU `xnu-12377.121.6` includes `osfmk/i386` and `config/MASTER.x86_64`.
- QEMU 10.0.13 in the disposable environment provides Q35, OVMF 4 MiB firmware, `isa-applesmc`, AHCI, E1000, USB input and VGA devices. A Debian 6.12.118 x86_64 guest booted under TCG.
- This environment has no `/dev/kvm` and no Tahoe disk. It can validate the launcher and TCG harness but cannot validate native KVM acceleration or macOS boot.
- ARM64 `fki.ko` and `xnu.ko` cross-compile successfully against Debian 6.12.118 ARM64 headers; an ARM64 runtime smoke test was not available here.
