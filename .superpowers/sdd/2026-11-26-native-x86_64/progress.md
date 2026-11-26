# SDD ledger — plan: docs/superpowers/plans/2026-11-26-native-x86_64.md

> Historical ledger for the superseded direct-module approach. The active implementation is tracked in `.superpowers/sdd/2026-11-26-x86_64-qemu-kvm-runtime/progress.md`.

Pre-flight: Task 1 produces the FKI execution context consumed by Task 2. Task 3 produces the x86_64 kernel and boot ABI consumed by Tasks 2 and 4. Task 4 validates and emits collections consumed by Task 3 and Task 5. Task 5 consumes the architecture and collection outputs from Tasks 3–4. Task 6 verifies both architecture paths and their system smoke procedures.

Task 1: closed after the safety findings below. Tasks 2–6 do not apply to the superseded direct-module design.

Task 1: Finding — x86_64 Linux 6.12.118 headers compile shared FKI objects until `fki_hostcall.c` uses ARM-oriented `page_to_phys`, which does not exist on x86. Replacing it with `PFN_PHYS(page_to_pfn(pg))` advances compilation to modpost, which reports missing architecture backend symbols (`fki_counter`, `fki_arch_linux_runs`, `fki_fpsimd_forget`, `fki_counter_frequency`, `fki_arch_init`, `fki_arch_exit`). The source has no x86_64 backend yet.

Task 1: Environment — QEMU 10.0.13, Debian 6.12.118 x86_64 image and headers were extracted under `/workspace/scratch`. The host has no `/dev/kvm`, no module headers for its running kernel, and no root privileges. QEMU TCG can host a test VM. This lets us load modules in a guest but cannot validate the current host kernel directly.

Task 1: Finding — The disposable QEMU TCG guest boots Debian 6.12.118 x86_64 to a BusyBox shell. This validates only the VM harness. The FKI module still fails modpost, so no project module has been loaded in the guest.

Task 1: Finding — Compiling `xnu-ko/xnu_main.c` for x86_64 reaches the assembler and fails on AArch64-only operations (`mrs` system registers, `ldaxr`/`stlxr`, PAC instructions, and ARM cache/barrier instructions). The world switch is also implemented only in `world.S` with AArch64 instructions. The native x86_64 execution boundary has not been implemented or demonstrated safe.

Task 1: Ruling — close the direct same-kernel x86_64 backend because its exception, syscall, and address-space ownership model is not safe or implemented; continue the x86_64 goal with the separate QEMU/KVM runtime plan while preserving ARM64 modules. Cost if wrong: x86_64 uses a VM boundary instead of the original same-kernel hosting model, and its guest will need QEMU/OVMF plus a prepared disk. ARM64 module compilation passes; an ARM64/macOS runtime smoke test is still unavailable here.

ARM64 regression build — Cross-compiled the complete `fki.ko` and `xnu.ko` modules against Debian 6.12.118 ARM64 headers using GCC 14. The build passed. The first attempt exposed that `kernel_neon_begin()` and `kernel_neon_end()` take no scratch-state parameter in these headers; `fki_fpsimd_forget()` was updated to the current API. This is compile evidence only; no ARM64 runtime is available in this x86_64 environment.
