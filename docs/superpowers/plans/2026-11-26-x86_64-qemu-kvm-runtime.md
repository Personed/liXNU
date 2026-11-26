# x86_64 QEMU/KVM Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a QEMU/KVM runtime for a user-prepared macOS Tahoe 26 x86_64 disk on Intel and AMD Linux PCs, while preserving the existing ARM64 module runtime.

**Architecture:** Keep `fki.ko` and `xnu.ko` on the existing ARM64 direct path. On x86_64, validate the host and a bootable disk, then launch a Q35/OVMF guest through QEMU/KVM. QEMU TCG remains an explicit development mode and is never presented as native-performance support.

**Tech Stack:** Bash, QEMU 10+, KVM (VT-x or AMD-V), OVMF UEFI, macOS Tahoe 26 x86_64, shell test harnesses.

**Spec:** `docs/superpowers/specs/2026-11-26-x86_64-qemu-kvm-runtime-design.md`

## Global Constraints

- The x86_64 support path runs a user-provided bootable Tahoe 26 x86_64 disk through QEMU/KVM; it does not build or load the ARM-only `xnu.ko` or `fki.ko`. Guest writes go to a persistent qcow2 overlay, preserving the source disk.
- A supported x86_64 host must expose `/dev/kvm` and the CPU feature floor in the spec (SSE4.2, AVX, FMA, MOVBE, POPCNT, LZCNT, BMI1, AVX2 and BMI2); `qemu-img` and `flock` are required, and TCG is development-only.
- The guest profile uses Q35, OVMF UEFI, Apple SMC, AHCI, E1000, USB keyboard/tablet and basic VGA display. KVM defaults to QEMU `host` CPU passthrough; TCG defaults to `max`. AMD disks need matching OpenCore AMD kernel patches.
- ARM64 continues to use the existing FKI/XNU modules and build workflow.
- No Apple system image or SMC key is committed to the repository.

## Review Focus

- Wrong host architecture: reject before starting QEMU; test with an injectable host architecture.
- KVM unavailable or inaccessible: fail with a clear message in default KVM mode; test with an injectable KVM device path.
- Missing x86_64h features for Tahoe: report the full missing-feature list; test with an injectable cpuinfo file.
- Missing disk, OVMF code/vars template, or Apple SMC key: fail before starting QEMU; test each missing input.
- Missing qemu-img/flock or unusable state directory: fail before writing VM state; test that `--check` does not create files.
- Invalid disk format or whitespace in the QEMU display value: reject invalid disk formats and pass display values as a single argument; test with the fake QEMU recorder.

---

### Task 1: Build the x86_64 QEMU launcher

**Files:**
- Create: `scripts/x86_64/xnulinux-boot.sh`
- Create: `tests/x86_64/test-launcher.sh`
- Test: `tests/x86_64/test-launcher.sh`

**Interfaces:**
- Consumes environment: `DISK`, `DISK_FORMAT` (`qcow2` by default; `raw` or `qcow2`), `ACCEL` (`kvm` by default or explicit `tcg`), `CPU_MODEL`, `CPUS`, `RAM_MB`, `OVMF_CODE`, `OVMF_VARS_TEMPLATE`, `STATE_DIR`, `QEMU_DISPLAY`, `APPLE_SMC_OSK` (64 bytes), `QEMU`, `QEMU_IMG`, `FLOCK`, `KVM_DEVICE`, `CPUINFO_FILE`.
- `scripts/x86_64/xnulinux-boot.sh [--check]` validates inputs and host capabilities. Without `--check`, it execs QEMU; errors exit nonzero before QEMU starts.
- KVM profile defaults to QEMU's `host` passthrough CPU. TCG defaults to QEMU's `max` CPU profile. User overrides use `CPU_MODEL`.
- The script creates a persistent writable OVMF vars copy and persistent qcow2 disk overlay in `STATE_DIR` only if they do not exist; it never modifies the template or source disk. `qemu-img` creates the overlay atomically, and a `flock` lock allows only one running QEMU instance per state directory. A SHA-256 sidecar verifies same-content source-image restoration and rejects changed base contents instead of silently switching overlays. The launcher also refuses unassociated legacy overlays when their former inode-based name cannot be reconstructed. The Apple SMC key is passed through a mode-0600 temporary config, not QEMU's process arguments or environment, and the file is removed on exit.

