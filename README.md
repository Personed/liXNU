# xnulinux

Run XNU and Darwin userspace on Linux directly on compatible ARM64 hosts, or in an ARM64 Linux VM on Intel/AMD x86-64 PCs. The PC path uses QEMU TCG software emulation and currently targets the PureDarwin console; a separate experimental QEMU/KVM path runs a user-prepared Tahoe x86-64 guest.

On ARM64, xnulinux builds XNU, the open-source kernel of macOS, from Apple's published sources and runs it as a loadable kernel
module (`xnu.ko`) inside an unmodified arm64 Linux kernel. The hosted XNU drives the CPU and MMU directly, so macOS
processes run natively next to Linux processes on the same CPUs. Linux lends XNU its memory, timers, files, network and
GPU through a small interface module (`fki.ko`).

On Intel/AMD x86-64 PCs, QEMU TCG runs an ARM64 Linux guest with the xnulinux modules and PureDarwin. It does not need
VT-x or AMD-V, but the documented profile allocates 16 GiB to the guest plus additional host RAM. It does not claim a
macOS desktop, graphics, networking, or practical performance on every PC. A separate experimental QEMU/KVM path runs
a user-prepared Tahoe 26 x86-64 guest without loading the ARM64-only project modules on x86-64.

On a supported ARM64 host with a stock Ubuntu kernel, `insmod xnu.ko` boots a macOS 27 system disk made by Apple's own installer to the
desktop: loginwindow, WindowServer, Dock, Finder, Safari and Chrome.

![macOS and Linux userspace side by side](docs/images/macos-and-linux.jpg)

*One system, two userspaces. Left: a Terminal in the hosted macOS 27.2.1 (`sw_vers`, the Darwin kernel, launchd,
WindowServer, Dock and Finder running). Right: from that macOS, an SSH session into the Linux it runs inside (Ubuntu,
Linux 7.0, the `xnu` and `fki` modules loaded, and the `insmod xnu.ko` process the macOS kernel runs in).
Straight from the hosted framebuffer, 1280×800.*

![The hosted macOS desktop](docs/images/desktop.jpg)

*The hosted desktop at the end of a 30-minute stress run, rendered on the CPU (no GPU bridge).*

