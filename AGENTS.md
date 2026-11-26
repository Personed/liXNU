# Instructions for project agents

## Project goal

Make xnulinux executable on compatible ARM64 Linux hosts and on Intel/AMD x86-64 PCs through a QEMU TCG ARM64 Linux guest.

- **Compatible ARM64 Linux hosts:** preserve and validate the direct XNU-in-Linux runtime using `fki.ko` and `xnu.ko`, including the macOS 27 path. Record the CPU, kernel, PAC, 4 KiB page size, exception level, memory layout, and furthest verified stage for each host profile.
- **Intel/AMD x86-64 PCs:** use QEMU TCG to boot ARM64 Linux, then run the hosted-XNU/PureDarwin path inside that guest. This is the active PC compatibility route and does not depend on VT-x or AMD-V. Keep host-visible RAM, guest RAM, operating system, QEMU build, and firmware requirements separate; the 20 GiB total / 4 GiB available screen is operational guidance, not a measured physical minimum.
- Treat end-to-end validation as incomplete until reproducible build, install, boot, PureDarwin console, guest-issued halt, module unload, Linux guest power-off, and clean QEMU exit are recorded for each claimed profile. Hosted runner results do not establish physical-PC support.
- Native x86_64 hosted-XNU/Tahoe and QEMU/KVM macOS guest work are separate experiments. They do not replace the active TCG route or establish its acceptance.

## Separate native x86_64/Tahoe experiment

- Keep the native x86_64 FKI/XNU work architecture-aware and fail closed until entry, return, fault recovery, and shutdown are validated. Target user-provided Tahoe 26 artifacts only; do not add Apple system images, firmware, or local secrets to Git or logs.
- Keep Intel and AMD physical acceptance separate. Record the tested Linux kernel, CPU features, memory, page size, XNU build, host-integrity checks, and furthest boot stage. A build, module probe, QEMU/KVM guest, or TCG guest is not native x86_64 hosted-XNU execution.
- Label this work as an experiment in user-facing support tables; it is not the current Intel/AMD PC compatibility path.

## Active PC compatibility route: QEMU TCG and PureDarwin

- Treat x86-64 PC → QEMU TCG → ARM64 Linux → FKI/XNU → PureDarwin as the current PC compatibility objective. Report firmware, Linux boot, module build/load, FKI configuration, carve-out allocation/reuse, XNU carve-out use, PureDarwin console, guest-issued halt, module unload, Linux guest power-off, and QEMU exit as distinct stages.
- A module build or probe is not XNU execution; a PureDarwin console is not macOS desktop support. Keep the direct ARM64 Linux host profile separate from the x86-64 PC/TCG profile.
- The #36 reference profile assigns 16 GiB to the ARM64 guest and screens for 20 GiB total host-visible RAM plus 4 GiB available before artifact preparation. Record these host values separately from guest RAM and identify the measurement phase. This screen is not a universal minimum or a performance claim.
- Keep #36/#46 CI results separate from local pinned-kernel results in #62 and other stacked runs. Preserve overlap and kernel-input caveats. Do not claim the route supported until current integrated artifacts pass console/halt/unload/power-off checks on the stated profiles.
- Record input hashes, host-capacity measurements, stage limits, and current evidence in [`docs/portability-validation.md`](docs/portability-validation.md). Keep README, acceptance criteria, and PR titles/bodies aligned with the TCG PC objective; keep native x86_64/Tahoe separate.

## Required PR coordination

Project work is coordinated continuously through pull request comments. For **every task**:

1. Read this file and inspect the working tree and branch before editing.
2. Find open PRs touching the files, runtime path, or interfaces involved. Read the current conversation comments, inline review comments, review submissions, and requested changes; the PR description alone is not enough. Recheck those threads as work progresses.
3. Before changing a shared interface or overlapping files, post a coordination comment on the relevant PR stating your scope, files/interfaces, and dependencies. If no relevant PR exists, create a ready-for-merge PR as soon as a reviewable change is ready.
4. Keep all related agents visibly in sync in PR comments: post decisions, interface changes, test results, and blockers; link related PRs; reply to one another's coordination comments. Do not rely on private or local handoffs for project decisions.
5. Use your own branch and PR. Do not push commits to `main` directly or close another agent's PR.

## Merge policy

The project owner explicitly authorized direct merging as soon as all required checks for the current head are green and GitHub reports the PR mergeable; do not wait for human review. Never merge while a required check is pending or failed. If a repository protection actually blocks the merge by requiring human approval, record the exact restriction and continue independent work. Do not disable repository protections.

Do not leave parallel agents to infer ownership from the code. Use the PR comments to divide overlapping work and confirm interface changes. Continue independent work while waiting on a reply; record unresolved dependencies in the PR.

## Separate native x86_64 experiment: validation

These commands validate the separate native x86_64 experiment and its fail-closed stub; they do not accept the active TCG PC route. Keep x86_64 and ARM64 kernel header trees separate, and pass the architecture explicitly to Kbuild. Run the x86_64 commands on an x86_64 Linux host with matching headers; the ARM64 commands below run on an ARM64 Linux host with matching headers. The x86_64 FKI build compiles only the host backend; the x86_64 XNU target is a refusal stub, not a runtime implementation. The stub-load script starts its own disposable VM and taints only that guest kernel. On Debian/Ubuntu, module builds need `build-essential` and matching kernel headers; the stub VM test needs `qemu-system-x86`, `busybox-static`, `cpio`, `kmod`, and `sudo`, plus APT download access to the matching `linux-image-unsigned-$KERNEL_RELEASE` package. CI covers the x86_64 FKI build, the x86_64 fail-closed XNU test, and the ARM64 module build.

```sh
X86_KDIR=/path/to/x86_64/kernel-headers
ARM64_KDIR=/path/to/arm64/kernel-headers

ARCH=x86_64 tests/x86_64/build-fki.sh "$X86_KDIR"
make -C "$X86_KDIR" ARCH=x86_64 M="$PWD/xnu-ko" modules
tests/x86_64/test-xnu-stub-vm.sh "$X86_KDIR"  # run on the x86_64 host; this script boots the disposable VM

make -C "$ARM64_KDIR" ARCH=arm64 M="$PWD/fki-ko" modules
make -C "$ARM64_KDIR" ARCH=arm64 M="$PWD/xnu-ko" KBUILD_EXTRA_SYMBOLS="$PWD/fki-ko/Module.symvers" modules
git diff --check
```

Retain QEMU/KVM launcher checks as separate guest-launcher coverage. Treat TCG tests as the active PC compatibility path, keep ARM64 module CI in `.github/workflows/modules.yml`, and preserve the native x86_64/Tahoe work as a separate experiment.

## Blockers and evidence

Resolve technical blockers and continue with independent tasks. If a step depends on unavailable physical hardware, a user-prepared macOS disk, or credentials, complete the code, docs, and automated checks that do not depend on that input, then record the exact remaining acceptance step and keep support claims conservative. Missing hardware evidence may limit support claims, but it must not hold a PR whose automated checks are green.
