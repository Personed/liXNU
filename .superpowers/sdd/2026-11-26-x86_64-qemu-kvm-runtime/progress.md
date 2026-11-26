# SDD ledger — plan: docs/superpowers/plans/2026-11-26-x86_64-qemu-kvm-runtime.md

Ruling: Use a persistent qcow2 overlay backed by the user disk, and default KVM to QEMU `host` CPU passthrough — QEMU documents backing files as preserving the backing image and recommends host passthrough when migration is unnecessary; an Intel-named Haswell profile is not a sound AMD default — cost if wrong: image writes grow in the state directory and host CPU identity/features become visible to macOS, so disk-space and OpenCore CPU patches must be documented.

Task 1: Ruling: Publish OVMF variables and qcow2 overlays through a temporary file while holding a per-state `flock` lock — this prevents partially initialized state and concurrent QEMU writers from corrupting the VM — cost if wrong: stale or interleaved state can make a prepared guest unbootable.

Task 1: Ruling: Select the ARM64 `kernel_neon_begin/end` signature at Linux 6.19, where upstream changed the API to accept `struct user_fpsimd_state *` — the repository's Debian 6.12 cross-build and Ubuntu 7.0 module CI cover opposite sides of the change — cost if wrong: ARM64 module builds fail on one supported kernel-header generation.

Task 1: Ruling: Bind a persistent overlay to a source-image SHA-256 recorded in a private sidecar and refuse unassociated legacy overlays — same-content restores reuse the overlay after an inode/metadata change, changed base contents fail closed, and legacy states cannot silently become empty VMs — cost if wrong: safe source-image replacement requires selecting a new state directory, and hashing may read the full disk.

Task 1: Ruling: Pass the local Apple SMC OSK through a mode-0600 temporary QEMU config instead of process arguments or environment — this reduces exposure through process listings and environment inspection — cost if wrong: QEMU must support `-readconfig` for the Apple SMC device configuration.

Project workflow ruling: No merge or auto-merge is authorized without an explicit user instruction. This optional QEMU/KVM guest path is separate from the active x86-64 TCG → ARM64 Linux → PureDarwin acceptance route.

Pre-flight: Task 1 produces launcher flags and disk/state paths consumed by Task 3 documentation; Task 2 produces the TCG smoke invocation and evidence consumed by Task 3. No direct code interfaces are shared by Tasks 1 and 2. Task 4 consumes Task 3's evidence matrix and real hardware requirements.

Task 1: complete — fake-QEMU tests cover preflight errors, CPU/KVM gating, TCG and KVM arguments, paths with spaces and commas, protected SMC config creation/cleanup, effective state-directory permission checks, a hash-named source image alongside a symlinked state directory, managed-overlay-as-base refusal, source image preservation, same-content restore, changed-base rejection, unassociated legacy overlay refusal, persistent state, and concurrent locking.

Task 2: complete — fake harness tests pass; the disposable Debian 6.12.118 guest booted with QEMU 10.0.13 TCG. This is Linux-only development evidence.

Task 3: in progress — documentation identifies the QEMU TCG → ARM64 Linux → PureDarwin route as the active target, preserves direct ARM64 hosting, and labels the Tahoe/KVM guest and native x86_64 port as separate experiments.

Task 4: pending physical Intel and AMD acceptance for this optional guest launcher. These results remain separate from the active TCG/PureDarwin acceptance.
