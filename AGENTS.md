# Instructions for project agents

## Project goal

Make xnulinux run on x86_64 PCs using QEMU TCG to boot Linux ARM64, load FKI/XNU, and reach the PureDarwin shell, while preserving the direct ARM64 hosted-XNU path. This is the active portability goal; do not claim universal compatibility.

- **ARM64 Linux:** preserve the existing direct XNU-in-Linux runtime using `fki.ko` and `xnu.ko`, including its build and runtime behavior and macOS 27 target.
- **x86_64 Intel and AMD PCs:** use QEMU TCG to boot the ARM64 Linux guest, load FKI/XNU, and reach the PureDarwin console. Linux x86_64 is the current required host profile; Windows and Intel macOS remain candidates until the full route passes there. Native x86_64/Tahoe hosting is separate and does not count toward this acceptance.
- **Separate workstreams:** retain the native x86_64/Tahoe port and QEMU/KVM launcher as separate experiments. Neither replaces the QEMU TCG/PureDarwin acceptance route.
- Track host OS and CPU profiles separately. Virtualized CI proves only the tested runner profile; it does not establish physical-PC or universal compatibility. Do not claim a physical profile without a full-route test on that hardware.

## Separate native x86_64 hosted-XNU experiment

The following requirements apply only to the separate native port; they are not acceptance criteria for the active QEMU TCG route.

- Build FKI, XNU, the machine layer, kexts, and kernel collections as architecture-specific x86_64 outputs while retaining ARM64 outputs and behavior.
- Keep register state, exception entry, MMU/TLB operations, syscall delivery, Mach-O CPU types, boot handoff, and kernel collections architecture-aware. Reject architecture and ABI mismatches before entering XNU.
- Establish a safe Linux/XNU execution boundary before running foreign kernel code. Verify Linux register, floating-point, exception, syscall, and memory state through entry, return, fault recovery, and shutdown.
- Use user-provided Tahoe 26 artifacts that match the selected XNU build. Do not add Apple system images, firmware, or local secrets to Git or logs.
- Measure Intel and AMD requirements independently. Record the tested Linux kernel, CPU features, memory, page size, XNU build, and furthest boot stage. A QEMU/KVM macOS guest or TCG ARM64 guest does not establish the native module port.
- Keep user-visible support tables precise: ARM64 direct hosted XNU is the existing supported path; x86_64 native hosting remains a separate experiment until its own target and host-integrity checks pass.

## Active QEMU TCG and PureDarwin acceptance path

- Use the QEMU TCG → ARM64 Linux → FKI/XNU → PureDarwin flow as the active cross-host acceptance path. It does not test native x86_64 module execution or establish Tahoe hosting.
- Report firmware, Linux boot, FKI configuration, carve-out allocation/reuse, XNU module probe, XNU carve-out use, PureDarwin console, guest-issued halt, module unload, Linux guest power-off, and QEMU exit as distinct stages. A module build or probe is not XNU execution; a PureDarwin console is not macOS desktop support.
- The historical #36 reference profile assigns 16 GiB to the ARM64 guest and screens for 20 GiB total host RAM plus 4 GiB available before artifact preparation. The reduced 6 GiB guest profile passed full-route CI on #52 run #138 (head `8d41fa9`) and #72 run #140 (tree identical to the earlier #52 head `3b47444`). PR #75 is now part of current #52 head `24e4a9c`; current-head image run #148 is in progress and must pass before claiming the pinned-tree profile is validated. Record host capacity separately from guest RAM and identify the measurement phase; neither profile establishes a physical host or guest minimum.
- Keep #36/#46 CI results separate from the local pinned-kernel result in #62. Preserve the overlap and kernel-input caveats. Do not claim physical-PC support until the console/halt/unload/power-off route passes on representative physical Intel and AMD hardware for each supported host OS profile, with measured resource floors.
- Record detailed stage limits, host capacity, input hashes, and current evidence in [`docs/portability-validation.md`](docs/portability-validation.md). Keep the support table and PR title/body aligned with the QEMU TCG/PureDarwin goal.

## Required PR coordination

Project work is coordinated continuously through pull request comments. For **every task**:

1. Read this file and inspect the working tree and branch before editing.
2. Find open PRs touching the files, runtime path, or interfaces involved. Read the current conversation comments, inline review comments, review submissions, and requested changes; the PR description alone is not enough. Recheck those threads as work progresses. For each applicable automated review finding, reply in its thread with the fix commit and validation evidence, or an evidence-based explanation that it is duplicate, superseded, or inapplicable.
3. Before changing a shared interface or overlapping files, post a coordination comment on the relevant PR stating your scope, files/interfaces, and dependencies. If no relevant PR exists, create a ready-for-merge PR as soon as a reviewable change is ready.
4. Keep all related agents visibly in sync in PR comments: post decisions, interface changes, test results, and blockers; link related PRs; reply to one another's coordination comments. Do not rely on private or local handoffs for project decisions.
5. Use your own branch and PR. Do not push commits to `main` directly or close another agent's PR.

## Merge policy

Do not merge a pull request or enable auto-merge unless the project owner explicitly authorizes it for that PR. After explicit authorization, merge only when all required checks for the current head are green and GitHub reports the PR mergeable. Do not disable repository protections.

Do not leave parallel agents to infer ownership from the code. Use the PR comments to divide overlapping work and confirm interface changes. Continue independent work while waiting on a reply; record unresolved dependencies in the PR.

## Validation

The following module-build commands support the separate native x86_64 experiment and the direct ARM64 runtime. The active acceptance is the QEMU TCG → ARM64 Linux → PureDarwin workflow in [`docs/portability-validation.md`](docs/portability-validation.md); module builds and smoke tests do not substitute for its full boot checks.

Keep x86_64 and ARM64 kernel header trees separate, and pass the architecture explicitly to Kbuild. Run the x86_64 commands on an x86_64 Linux host with matching headers; the ARM64 commands below run on an ARM64 Linux host with matching headers. The x86_64 FKI build compiles only the host backend; the x86_64 XNU target is a refusal stub, not a runtime implementation. The stub-load script starts its own disposable VM and taints only that guest kernel. On Debian/Ubuntu, module builds need `build-essential` and matching kernel headers; the stub VM test needs `qemu-system-x86`, `busybox-static`, `cpio`, `kmod`, and `sudo`, plus APT download access to the matching `linux-image-unsigned-$KERNEL_RELEASE` package. CI covers the x86_64 FKI build, the x86_64 fail-closed XNU test, and the ARM64 module build.

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

Use the QEMU TCG full-route checks as the primary portability acceptance and label their runner limits. Keep native x86_64 builds and the QEMU/KVM launcher as separate coverage; neither substitutes for the TCG PureDarwin boot. Keep ARM64 module CI in `.github/workflows/modules.yml`; add x86_64 builds without dropping ARM64 coverage.

## Blockers and evidence

Resolve technical blockers and continue with independent tasks. If a step depends on unavailable physical hardware, a user-prepared macOS disk, or credentials, complete the code, docs, and automated checks that do not depend on that input, then record the exact remaining acceptance step and keep support claims conservative. Missing hardware evidence may limit support claims, but it must not hold a PR whose automated checks are green.
