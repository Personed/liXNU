# SDD ledger — separate native x86_64 experiment

This ledger records a separate native x86_64 experiment. The active portability goal is x86-64 PC → QEMU TCG → ARM64 Linux → FKI/XNU → PureDarwin console; this experiment does not replace or redefine that goal.

Pre-flight: Task 1 produces the FKI execution context consumed by Task 2. Task 3 produces the x86_64 kernel and boot ABI consumed by Tasks 2 and 4. Task 4 validates and emits collections consumed by Task 3 and Task 5. Task 5 consumes the architecture and collection outputs from Tasks 3–4. Task 6 verifies both architecture paths and their system smoke procedures.

Task 1: in progress. An x86_64 FKI host backend now builds, but advertises no XNU execution capability. A safe XNU entry/return boundary is still unimplemented; continue the port and keep the ARM64 regression build passing.

Task 1: Baseline finding — Before the x86_64 backend was added, Debian 6.12.118 x86_64 headers compiled shared FKI objects until `fki_hostcall.c` used ARM-oriented `page_to_phys`, which does not exist on x86. Replacing it with `PFN_PHYS(page_to_pfn(pg))` advanced compilation to modpost, which reported missing architecture backend symbols (`fki_counter`, `fki_arch_linux_runs`, `fki_fpsimd_forget`, `fki_counter_frequency`, `fki_arch_init`, `fki_arch_exit`).

Task 1: Finding — The FKI host-backend build in [PR #48](https://github.com/Personed/xnulinux/pull/48) adds Linux-side x86_64 hooks, counters and an alternate Linux stack. It builds against Debian 6.12.118 x86_64 headers. `fki_caps()` returns zero; the backend does not enter or execute XNU, and its stack helpers are not evidence of a safe Linux/XNU context boundary. [PR #47](https://github.com/Personed/xnulinux/pull/47) adds a fail-closed x86_64 `xnu.ko` target that returns `-EOPNOTSUPP` until a native world switch exists.

Task 1: Environment — QEMU 10.0.13, Debian 6.12.118 x86_64 image and headers were extracted under `/workspace/scratch`. The host has no `/dev/kvm`, no module headers for its running kernel, and no root privileges. QEMU TCG can host a test VM. This lets us load modules in a guest but cannot validate the current host kernel directly.

Task 1: Finding — The disposable QEMU TCG guest boots Debian 6.12.118 x86_64 to a BusyBox shell. This validates only the VM harness. At the time of that initial smoke run, the baseline FKI module failed modpost; the later host-backend build is separate and has not been loaded or runtime-tested in the guest.

Task 1: Finding — Compiling `xnu-ko/xnu_main.c` for x86_64 reaches the assembler and fails on AArch64-only operations (`mrs` system registers, `ldaxr`/`stlxr`, PAC instructions, and ARM cache/barrier instructions). The world switch is also implemented only in `world.S` with AArch64 instructions. The native x86_64 execution boundary has not been implemented or demonstrated safe.

Task 1: Ruling — native x86_64 hosting remains exploratory work in its separate PRs. It is not the active acceptance goal in this conversation. Do not claim x86_64 runtime support or enter XNU until the execution boundary and host-integrity checks pass. Preserve the ARM64 module build and runtime.

ARM64 regression build — Cross-compiled the complete `fki.ko` and `xnu.ko` modules against Debian 6.12.118 ARM64 headers using GCC 14. The build passed. The first attempt exposed that `kernel_neon_begin()` and `kernel_neon_end()` take no scratch-state parameter in these headers; `fki_fpsimd_forget()` was updated to the current API. This is compile evidence only; no ARM64 runtime is available in this x86_64 environment.
