# SDD ledger — plan: docs/superpowers/plans/2026-11-26-x86_64-qemu-kvm-runtime.md

Ruling: Use a persistent qcow2 overlay backed by the user disk, and default KVM to QEMU `host` CPU passthrough — QEMU documents backing files as preserving the backing image and recommends host passthrough when migration is unnecessary; an Intel-named Haswell profile is not a sound AMD default — cost if wrong: image writes grow in the state directory and host CPU identity/features become visible to macOS, so disk-space and OpenCore CPU patches must be documented.

Task 1: Ruling: Publish OVMF variables and qcow2 overlays through a temporary file while holding a per-state `flock` lock — this prevents partially initialized state and concurrent QEMU writers from corrupting the VM — cost if wrong: stale or interleaved state can make a prepared guest unbootable.

Task 1: Ruling: Select the ARM64 `kernel_neon_begin/end` signature at Linux 6.19, where upstream changed the API to accept `struct user_fpsimd_state *` — the repository's Debian 6.12 cross-build and Ubuntu 7.0 module CI cover opposite sides of the change — cost if wrong: ARM64 module builds fail on one supported kernel-header generation.

Project workflow ruling: A ready PR merges when all automated checks are green; human approval is not a gate. Use auto-merge only when branch rules enforce the relevant checks, otherwise wait for every status check to turn green before merging.

Pre-flight: Task 1 produces launcher flags and disk/state paths consumed by Task 3 documentation; Task 2 produces the TCG smoke invocation and evidence consumed by Task 3. No direct code interfaces are shared by Tasks 1 and 2. Task 4 consumes Task 3's evidence matrix and real hardware requirements.

Task 1: complete — fake-QEMU tests cover preflight errors, CPU/KVM gating, argument safety, source image preservation, persistent state, and concurrent locking.

Task 2: complete — fake harness tests pass; the disposable Debian 6.12.118 guest booted with QEMU 10.0.13 TCG. This is Linux-only development evidence.

Task 3: in progress — split support documentation, continuous PR-comment coordination, no-human-review merge policy, PR template, and x86_64 launcher CI are in place. Local shell tests and Linux 6.12 ARM64 cross-build pass; GitHub's Ubuntu 7.0 ARM64 build exposed and is rerunning the 6.19 API compatibility fix.

Task 4: pending physical Intel/AMD Tahoe 26 and ARM64 macOS 27 acceptance; no hardware or user-prepared Tahoe disk is available in this environment.
