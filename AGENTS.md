# Instructions for project agents

## Project goal

Support Linux PCs without removing the working ARM64 path:

- **ARM64:** keep the existing direct XNU-in-Linux runtime using `fki.ko` and `xnu.ko`. Preserve its build and runtime behavior unless a task specifically changes it.
- **x86_64 Intel and AMD:** use QEMU/KVM to run a user-prepared macOS Tahoe 26 x86_64 guest. The current project modules and world-switch code are ARM64-only; do not present them as an x86_64 runtime or add unsafe no-op x86 hooks.
- Keep Intel and AMD acceptance separate. The launcher and TCG checks do not establish support. Claim support for a vendor only after a physical host boots Tahoe to the desktop and passes host-integrity and shutdown checks.

## Runtime decisions and boundaries

- KVM is the default accelerator. QEMU 10 or newer, `qemu-img`, and `flock` (util-linux) are required. QEMU `host` is the default guest CPU model under KVM so the guest sees the host's actual CPU model/features. TCG is an explicit development/test path only and defaults to QEMU `max`.
- The x86 host feature floor is SSE4.2, AVX, FMA, MOVBE, POPCNT, LZCNT, BMI1, AVX2 and BMI2. AMD guests need matching OpenCore AMD kernel patches and physical AMD validation.
- The x86 launcher accepts a user-prepared raw or qcow2 boot disk with EFI/OpenCore. It writes guest changes to a persistent qcow2 overlay and keeps OVMF variables in the per-VM state directory. `STATE_DIR` must be readable, writable, and searchable so the launcher can check existing disk state. A state directory is for one VM and one running QEMU process. Do not write to the source disk or firmware template.
- Do not add Apple macOS images, Apple firmware, or the local `APPLE_SMC_OSK` value to Git or logs. Never put the SMC value in QEMU's argv or environment; pass it through a mode-0600 temporary QEMU config that is removed after exit. Do not claim that a TCG Linux boot is a macOS or KVM test.
- Keep user-visible feature/support tables precise: ARM64 uses modules; x86_64 uses a QEMU/KVM guest.

## Required PR coordination

Project work is coordinated continuously through pull request comments. For **every task**:

1. Read this file and inspect the working tree and branch before editing.
2. Find open PRs touching the files, runtime path, or interfaces involved. Read the current conversation comments, inline review comments, review submissions, and requested changes; the PR description alone is not enough. Recheck those threads as work progresses.
3. Before changing a shared interface or overlapping files, post a coordination comment on the relevant PR stating your scope, files/interfaces, and dependencies. If no relevant PR exists, create a ready-for-merge PR as soon as a reviewable change is ready.
4. Keep all related agents visibly in sync in PR comments: post decisions, interface changes, test results, and blockers; link related PRs; reply to one another's coordination comments. Do not rely on private or local handoffs for project decisions.
5. Use your own branch and PR. Do not push commits to `main` directly or close another agent's PR.

## Merge policy

The project owner has authorized automatic integration: **do not wait for human review**. When branch protection requires the relevant CI checks, enable GitHub auto-merge on each ready PR. If checks are not enforced by branch protection or auto-merge is unavailable, monitor the PR and merge only after all of its automated checks report green. Never merge with a failed or pending check. If repository rules require a human approval, remove that review gate when repository permissions allow; otherwise report the exact GitHub restriction and continue other work without treating a human review as an engineering dependency.

Do not leave parallel agents to infer ownership from the code. Use the PR comments to divide overlapping work and confirm interface changes. Continue independent work while waiting on a reply; record unresolved dependencies in the PR.

## Validation

For x86_64 launcher or documentation changes, run the relevant checks:

```sh
bash tests/x86_64/test-launcher.sh
bash tests/x86_64/test-tcg-smoke.sh
for script in scripts/x86_64/xnulinux-boot.sh tests/x86_64/test-launcher.sh tests/x86_64/tcg-smoke.sh tests/x86_64/test-tcg-smoke.sh; do bash -n "$script"; done
git diff --check
```

When a disposable x86_64 Linux kernel and initramfs are available, run `tests/x86_64/tcg-smoke.sh KERNEL INITRD` and report that it is a TCG-only Linux smoke test. Preserve the ARM64 module CI in `.github/workflows/modules.yml`; run its build steps or equivalent ARM64 cross-build after shared kernel-module changes.

## Blockers and evidence

Resolve technical blockers and continue with independent tasks. If a step depends on unavailable physical hardware, a user-prepared macOS disk, or credentials, complete the code, docs, and automated checks that do not depend on that input, then record the exact remaining acceptance step and keep support claims conservative. Missing hardware evidence may limit support claims, but it must not hold a PR whose automated checks are green.
