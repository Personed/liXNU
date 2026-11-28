# x86_64 PC support

The native x86_64 hosted-XNU port is in progress. The FKI Linux host backend now builds on x86_64 and preserves the
existing ARM64 backend, but it reports no XNU execution capabilities. The current XNU implementation in `xnu_main.c`
and `world.S` is AArch64-only and requires the FKI PAC capability. The x86_64 fail-closed XNU stub is tracked
separately in PR #47; it is not part of this FKI change and does not execute XNU. The architecture-specific XNU entry,
context switch, address-space and fault paths remain to be implemented and verified before macOS can run natively on
x86_64.

The source-pinned boot argument fixtures also check EFI map contents. The preflight reads the caller's byte
buffer using the declared descriptor stride and rejects physical page ranges that would truncate in XNU.
Pages 0 and UINT32_MAX pass the representation check, but the pinned XNU ignores them. Firmware memory
types, physical ownership, usable ranges, and placement in the host carve-out need separate validation.

```sh
bash tests/x86_64/test-x86-boot-args.sh
```

## Supplemental QEMU/KVM guest path

The optional x86_64 launcher runs a user-prepared macOS Tahoe 26 guest through QEMU/KVM. This guest path complements
the native port and does not build or load the project's `xnu.ko`. Intel and AMD remain separate acceptance targets;
neither is marked supported until tested on physical hardware.

## Host prerequisites

- x86_64 Linux with VT-x or AMD-V and a readable/writable `/dev/kvm` for native acceleration.
- Host CPU flags: SSE4.2, AVX, FMA, MOVBE, POPCNT, LZCNT (`lzcnt` or `abm` in Linux flags), BMI1, AVX2 and BMI2.
- QEMU 10 or newer, `qemu-img`, `flock` (from util-linux), and OVMF 4 MiB firmware. The launcher auto-detects common
  Debian/Ubuntu (`/usr/share/OVMF`), Fedora/RHEL (`/usr/share/edk2/ovmf*`, including qcow2 firmware), Arch
  (`/usr/share/edk2/x64`), and openSUSE (`/usr/share/qemu`) layouts. `OVMF_DIR` adds a custom directory; explicit
  `OVMF_CODE` and `OVMF_VARS_TEMPLATE` remain supported.
- A bootable Tahoe 26 x86_64 raw or qcow2 disk with EFI/OpenCore already configured. AMD guests need matching OpenCore
  AMD kernel patches.
- A locally supplied 64-byte Apple SMC OSK value in `APPLE_SMC_OSK`. Keep it out of the repository and logs. The launcher writes it to a mode-0600 temporary QEMU config, never puts it in QEMU's argv or environment, and removes the config when QEMU exits.

The launcher defaults to `CPU_MODEL=host` in KVM mode so the guest receives the actual host CPU model/features. TCG
defaults to QEMU's `max` model and is only for development tests. A hardware-support claim requires KVM.

## Boot a prepared guest

```sh
export DISK="$HOME/VMs/tahoe26.qcow2"
export DISK_FORMAT=qcow2
read -r -s -p 'Apple SMC OSK (64 bytes): ' APPLE_SMC_OSK
export APPLE_SMC_OSK
printf '\n'
scripts/x86_64/xnulinux-boot.sh --check
scripts/x86_64/xnulinux-boot.sh
```

