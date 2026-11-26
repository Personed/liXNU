# Experimental x86_64 QEMU/KVM guest launcher

This optional launcher runs a user-prepared macOS Tahoe 26 guest through QEMU/KVM. It does not build or load the
architecture-matched `xnu.ko` and `fki.ko` required by the native x86_64 hosted-XNU target. It remains useful as a
separate guest experiment; it does not establish native x86_64 support. The ARM64 direct-kernel path remains unchanged.

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

```sh
bash tests/x86_64/test-launcher.sh
bash tests/x86_64/test-tcg-smoke.sh
tests/x86_64/tcg-smoke.sh /path/to/x86_64/vmlinuz /path/to/initrd.cpio.gz
```

The smoke initramfs must print `XNU_LINUX_TCG_BOOTED` on its serial console and shut down before the timeout. TCG boots
only a disposable Linux guest; it does not demonstrate macOS boot, KVM, Intel/AMD compatibility or useful performance.

The native x86_64 prototype is separate from the active TCG/PureDarwin path. The FKI backend build in [PR #48](https://github.com/Personed/xnulinux/pull/48)
passes on x86_64 but advertises no XNU execution capability. [PR #47](https://github.com/Personed/xnulinux/pull/47)
adds an x86_64 `xnu.ko` stub that deliberately returns `EOPNOTSUPP`; the disposable-VM check verifies refusal, not
XNU execution. Architecture-matched XNU entry, world switch, address-space and fault paths remain unimplemented. Do
not describe the x86_64 module build as native support, and do not reuse one `KDIR` across x86_64 and ARM64 builds.
The QEMU/KVM launcher below remains a separate guest experiment.

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
- An initial direct x86 FKI build, before the native backend work, failed modpost because architecture hooks were
  missing. The host-side FKI backend build in [PR #48](https://github.com/Personed/xnulinux/pull/48) passes against
  Debian 6.12.118 x86_64 headers. Its capability mask is zero. [PR #47](https://github.com/Personed/xnulinux/pull/47)
  adds a fail-closed x86_64 XNU module target; neither change enters XNU. `xnu_main.c` and `world.S` still contain
  AArch64-only code, and the x86_64 XNU entry, world switch, address-space and fault paths remain unimplemented. The
  optional QEMU/KVM launcher is a separate guest experiment and is not a substitute for that work.
- ARM64 `fki.ko` and `xnu.ko` cross-compiled against Debian 6.12.118 ARM64 headers. This is a build check, not an ARM64
  runtime smoke test.
- This environment has no `/dev/kvm`, no Tahoe disk and no physical Intel/AMD/ARM hardware. No native hosted-XNU boot,
  QEMU/KVM macOS guest run, or hardware-integrity acceptance has been recorded.

## Native x86_64 acceptance evidence still required

- Build architecture-matched XNU, machine-layer, kext, and collection outputs for x86_64, extending the existing FKI
  host-backend build, while retaining ARM64 builds.
- Verify entry/return, fault recovery, Linux register/FPU/memory state, host syscalls, disk, input, network, display, and clean shutdown.
- Boot Tahoe 26 to the desktop through the native hosted-XNU path on physical Intel and AMD PCs separately. Record CPU model/features, Linux kernel, RAM, page size, matching system build, host-integrity checks, and results.
- Re-run the documented macOS 27 ARM64 desktop smoke on supported ARM64 hardware and record the result.
- Keep support rows separate and claim only combinations with complete logs and passing results.