| | |
|---|---|
| ![Safari on Wikipedia's XNU article](docs/images/safari-xnu.jpg) | ![Safari on kernel.org](docs/images/safari-kernel-org.jpg) |
| ![Safari on GitHub](docs/images/safari-github.jpg) | |

*Safari in the hosted macOS: Wikipedia's article on XNU, kernel.org and the Linux repository on GitHub, over the
hosted system's network link through Linux, with the GPU bridge on (`GPU=auto`).*

![Hosted macOS running on bare-metal NVIDIA DGX Spark](docs/images/dgx-spark.jpg)

*Bare-metal NVIDIA DGX Spark (Ubuntu 24.04.5 LTS aarch64, Linux `7.0.0-1025-nvidia`): hosted macOS 27 running with
4 CPUs and 8 GiB of RAM (CPU rendering), shown over macOS Screen Sharing alongside an SSH session from the DGX Spark
host into the hosted Darwin userspace.*

![About This Mac and sysctl on bare-metal NVIDIA DGX Spark](docs/images/dgx-spark-about.jpg)

*Inside the hosted macOS on DGX Spark: About This Mac (`Chip: Unknown`, `7.80 GB` memory, `macOS Golden Gate 27.2`)
alongside `sysctl machdep.cpu` (`ARM processor`, `hw.cpufamily: 0`, `hw.ncpu: 4`) and `hw.optional.arm`.*

## How it works

```
 macOS processes (launchd, WindowServer, Safari...)    Linux processes (systemd, sshd, xnudrmd...)
 ──────────────────────────────────────────────────    ────────────────────────────────────────────
 XNU (source-built, LINUX64 machine layer, kexts)  │
   ↕ linux64 host calls (xnu-patches/include/linux64_abi.h)
 xnu.ko   (APSL side: loads and runs the kernel collection)
   ↕ FKI, the foreign kernel interface (fki_*)
 fki.ko   (GPL side: CPUs, memory, faults, IPIs, files, network, host calls)
 Linux kernel (stock distro kernel, 4K pages, PAC)
```

- **CPUs**: each hosted CPU is a Linux kernel thread pinned to a Linux CPU. XNU runs on it until it calls the host
  (idle, I/O, a timer); Linux keeps its own exception vectors, and `fki.ko` forwards the faults and user exceptions of
  XNU code to XNU's handlers.
- **Memory**: XNU gets one physically contiguous block from Linux (CMA) and manages it with its own pmap; XNU and Linux
  share the user half of the address space layout (4K pages).
- **Devices**: kexts in `kexts/` give macOS a disk (a file on the Linux side), a display (a framebuffer Linux reads), a
  keyboard and pointer, an Ethernet link (a Linux network interface), a keystore and a GPU bridge.
- **GPU**: Metal in the hosted macOS runs on `gpu/xnu-cpu-metal` (metal2vulkan over Vulkan). Vulkan reaches the Linux
  host's GPU through the DRM bridge (`kexts/XnuDrm` → host call → `gpu/xnu-drm/xnudrmd` → the Linux render node), using
  Mesa's Venus driver; without a supported DRM bridge backend it falls back to Mesa lavapipe on the CPU.

## Status

Working on two arm64 Linux setups:

- **Apple Silicon Mac in an arm64 Linux VM (`krunkit`, 1 to 6 hosted CPUs)**:
  - macOS 27.2.1 boots to the desktop with auto-login in 31–48 s (an Apple Virtualization macOS VM on the same Mac:
    49 s).
  - Apps open and quit, Spotlight and the file system work; a 30-minute stress run (40 rounds of Safari, Notes,
    Calculator, TextEdit, System Settings, Spotlight and a disk walk) finishes with no panic and no crash report.
  - Runs as a systemd service: started at boot, stopped cleanly with the VM, restarted after a hosted-kernel panic, each
    run's log kept in `/var/log/xnulinux/`.
  - The GPU bridge gives hosted apps the host GPU (`Virtio-GPU Venus (Apple M5 Max)`) and is enabled by default
    (`GPU=auto`) when a virtio-gpu render node is present, falling back to CPU rendering (`GPU=off`) otherwise.
- **Bare-metal NVIDIA DGX Spark (Ubuntu 24.04.5 LTS aarch64, Linux `7.0.0-1025-nvidia`)**:
  - Boots to the full macOS desktop (Screen Sharing and SSH working) directly on bare-metal arm64 Linux with `CPUS=4`
    and 8 GiB of RAM reserved for XNU.
  - Currently renders on the CPU (Mesa lavapipe, `GPU=off`): `xnudrmd` only forwards `virtio-gpu` (Venus) ioctls today,
    so hardware GPU pass-through on the GB10 GPU requires a new DRM bridge backend.

### Architecture support

| Linux host | Guest/system | Status |
|---|---|---|
| ARM64 with PAC and 4 KiB pages | macOS 27 ARM64 | Existing supported path; tested on Apple Silicon Linux VMs and NVIDIA DGX Spark as described above. |
| Intel/AMD x86-64 with QEMU TCG and 16 GiB guest RAM plus host RAM | ARM64 Linux, then PureDarwin ARM64 console | Experimental; full console boot and guest-requested halt are awaiting the current CI run. No macOS desktop or performance claim. |
| Intel x86_64 with VT-x/KVM and required CPU features | macOS Tahoe 26 x86_64 | Experimental QEMU/KVM launcher; physical Intel desktop boot and host-integrity acceptance remain. |
| AMD x86_64 with AMD-V/KVM and required CPU features | macOS Tahoe 26 x86_64 | Experimental QEMU/KVM launcher; requires AMD OpenCore kernel patches and physical AMD acceptance. |

Host setup for the ARM64 VM is documented in [QEMU launcher PR #3](https://github.com/Personed/xnulinux/pull/3), and
the current acceptance profile is in [`docs/portability-validation.md`](docs/portability-validation.md). A passing TCG
console run validates that profile on its recorded runner; it does not establish a macOS desktop, graphics, networking,
or practical performance on every PC. The separate x86_64 QEMU/KVM launcher remains experimental and needs Tahoe desktop
boot and Linux host-integrity checks on physical Intel and AMD systems. See [`tests/x86_64/README.md`](tests/x86_64/README.md)
for that path.

Shipping it as a hosted service (what is distributed, what a user installs to run Apple's macOS) is in
[`docs/hosted-service.md`](docs/hosted-service.md).

What is still a stopgap is listed in [`docs/production.md`](docs/production.md). Notably: the OS cryptex is merged into
the system volume, AMFI is patched and SIP relaxed, and memory access is slower than a native VM (4K pages vs 16K).

## Requirements

### ARM64 direct path

- An arm64 Linux host with PAC (`FEAT_PAuth`), 4K pages, and at least 16 GB of RAM (8 GB reserved for the hosted
  kernel). Tested on:
  - **NVIDIA DGX Spark** (bare-metal Ubuntu 24.04.5 LTS aarch64, Linux `7.0.0-1025-nvidia`; CPU rendering for now).
  - **Apple Silicon Mac (M1 or later)** running an arm64 Linux VM (`krunkit` for GPU access via virtio-gpu Venus, or
    `tart`).
- A Mac with Xcode to make the system disk and build the GPU parts.
- A macOS 27 installation (an Apple Virtualization VM disk) of your own. **No Apple binaries are in this repository or
  distributed by it**: the kexts Apple ships, the macOS system disk and its trust cache come from your own installation.

### ARM64 guest on an Intel/AMD PC

- An Intel/AMD x86-64 host with QEMU's AArch64 system emulator (`qemu-system-aarch64`) and ARM64 UEFI firmware. QEMU TCG
  does not need VT-x or AMD-V. QEMU 10 or newer is used by the documented launcher profile.
- For the full PureDarwin console profile, allocate 16 GiB to the guest and keep additional physical RAM available for
  the host. The guest Linux kernel must use 4 KiB pages, expose ARM pointer authentication, and run at EL1. The profile
  reserves 8,156 MiB for FKI and 7,900 MiB for XNU.
- The end-to-end Actions profile currently runs on Ubuntu 24.04 x86-64. Windows and macOS hosts have UEFI launcher
  coverage; the complete PureDarwin boot on those host operating systems is not yet validated. TCG may be too slow for
  practical desktop use, and this profile does not establish graphics, networking, or macOS support.

### x86_64 QEMU/KVM path

- An Intel or AMD x86_64 Linux PC with VT-x or AMD-V, `/dev/kvm` accessible to the running user, and SSE4.2, AVX, FMA,
  MOVBE, POPCNT, LZCNT, BMI1, AVX2 and BMI2 exposed by the host CPU.
- QEMU 10 or newer, `qemu-img`, `flock` (from util-linux), and OVMF 4 MiB firmware files. Package names and OVMF paths vary by distribution;
  override `OVMF_CODE` and `OVMF_VARS_TEMPLATE` when needed.
- A user-prepared bootable macOS Tahoe 26 x86_64 raw or qcow2 image with a working EFI/OpenCore setup. AMD images must
  include matching OpenCore AMD kernel patches. No Apple system image is provided here.
- The 64-byte Apple SMC OSK value supplied locally through `APPLE_SMC_OSK`; do not commit it or paste it into logs. The launcher writes it to a mode-0600 temporary QEMU config, keeps it out of QEMU's argv and environment, and removes the config when QEMU exits.
- Enough physical RAM for the configured guest allocation plus Linux/QEMU overhead. The default guest allocation is
  8 GiB and can be changed with `RAM_MB`.

## Building and running

1. `scripts/vm-xnu-build.sh` (on the arm64 Linux host, once): sets up the macOS-DroidVM Linux toolchain and builds XNU
   (`xnu-12377.121.6`) natively on arm64.
2. `scripts/vm-xnu-rebuild.sh`: applies `xnu-patches/`, builds the kernel and `kexts/`, and links the kernel collection
   with Apple's kexts from your macOS installation (`scripts/kclink`). To link that collection without building
   anything, `scripts/apple-kc/link.sh`, on the machine that runs `xnu.ko`, takes a kit of the built kernel and kexts
   and macOS 27.2's kernelcache, read from Apple's IPSW ([docs/free-kc.md](docs/free-kc.md)).
3. `scripts/mk-macvm-disk.sh` (on the Mac): turns a macOS VM disk made by Apple's installer into the hosted system's disk
   and copies it into the Linux VM.
4. `scripts/xnulinux-boot.sh` (on the arm64 Linux host): builds `fki.ko` and `xnu.ko` against the running kernel, brings
   up the network (and the GPU bridge when there is a render node) and boots the hosted system. Its console is
   `scripts/xnu-console.sh`, its screen `tools/hosted/fb2png.py`, input `tools/hosted/xnu-input.py`. On a bare-metal
   host (such as a DGX Spark), set the carve-out (`mem=`, `carve_base=`, `carve_mb=`) to fit the machine's physical
   memory layout, and set `GPU=off` (`xnudrmd` supports only virtio-gpu for now).
5. `scripts/install-service.sh` (on the arm64 Linux host): runs it as `xnulinux.service`, configured in
   `/etc/xnulinux.conf` (CPUs, memory, disk, GPU). On the Mac, `scripts/install-krun-agent.sh` keeps the VM itself
   running.
6. GPU (optional, on the Mac): `gpu/venus/build-darwin.sh` builds the hosted Vulkan driver, `gpu/xnu-cpu-metal/` the
   Metal plugin (see its README).

### Experimental x86_64 guest

This path requires a bootable image prepared separately by the user. QEMU writes macOS changes to a persistent qcow2
overlay under `$XDG_STATE_HOME/xnulinux/x86_64` when `XDG_STATE_HOME` is set, or `~/.local/state/xnulinux/x86_64`
otherwise; OVMF variable state is stored there too. Only one QEMU process can use a state directory at a time; use a
readable, writable, and searchable `STATE_DIR` for each VM. The source image and firmware templates stay untouched. The overlay grows as the
guest writes data. Overlay identity is tied to the canonical source path, format, and a SHA-256 sidecar. If the source
file metadata changes, the launcher hashes it: identical restored bytes keep the existing overlay, while changed bytes
fail before QEMU starts. The initial launch and any metadata change can require reading the full source image to hash it;
later launches with unchanged metadata skip that read. Restore the matching source image or choose a new `STATE_DIR` to
start a fresh VM state. Unassociated overlays left by an older launcher stop startup with migration guidance rather
than silently creating empty guest state. Do not pass this VM's managed qcow2 overlay back as `DISK` with the same
`STATE_DIR`; use the original base disk, or use another state directory if you intend to fork that image.
The `.base` sidecar beside a generated `disk-<20 hex>.qcow2` identifies launcher-owned overlays; a user-prepared qcow2
with that filename pattern is accepted as a source when it has no matching sidecar. Existing overlays may be stored on
another volume and linked into `STATE_DIR` as long as the link resolves to a regular file. `--check` validates host
requirements and disk readability without creating state; disk-to-state association is checked when launching.

Set the disk, source format, and local Apple SMC OSK value, then preflight the host:

```sh
export DISK="$HOME/VMs/tahoe26.qcow2"
export DISK_FORMAT=qcow2
read -r -s -p 'Apple SMC OSK (64 bytes): ' APPLE_SMC_OSK
export APPLE_SMC_OSK
printf '\n'
scripts/x86_64/xnulinux-boot.sh --check
```

If OVMF is installed outside `/usr/share/OVMF/`, set `OVMF_CODE` and `OVMF_VARS_TEMPLATE` to the corresponding 4 MiB
files before preflight. If preflight succeeds, run:

```sh
scripts/x86_64/xnulinux-boot.sh
```

KVM and QEMU's `host` CPU model are the defaults. `CPU_MODEL`, `CPUS`, `RAM_MB`, `STATE_DIR`, and `QEMU_DISPLAY` can be
overridden. `ACCEL=tcg` is only for development and Linux smoke tests; it is not a performance or hardware-support
result. For AMD, OpenCore must already contain AMD kernel patches. The current x86_64 path is separate from the ARM64
`fki.ko`/`xnu.ko` runtime.

### Testing the Linux host layer in QEMU

GitHub Actions uses QEMU's software emulation (TCG) on x86-64 Linux, x86-64 Windows, and ARM64 macOS
to boot ARM64 Ubuntu with pointer authentication, load `fki.ko`, reserve memory outside Linux's RAM map,
allocate and free the same 16 MiB carve-out twice, and verify that `xnu.ko` reaches its firmware loader.
On Linux, build the probe against the supplied ARM64 kernel headers and run the check with QEMU, GNU cpio,
Python 3 and zstd:

```sh
make -C "$KDIR" M="$PWD/tests/qemu" KBUILD_EXTRA_SYMBOLS="$PWD/fki-ko/Module.symvers" modules
tests/qemu/run.sh /path/to/arm64/vmlinuz /path/to/arm64/static-busybox /path/to/fki.ko /path/to/xnu.ko tests/qemu/fki-carve-probe.ko
```

Build the modules against the supplied ARM64 kernel's headers. To run the check on a different host, prepare
the kernel and initramfs on Linux with `tests/qemu/prepare.sh KERNEL BUSYBOX FKI_KO XNU_KO PROBE_KO OUTPUT_DIR`.
Copy those files to the host and run `python3 tests/qemu/boot.py OUTPUT_DIR` with `qemu-system-aarch64` on its
PATH. The check stops at the firmware loader before booting PureDarwin or macOS.

## Repository layout and licenses

The Linux side and the XNU side are kept apart by license and meet only at FKI. Every source file carries an
`SPDX-License-Identifier`; the license texts are in [`LICENSES/`](LICENSES).

| Path | License | What |
|---|---|---|
| `fki-ko/`, `linux/` | GPL-2.0-only | FKI: the out-of-tree module for stock kernels, and the same as an in-tree driver |
| `xnu-ko/` | APSL-2.0 | `xnu.ko`: loads XNU's kernel collection and runs it; calls Linux only through `fki_*` |
| `xnu-patches/` | APSL-2.0 | The LINUX64 machine layer and the other changes to XNU's sources, and their shared headers |
| `kexts/` | APSL-2.0 | The hosted system's kexts (CPU, disk, keystore, Metal accelerator, DRM bridge) |
| `kexts/XnuDisplay/`, `kexts/XnuEthernet/`, `kexts/XnuInput/` | BSD-2-Clause-Patent | Derived from [macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM)'s kexts |
| `gpu/xnu-cpu-metal/` | GPL-2.0-or-later | The Metal plugin over metal2vulkan (GPL-2.0-or-later); `XnuCpuMetalShim.h` is BSD-2-Clause-Patent (from [AdrenoMetal](https://github.com/UniXNU/AdrenoMetal)) |
| `gpu/xnu-drm/` | GPL-2.0-only, MIT | `xnudrmd` (the Linux end of the DRM bridge) is GPL; the protocol header and `darwin/` (built into Mesa) are MIT |
| `gpu/venus/` | GPL-2.0-only; patches MIT | Build scripts and tests; the patches to Mesa and virglrenderer are under their upstream license (MIT) |
| `gpu/xnu-cpu-metal/patches/` | upstream's | Patches to Mesa (MIT) and metal2vulkan (GPL-2.0-or-later) |
| `scripts/`, `tools/`, `tests/`, `docs/` | GPL-2.0-only | Build, disk, boot, debugging and test tools; documentation |

`xnu.ko` is on the XNU side of the line: it runs XNU's code in its own address space, so it is APSL-2.0 like XNU, and
Linux reports it as a non-GPL module (`MODULE_LICENSE("Proprietary")`, which taints the kernel as ZFS does). It must not
include Linux headers beyond the module ABI, call Linux functions other than `fki_*`, or carry Linux code. Upstream code
(XNU, macOS-DroidVM's toolchain, Mesa, metal2vulkan, the Vulkan loader, LLVM) is fetched at pinned revisions and patched at
build time; it is never committed.

## Acknowledgements

- [macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM) and [UniXNU](https://github.com/UniXNU): the 4K-page XNU
  base, the Linux-hosted XNU toolchain, and the display, Ethernet and input kexts these are derived from.
- [metal2vulkan](https://github.com/steelbrain/metal2vulkan), [Mesa](https://mesa3d.org) (Venus, lavapipe),
  [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) and [krunkit](https://github.com/containers/krunkit).
- Apple's open-source XNU.

macOS, Metal and XNU are trademarks of Apple Inc. This project is not affiliated with or endorsed by Apple.

Copyright © 2026 Heyang Zhou and contributors.