For a raw image, set `DISK_FORMAT=raw`. Override `OVMF_DIR`, `OVMF_CODE`, `OVMF_VARS_TEMPLATE`, `STATE_DIR`,
`QEMU_DISPLAY`, `CPU_MODEL`, `CPUS`, or `RAM_MB` when the host needs different paths or resources. The default state directory is
`$XDG_STATE_HOME/xnulinux/x86_64` when `XDG_STATE_HOME` is set, or `~/.local/state/xnulinux/x86_64` otherwise. It
stores writable OVMF variables and a persistent qcow2 overlay backed by the source disk. Only one QEMU process may use a
state directory at a time; use one readable, writable, and searchable `STATE_DIR` per VM. Guest writes do not change the source image. Keep the source
image at its current path while the overlay exists; QEMU records that backing path. Overlay identity is
tied to the canonical source path, format, and a SHA-256 sidecar. If file metadata changes, the launcher checks the
contents: identical restored bytes reuse the overlay, but changed bytes fail before QEMU starts. Hashing reads the full
source image on its first launch and after metadata changes; later launches with unchanged metadata skip the read.
Restore the matching base image or set a new `STATE_DIR` to start fresh. `--check` validates requirements without
creating state files; disk-to-state association is checked at launch. The `.base` sidecar identifies launcher-owned
`disk-<20 hex>.qcow2` overlays, so a user-prepared qcow2 source with the same filename pattern is accepted when it has
no matching sidecar. An unassociated overlay from an earlier launcher stops startup with migration guidance. Existing
overlays can be stored on another volume and symlinked into `STATE_DIR` when the link resolves to a regular file. Do
not select a managed qcow2 overlay as `DISK` with the same `STATE_DIR`; use the original base, or choose another state
directory to fork it intentionally.

## Development tests

To reproduce the Debian 13 CI module pair, install `build-essential`, `git`, `linux-headers-amd64`, and `pahole`.
Use the same commit, kernel headers, and tool versions as the artifact's build manifest. The builder checks both
modules byte for byte across two clean build directories before writing them to the new output directory.

```sh
bash scripts/ci/build-debian-x86-module-pair.sh /path/to/headers HEAD /usr/src/lixnu ./debian-x86-modules
```

```sh
bash tests/x86_64/test-launcher.sh
bash tests/x86_64/test-tcg-smoke.sh
tests/x86_64/tcg-smoke.sh /path/to/x86_64/vmlinuz /path/to/initrd.cpio.gz
tests/x86_64/test-boundary-probe-vm.sh /path/to/x86_64/kernel-headers
tests/x86_64/exception-recovery/test-vm.sh /path/to/x86_64/kernel-headers
```

The smoke initramfs must print `XNU_LINUX_TCG_BOOTED` on its serial console and shut down before the timeout. TCG boots
only a disposable Linux guest; it does not demonstrate macOS boot, KVM, Intel/AMD compatibility or useful performance.

The one-argument boundary probe command uses APT on Debian or Ubuntu. It needs an unsigned kernel image package
matching the kernel headers and a `busybox-static` package in the configured repositories. To use local files on
another x86_64 Linux distribution, pass a bootable kernel image for those headers and a static x86_64 BusyBox:

```sh
tests/x86_64/test-boundary-probe-vm.sh /path/to/kernel-headers /path/to/vmlinuz /path/to/busybox-static
```

The runner builds its own initramfs and checks the booted kernel release against the headers before loading a module.
The BusyBox binary must support `--install -s` and include `sh`, `cat`, `uname`, `mount`, `insmod`, `rmmod`,
`dmesg`, `sync`, and `poweroff`. Both modes require compiler and kernel build tools, static C libraries, `cpio`, and
QEMU. Use the kernel configuration that produced the headers so the disposable guest can load the test modules.

The boundary probe builds a separate test module and boots it only in a disposable x86_64 Linux VM. It runs a synthetic
assembly call on each requested online CPU via `work_on_cpu`, switches to a temporary stack with low/high canaries,
deliberately clobbers callee-saved GPRs and RFLAGS, and checks normal and software-abort returns. Kernel snapshots
compare task/CPU identity, task stack bounds/RSP, CR3 root plus PCID, FS/GS/KERNEL_GS bases, preemption/IRQ state and
XCR0. The preemption-count check is nonzero only when the kernel is built with `CONFIG_PREEMPT_COUNT`; all builds still
compare its before/after value. The shim entry uses the SysV 16-byte stack rule; the Linux kernel call frame is checked
for bounds and restoration without imposing that user ABI on internal kernel calls. Userspace captures and compares
the full standard XSAVE image for all XCR0-enabled user components around the raw ioctl syscall. The probe deliberately
seeds and overwrites x87 ST(0), the x87 control word, MXCSR, and YMM0–YMM7. Other enabled user components have passive
before/after comparison. The test also checks Linux syscalls before and after each ioctl. The VM must unload both modules and
power off cleanly. TCG runs on every CI attempt; the script adds a KVM run when
`/dev/kvm` and host XSAVE/AVX are available. On a local host whose QEMU is older than 8.2,
setting `X86_BOUNDARY_ALLOW_KVM_ONLY=1` skips the unsupported TCG profile and instead requires
the KVM/host run. That explicit fallback is complementary local validation only; it is not a
substitute for the CI TCG profile.

