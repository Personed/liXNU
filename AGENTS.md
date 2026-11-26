# Instructions for project agents

## Project goal

Run macOS on Intel and AMD x86_64 PCs through QEMU/KVM while preserving the working ARM64 hosted-XNU path.

- **ARM64 Linux:** preserve the existing direct XNU-in-Linux runtime using `fki.ko` and `xnu.ko`, including its build and runtime behavior.
- **x86_64 Intel and AMD Linux:** use QEMU/KVM to run a user-prepared macOS Tahoe 26 x86_64 guest, the latest macOS release compatible with Intel. The project modules and world-switch code remain ARM64-only; do not present them as an x86_64 runtime or add unsafe no-op x86 hooks.
- **ARM64 Linux under QEMU TCG on an x86_64 PC:** keep this as a complementary compatibility and development profile for the PureDarwin console. It does not replace the native x86_64 macOS guest target and does not establish Tahoe, KVM, or physical-hardware support.
- Keep Intel and AMD acceptance separate. Launcher tests and TCG Linux checks do not establish support. Claim support for a vendor only after a physical host boots Tahoe to the desktop and passes host-integrity and shutdown checks.

## x86_64 macOS/KVM runtime

- KVM is the default accelerator. QEMU 10 or newer, `qemu-img`, and `flock` (util-linux) are required. QEMU `host` is the default guest CPU model under KVM so the guest sees the host's actual CPU model/features. TCG is an explicit development/test path only and defaults to QEMU `max`.
- The x86 host feature floor is SSE4.2, AVX, FMA, MOVBE, POPCNT, LZCNT, BMI1, AVX2 and BMI2. AMD guests need matching OpenCore AMD kernel patches and physical AMD validation.
- The x86 launcher accepts a user-prepared raw or qcow2 boot disk with EFI/OpenCore. It writes guest changes to a persistent qcow2 overlay and keeps OVMF variables in the per-VM state directory. `STATE_DIR` must be readable, writable, and searchable so the launcher can check existing disk state. A state directory is for one VM and one running QEMU process. The `.base` sidecar identifies launcher-owned overlays; a user source image with the reserved overlay filename pattern is accepted when it has no matching sidecar. Existing overlays may be stored elsewhere and symlinked into `STATE_DIR` if the link resolves to a regular file. Do not use that VM's managed overlay as `DISK` with the same state directory; choose its original base or a separate directory to fork the overlay. Do not write to the source disk or firmware template.
- Do not add Apple macOS images, Apple firmware, or the local `APPLE_SMC_OSK` value to Git or logs. Never put the SMC value in QEMU's argv or environment; pass it through a mode-0600 temporary QEMU config that is removed after exit. Do not claim that a TCG Linux boot is a macOS or KVM test.
- Keep user-visible feature/support tables precise: ARM64 uses the hosted XNU modules; x86_64 uses a QEMU/KVM macOS guest. Label the ARM64 TCG path separately.

## Required PR coordination

Project work is coordinated continuously through pull request comments. For **every task**:

1. Read this file and inspect the working tree and branch before editing.
2. Find open PRs touching the files, runtime path, or interfaces involved. Read the current conversation comments, inline review comments, review submissions, and requested changes; the PR description alone is not enough. Recheck those threads as work progresses.
3. Before changing a shared interface or overlapping files, post a coordination comment on the relevant PR stating your scope, files/interfaces, and dependencies. If no relevant PR exists, create a ready-for-merge PR as soon as a reviewable change is ready.
4. Keep all related agents visibly in sync in PR comments: post decisions, interface changes, test results, and blockers; link related PRs; reply to one another's coordination comments. Do not rely on private or local handoffs for project decisions.
5. Use your own branch and PR. Do not push commits to `main` directly or close another agent's PR.

## Merge policy

The project owner explicitly authorized merging pull requests as soon as all required automated checks are green; do not wait for a human review. Never merge while a required check is pending or failed. Use auto-merge when it is available and enforces the checks; otherwise monitor the checks and merge directly once they are green. If a repository rule independently requires human approval and blocks a merge, report the exact restriction and continue other work; do not disable repository protections without separate authorization.

Do not leave parallel agents to infer ownership from the code. Use the PR comments to divide overlapping work and confirm interface changes. Continue independent work while waiting on a reply; record unresolved dependencies in the PR.

## Validation

For x86_64 launcher or documentation changes, run the relevant checks:

```sh
bash tests/x86_64/test-launcher.sh
bash tests/x86_64/test-qemu-overlay-symlink.sh
bash tests/x86_64/test-tcg-smoke.sh
for script in scripts/x86_64/xnulinux-boot.sh tests/x86_64/test-launcher.sh tests/x86_64/test-qemu-overlay-symlink.sh tests/x86_64/tcg-smoke.sh tests/x86_64/test-tcg-smoke.sh; do bash -n "$script"; done
git diff --check
```

When a disposable x86_64 Linux kernel and initramfs are available, run `tests/x86_64/tcg-smoke.sh KERNEL INITRD` and report that it is a TCG-only Linux smoke test. Preserve the ARM64 module CI in `.github/workflows/modules.yml`; run its build steps or equivalent ARM64 cross-build after shared kernel-module changes.

## Blockers and evidence

Resolve technical blockers and continue with independent tasks. If a step depends on unavailable physical hardware, a user-prepared macOS disk, or credentials, complete the code, docs, and automated checks that do not depend on that input, then record the exact remaining acceptance step and keep support claims conservative. Missing hardware evidence may limit support claims, but it must not hold a PR whose automated checks are green.
