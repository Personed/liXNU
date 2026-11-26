# x86_64 QEMU/KVM runtime

The x86_64 path runs a user-prepared macOS Tahoe 26 guest through QEMU/KVM. It does not build or load the project's
ARM64-only `xnu.ko` or `fki.ko`. The ARM64 direct-kernel path remains unchanged. Intel and AMD are tracked as separate
acceptance targets; neither is marked supported until tested on physical hardware.

## Host prerequisites

- x86_64 Linux with VT-x or AMD-V and a readable/writable `/dev/kvm` for native acceleration.
- Host CPU flags: SSE4.2, AVX, FMA, MOVBE, POPCNT, LZCNT (`lzcnt` or `abm` in Linux flags), BMI1, AVX2 and BMI2.
- QEMU 10 or newer, `qemu-img`, `flock` (from util-linux), and OVMF 4 MiB firmware. Package names and OVMF locations vary by distribution.
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

For a raw image, set `DISK_FORMAT=raw`. Override `OVMF_CODE`, `OVMF_VARS_TEMPLATE`, `STATE_DIR`, `QEMU_DISPLAY`,
`CPU_MODEL`, `CPUS`, or `RAM_MB` when the host needs different paths or resources. The default state directory is
`$XDG_STATE_HOME/xnulinux/x86_64` when `XDG_STATE_HOME` is set, or `~/.local/state/xnulinux/x86_64` otherwise. It
stores writable OVMF variables and a persistent qcow2 overlay backed by the source disk. Only one QEMU process may use a
state directory at a time; use one readable, writable, and searchable `STATE_DIR` per VM. Guest writes do not change the source image. Keep the source
image at its current path while the overlay exists; QEMU records that backing path. Overlay identity is
tied to the canonical source path, format, and a SHA-256 sidecar. If file metadata changes, the launcher checks the
contents: identical restored bytes reuse the overlay, but changed bytes fail before QEMU starts. Hashing reads the full
source image on its first launch and after metadata changes; later launches with unchanged metadata skip the read.
Restore the matching base image or set a new `STATE_DIR` to start fresh. `--check` validates requirements without
creating state files. An unassociated overlay from an earlier launcher stops startup with migration guidance. Do not
select a managed qcow2 overlay as `DISK` with the same `STATE_DIR`; use the original base, or choose another state
directory to fork it intentionally.

## Development tests

```sh
bash tests/x86_64/test-launcher.sh
bash tests/x86_64/test-tcg-smoke.sh
tests/x86_64/tcg-smoke.sh /path/to/x86_64/vmlinuz /path/to/initrd.cpio.gz
```

The smoke initramfs must print `XNU_LINUX_TCG_BOOTED` on its serial console and shut down before the timeout. TCG boots
only a disposable Linux guest; it does not demonstrate macOS boot, KVM, Intel/AMD compatibility or useful performance.

The historical `build-fki.sh KDIR` and `build-xnu-main.sh KDIR` scripts probe the rejected direct x86_64 module path.
They are expected to expose missing FKI architecture hooks and AArch64-only XNU assembly on an x86_64 kernel tree;
they are not pass/fail tests for the selected QEMU/KVM path.

## Evidence and limits

- The fake-QEMU launcher suite passes preflight errors, TCG and KVM argument construction, paths containing spaces and
  commas, SMC config permissions/redaction/cleanup, source-disk immutability, same-content base restoration, rejection
  of changed base contents, detection of unassociated legacy overlays, rejection of a managed overlay selected as its
  own base, and a hash-named disk stored inside a symlinked `STATE_DIR`.
- QEMU 10.0.13 booted a disposable Debian 6.12.118 x86_64 guest with a BusyBox initramfs under TCG. The guest printed
  `XNU_LINUX_TCG_BOOTED` and powered off cleanly.
- The actual QEMU 10.0.13 launcher passed `--check` with real `qemu-img`, and a paused TCG invocation accepted its Q35,
  OVMF, AppleSMC, AHCI, E1000, USB and VGA arguments. This checked command/device setup only.
- The direct x86 FKI module check found missing architecture hooks, and `xnu_main.c` / `world.S` contain AArch64-only
  code. That is why the x86 path uses QEMU/KVM instead of direct x86 modules.
- ARM64 `fki.ko` and `xnu.ko` cross-compiled against Debian 6.12.118 ARM64 headers. This is a build check, not an ARM64
  runtime smoke test.
- This environment has no `/dev/kvm`, no Tahoe disk and no physical Intel/AMD/ARM hardware. No macOS boot, native KVM
  run or hardware-integrity acceptance has been recorded.

## Acceptance evidence still required

- Boot Tahoe 26 to the desktop on physical Intel and AMD PCs separately. Record CPU model/features, Linux kernel,
  RAM, guest build, display/input/network/disk results, shutdown and host-integrity checks.
- Re-run the documented macOS 27 ARM64 desktop smoke on supported ARM64 hardware and record the result.
- Keep support rows separate and claim only combinations with complete logs and passing results.