The boundary-probe and exception-recovery disposable VMs use the Linux `no5lvl` boot parameter because version 1 of the control contract is explicitly four-level; this fixture constraint is not evidence that the contract supports five-level paging hosts. The boundary probe also exercises the validation-only FKI control-block and transaction-state models around the
same normal-return and software-abort calls. The test builds a synthetic `HOST_BOUND_SHIM`/`ENTER` object from live
host control state plus probe-owned XSAVE and alternate-stack memory, validates it, and requires the synthetic state
machine to reach `COMPLETE` for normal return or `ABORTED` for controlled software abort. This coupling is a fixture
shape check around the existing probe, not a production entry transaction. On hosts enabling later XCR0 features, only the x87/SSE/AVX projection is represented in version-1 control state; the oracle still compares the complete live XCR0.

The boundary probe described above is a host-only call/return test. It does not enter XNU, change `fki.h`, enable FKI
capabilities, change CR3 or GS, inspect supervisor XSS state, install or replace an IDT or exception handler, or run C
under foreign host state. It does not populate or restore a production XSAVE image, implement resume/fault/shutdown,
or own production exceptions. Passing it does not establish native XNU runtime support or prove the proposed early
exception/syscall bridge.

The same disposable VM also runs a synthetic task-stack continuation model. It covers a reply
published after the owner's initial completion check but before its first wait, a delayed reply
while the owner sleeps and a second task on that CPU is refused entry to the reserved stack/context,
a timeout followed by a rejected late reply, and cancellation acknowledged by the owner before a
late reply is rejected. The checks include the owner and responder task stacks, one resume with the
exact reply payload/generation/request ID, unchanged alternate-stack canaries, and a voluntary
context switch on the delayed/timeout/cancellation paths. The reply timer starts after the
contender has been refused. For immediate and delayed replies, the owner resumes after the
responder checks for a duplicate reply. Request and reply storage remains allocated until the
responder acknowledges completion, timeout handling, or cancellation. This models the
proposed ownership rules. The synthetic callback runs with local interrupts and preemption disabled;
both are restored after returning to the owner's task stack and before any wait. It does not call
`fki_arch_linux_runs()` or validate a production FKI continuation.

The separate exception-recovery probe runs in a disposable x86_64 VM and temporarily loads a test-owned shadow IDT on
the selected vCPU to test controlled `#PF`/`#GP` recovery. It needs matching x86_64 kernel headers as `KDIR`, QEMU 8.2
or newer, `cpio`, static C libraries and the normal module build tools. Run it as root or with usable `sudo` so it can
create `/dev/console` and `/dev/null` in the temporary initramfs.

The one-argument command reads the release from `KDIR/include/config/kernel.release` and downloads the matching
unsigned kernel image (`linux-image-unsigned-$KERNEL_RELEASE` on Ubuntu or `linux-image-$KERNEL_RELEASE-unsigned` on
Debian) and `busybox-static` through APT. To use local boot inputs, pass both files:

```sh
tests/x86_64/exception-recovery/test-vm.sh /path/to/kernel-headers /path/to/vmlinuz /path/to/busybox-static
```

Supply a static x86_64 BusyBox with `--install -s`, `sh`, `cat`, `cut`, `grep`, `uname`, `mount`, `insmod`, `rmmod`,
`dmesg`, `sync` and `poweroff`. The guest verifies that its kernel release matches the headers before loading a module.

The separate [test-only entry/return contract](../../docs/x86_64-entry-return-contract.md) defines the remaining gates
and lists the Tahoe artifact and ABI evidence still required before any XNU handoff can be considered.