- [x] Write failing launcher tests for missing disk, missing firmware, missing SMC key/length, invalid disk format, missing `qemu-img`/`flock`, unavailable KVM, unsupported host CPU flags, wrong host architecture, unreadable/unusable state directory, partial overlay cleanup, unassociated legacy overlay refusal, rejecting a metadata-marked managed overlay selected by link or target as its own base, accepting a backed qcow2 source with an overlay-shaped filename, reusing an overlay symlinked from another volume, a hash-named source disk inside a symlinked `STATE_DIR`, concurrent state locking, source-image preservation, persistent overlay reuse, and a valid TCG launch. Add a `qemu-img`/`qemu-io` integration check that persists a write and validates launch rejection through the symlink and external target.
- [x] Run `bash tests/x86_64/test-launcher.sh` and confirm the expected failures and argument checks.
- [x] Implement the launcher with strict shell mode, explicit preflight checks, Q35/OVMF pflash, Apple SMC, AHCI disk backed by a qcow2 overlay, E1000 network, USB input and VGA display.
- [x] Run the test script and confirm fake QEMU argument construction, source-image preservation, stable overlay identity after byte-identical restoration, rejection of changed base contents, mode-0600 SMC config cleanup, and absence of the key from QEMU's argv/environment and launcher output.

### Task 2: Exercise the QEMU TCG development path

**Files:**
- Create: `tests/x86_64/tcg-smoke.sh`
- Create: `tests/x86_64/test-tcg-smoke.sh`
- Modify: `tests/x86_64/README.md`
- Test: `tests/x86_64/tcg-smoke.sh`

**Interfaces:**
- `tests/x86_64/tcg-smoke.sh KERNEL INITRD` boots a disposable x86_64 Linux kernel with QEMU TCG, serial output and a bounded timeout. It succeeds only after seeing the caller-provided init script's `XNU_LINUX_TCG_BOOTED` marker.
- The smoke test does not claim macOS, KVM, Intel, AMD or performance support.

- [x] Write the smoke script and verify it rejects missing kernel/initrd paths.
- [x] Run it with the disposable Debian 6.12.118 kernel and BusyBox initramfs in `/workspace/scratch` and confirm it sees the boot marker.
- [x] Record the exact QEMU version, guest kernel and limitations in `tests/x86_64/README.md`.

### Task 3: Document the split architecture support path

**Files:**
- Create: `AGENTS.md`
- Create: `.github/PULL_REQUEST_TEMPLATE.md`
- Create: `.github/workflows/x86_64.yml`
- Modify: `README.md`
- Modify: `tests/x86_64/README.md`
- Modify: `docs/superpowers/specs/2026-11-26-x86_64-qemu-kvm-runtime-design.md`
- Modify: `docs/superpowers/plans/2026-11-26-x86_64-qemu-kvm-runtime.md`
- Modify: `.superpowers/sdd/2026-11-26-x86_64-qemu-kvm-runtime/progress.md`
- Mark earlier direct-x86 module proposal as superseded.

- [x] Replace the proposed direct x86 module claims with the selected QEMU/KVM execution model and link the new design.
- [x] Document user-prepared EFI/OpenCore Tahoe disk requirements, AMD kernel patches, OVMF paths, Apple SMC environment setup, persistent overlay and KVM setup, and explicit TCG development mode.
- [x] Keep Intel, AMD and ARM support rows separate and mark only configurations with recorded full system runs as supported.
- [x] Add a pull request workflow for the x86_64 launcher tests so green PRs have an automated check.
- [x] Run `git diff --check`, `bash -n scripts/x86_64/xnulinux-boot.sh tests/x86_64/test-launcher.sh tests/x86_64/tcg-smoke.sh tests/x86_64/test-tcg-smoke.sh`, both x86_64 test scripts, and the existing ARM64 module build workflow in `.github/workflows/modules.yml` (or an equivalent ARM64 cross-build).

### Task 4: Hardware acceptance

**Files:**
- Update: `tests/x86_64/README.md`
- Update: `README.md`

- [ ] Boot the user-provided Tahoe 26 x86_64 image to the desktop with KVM on a physical Intel PC; verify display, network, input, disk and clean shutdown.
- [ ] Repeat on a physical AMD PC using the documented CPU profile; verify host integrity during and after the run.
- [ ] Re-run the documented macOS 27 ARM64 desktop smoke on supported ARM64 hardware and record kernel, CPU, RAM, page size and results.
- [ ] Claim only the host/guest combinations with complete logs and passing results.

## Handoff

Implement Tasks 1–3 in order. Task 4 needs the user-provided Tahoe disk and physical Intel, AMD and ARM64 hardware; the launcher and test harness must still be completed and verified in available environments.
