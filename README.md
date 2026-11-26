# xnulinux

Run xnulinux on common x86-64 PCs through QEMU TCG by booting an ARM64 Linux guest, then running FKI/XNU inside that guest through the PureDarwin console. Preserve xnulinux's existing direct ARM64 hosted-XNU path. A macOS Tahoe guest through x86-64 QEMU/KVM is a separate experiment.

On ARM64, xnulinux builds XNU, the open-source kernel of macOS, from Apple's published sources and runs it as a loadable kernel
module (`xnu.ko`) inside an unmodified arm64 Linux kernel. The hosted XNU drives the CPU and MMU directly, so macOS
processes run natively next to Linux processes on the same CPUs. Linux lends XNU its memory, timers, files, network and
GPU through a small interface module (`fki.ko`).

On x86-64 PCs, QEMU TCG boots an ARM64 Linux guest, where `fki.ko` and `xnu.ko` run. The target validation requires a real
PureDarwin console command, guest-issued halt, XNU unload, and Linux guest power-off with a clean QEMU exit. Firmware,
Linux userspace, FKI allocation, XNU execution, the PureDarwin console, and a macOS desktop are separate validation stages; see
[`docs/portability-validation.md`](docs/portability-validation.md).

The separate x86-64 QEMU/KVM experiment runs a user-prepared macOS Tahoe guest. It does not use the project's ARM64
modules and does not establish the TCG/PureDarwin path.

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

| Linux host | Guest target | Status |
|---|---|---|
| ARM64 with PAC and 4 KiB pages | macOS 27 ARM64 (direct hosted XNU) | Existing supported path; tested on Apple Silicon Linux VMs and NVIDIA DGX Spark as described above. |
| Intel/AMD x86_64 with QEMU TCG | ARM64 Linux → FKI/XNU → PureDarwin console | Active x86-64 portability target; full acceptance is tracked in #36. The current profile uses 16 GiB guest RAM plus host overhead and does not claim every PC or practical TCG speed. |
| Intel/AMD x86_64 with VT-x/AMD-V and QEMU/KVM | User-prepared macOS Tahoe 26 x86_64 | Separate experimental path; physical desktop and host-integrity acceptance remain unverified. |