`build-fki.sh KDIR` now builds the x86_64 FKI host backend. `build-xnu-objects.sh KDIR` compiles `macho.o` and `adt.o`
as validation objects without linking them into `xnu.ko`; on x86_64 `macho_rebase()` returns `-EOPNOTSUPP` rather than
simulating PAC signing. `build-xnu-main.sh KDIR` remains a diagnostic probe and currently exposes AArch64-specific
assumptions in `xnu_main.c`; it is not yet an x86_64 pass condition. The x86_64 fail-closed XNU target rejects CPUs
that lack the documented feature floor before reaching its world-switch refusal. The native port must keep XNU execution
capability disabled until its context switch, memory isolation and exception paths are implemented.

`tools/dev/x86_64/xnu-macho-inspect` is a host-side read-only inspector for a candidate `kernel.development` artifact.
Build it with the instructions in `tools/dev/x86_64/README.md`, then run:

```sh
/tmp/xnu-macho-inspect /path/to/kernel.development
```

Its JSON reports the parser-accepted x86_64 subtype, unslid thread RIP, aggregate and per-segment metadata, and whether
fixup/linkedit commands are present. The tool exits nonzero for unsupported or malformed inputs and never maps,
relocates, interprets fixups, or executes the image. A successful report is structural inspection only, not loadability
or runtime evidence. `tests/x86_64/test-macho-inspect.sh` runs synthetic positive and negative fixtures; CI runs it with
the x86_64 parser job. No Tahoe artifact has yet been confirmed against this subset.

## Evidence and limits

- The fake-QEMU launcher suite passes preflight errors, OVMF auto-detection including qcow2 firmware arguments and persistent NVRAM format detection, TCG
  and KVM argument construction, paths containing spaces and commas, SMC config permissions/redaction/cleanup,
  source-disk immutability, same-content base restoration, rejection
  of changed base contents, detection of unassociated legacy overlays, rejection of a managed overlay selected as its
  own base, a backed qcow2 source with an overlay-shaped filename, an overlay symlinked from another volume, and a
  hash-named disk stored inside a symlinked `STATE_DIR`.
- `test-qemu-overlay-symlink.sh` uses real `qemu-img` and `qemu-io` to open a backing chain through a symlink, write
  and reopen the overlay, confirm the backing image remains unchanged, and verify that the launcher rejects the
  managed overlay selected through either its symlink or external target.
- QEMU 10.0.13 booted a disposable Debian 6.12.118 x86_64 guest with a BusyBox initramfs under TCG. The guest printed
  `XNU_LINUX_TCG_BOOTED` and powered off cleanly.
- The actual QEMU 10.0.13 launcher passed `--check` with real `qemu-img`, and a paused TCG invocation accepted its Q35,
  OVMF, AppleSMC, AHCI, E1000, USB and VGA arguments. This checked command/device setup only.
- The x86_64 FKI module now builds against Debian 6.12.118 x86_64 headers. It reports zero execution capabilities,
  and the current XNU startup requires `FKI_CAP_PAC`. `xnu_main.c` and `world.S` still contain AArch64-only code;
  native macOS execution is not implemented. The fail-closed x86_64 XNU stub introduced in PR #47 is extended here
  with CPU-feature gating and is exercised in disposable VMs on both feature-complete and feature-limited CPUs; neither
  run validates native execution.
- ARM64 `fki.ko` and `xnu.ko` cross-compiled against Debian 6.12.118 ARM64 headers. This is a build check, not an ARM64
  runtime smoke test.
- The XNU-stub test boots a feature-complete Intel Haswell profile in TCG and requires the final world-switch refusal.
  The optional KVM probe requires write access to `/dev/kvm` and all CPU features checked by the script. Tahoe boot,
  host-integrity checks, and physical Intel/AMD/ARM acceptance are still pending.

## Acceptance evidence still required

- Boot Tahoe 26 to the desktop on physical Intel and AMD PCs separately. Record CPU model/features, Linux kernel,
  RAM, guest build, display/input/network/disk results, shutdown and host-integrity checks.
- Re-run the documented macOS 27 ARM64 desktop smoke on supported ARM64 hardware and record the result.
- Keep support rows separate and claim only combinations with complete logs and passing results.