QEMU TCG does not require VT-x/AMD-V. The reference profile allocates 16 GiB to the ARM64 Linux guest. Its host
capacity screen requires at least 20 GiB total physical RAM and 4 GiB available before artifact preparation; record
these host measurements separately from guest RAM. This screen is not a measured universal minimum. The first
#36 run (#69) built the pinned image and HFS+ disk, but `kclink` failed on a `__TEXT_EXEC`/`__PRELINK_INFO` overlap.
At head `5312b29`, [run #72](https://github.com/Personed/xnulinux/actions/runs/45845795576) completed the source build,
prepared the guest, and passed the full console command, guest halt, XNU unload, Linux power-off, and QEMU exit checks
on Ubuntu 24.04 x86-64. A later Mach-O inspection found overlapping `__TEXT_EXEC`/`__KLD` ranges. Current #36 head
`97727ac` adds a file-backed range check; run #74 failed before boot on that overlap, so run #72 does not validate
the current layout. PR #50 proposes relocating `__KLD`/`__LAST` and relinking; run #86 is rebuilding the pinned
guest image. PR #52 pins the ARM64 guest kernel and records input hashes; run #81 built that image and is building
the PureDarwin disk/collection, but #52 is still based on an older #50 head and needs a rebase before its results
can validate the canonical relink. Cross-host PR #46 run #79 built the guest image but failed the XNU source build
at the same `__TEXT_EXEC`/`__KLD` overlap; preparation and host boots were skipped. It is based on the
pre-relocation/pre-pin head and provides no host acceptance. PR #58 adds guest-input hash checks and reliable
per-host preflight reports; run #85 is building its guest image, with review still requesting macOS preflight
regression coverage and precise RAM measurement ordering.
After #50/#52/#58 are synchronized, rerun the full console/halt route and the host matrix with identical guest hashes.
Its public runners are reported as 16 GB for Windows and 14 GB for Intel macOS, less than the 16 GiB guest allocation
plus host overhead; use larger runners or mark those combinations unsupported for this profile if they cannot boot.
Until #52 is integrated and its package check passes, the #36 guest workflow still selects the latest Ubuntu ARM64
kernel at run time, so reruns are not yet pinned to identical kernel inputs. This is not a universal RAM minimum or a
performance, GUI, networking, or Tahoe/KVM claim. See
[PR #3](https://github.com/Personed/xnulinux/pull/3) for Windows/macOS/Linux host setup and
[`docs/portability-validation.md`](docs/portability-validation.md) for stage-specific evidence.

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

### x86_64 QEMU TCG path

- The reusable host launcher and setup for Windows/macOS/Linux are documented in [PR #3](https://github.com/Personed/xnulinux/pull/3). Its separate CI smoke reaches the ARM64 UEFI shell on Linux x86-64, Windows x86-64, and macOS Apple Silicon; the Apple Silicon result is not x86-64 host coverage. Intel macOS and Windows ARM64 are not in that launcher matrix. This UEFI smoke does not boot PureDarwin; full-console cross-host checks are proposed in [PR #46](https://github.com/Personed/xnulinux/pull/46) and remain pending.
- For a step-by-step Linux guest installation, follow [PR #3's QEMU guide](https://github.com/Personed/xnulinux/blob/2b5fad8a2189ff21bead02f77a280bc026d96fcd/docs/qemu.md). Inside the ARM64 guest, follow the [PureDarwin build and boot guide](docs/free-kc.md#using-it); this validation guide records the console markers, clean shutdown checks, and hardware limits.
- QEMU 10 or newer for the reusable UEFI launcher, with AArch64 system emulation, matching AArch64 UEFI code and variables, and Python 3. VT-x/AMD-V is not required. The separate direct-kernel full-boot job passed with QEMU 8.2.2.
- The PureDarwin reference profile allocates 16 GiB to the ARM64 Linux guest plus additional physical RAM for the host OS and QEMU. Smaller Linux/FKI profiles do not validate PureDarwin. Record physical host RAM and available RAM separately from guest RAM; this is a measured profile, not a universal minimum.
- Use an ARM64 Linux kernel with 4 KiB pages and PAC at EL1, built with matching FKI/XNU modules. The [#36 guest workflow](https://github.com/Personed/xnulinux/blob/codex/puredarwin-x86-tcg-e2e/.github/workflows/puredarwin-arm64-image.yml) currently selects the latest Ubuntu generic ARM64 kernel available at run time and records its release and configuration digest; pinning the package or repository snapshot is still needed for identical reruns.
- The full x86-64 TCG route, tested guest profile, kernel reporting fields, FKI carve-out, and stage limits are documented in [`docs/portability-validation.md`](docs/portability-validation.md).

### Separate experimental x86_64 QEMU/KVM path

- An Intel or AMD x86_64 Linux PC with VT-x or AMD-V, `/dev/kvm` accessible to the running user, and SSE4.2, AVX, FMA,
  MOVBE, POPCNT, LZCNT, BMI1, AVX2 and BMI2 exposed by the host CPU.
- QEMU 10 or newer, `qemu-img`, `flock` (from util-linux), and OVMF 4 MiB firmware files. The launcher auto-detects
  common Debian/Ubuntu, Fedora/RHEL, Arch, and openSUSE layouts; override `OVMF_CODE` and `OVMF_VARS_TEMPLATE`, or add
  a custom directory with `OVMF_DIR`, when needed.
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

### Separate experimental x86_64 macOS guest (QEMU/KVM)

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

If OVMF is not auto-detected, set `OVMF_CODE` and `OVMF_VARS_TEMPLATE` to the corresponding 4 MiB files before
preflight. `OVMF_DIR` adds one custom directory to the search path. Fedora's qcow2 firmware pair is passed to QEMU as
qcow2; raw pairs remain raw, and existing NVRAM state keeps its own format. If preflight succeeds, run:

```sh
scripts/x86_64/xnulinux-boot.sh
```

KVM and QEMU's `host` CPU model are the defaults. `CPU_MODEL`, `CPUS`, `RAM_MB`, `STATE_DIR`, and `QEMU_DISPLAY` can be
overridden. `ACCEL=tcg` is only for development and Linux smoke tests; it is not a performance or hardware-support
result. For AMD, OpenCore must already contain AMD kernel patches. The current x86_64 path is separate from the ARM64
`fki.ko`/`xnu.ko` runtime.

### Testing the Linux host layer in QEMU

GitHub Actions uses QEMU's software emulation (TCG) on x86-64 Linux, x86-64 Windows, and x86-64 macOS
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
