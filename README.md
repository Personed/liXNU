# xnulinux

Make xnulinux run on common x86-64 PCs using QEMU TCG to boot Linux ARM64, load FKI/XNU, and reach the PureDarwin shell, while preserving the existing direct ARM64 hosted-XNU path. This is the active portability goal for the documented profiles, not a claim of universal PC compatibility. Native x86_64/Tahoe hosting and QEMU/KVM remain separate work.

On ARM64, xnulinux builds XNU, the open-source kernel of macOS, from Apple's published sources and runs it as a loadable kernel
module (`xnu.ko`) inside an unmodified arm64 Linux kernel. The hosted XNU drives the CPU and MMU directly, so macOS
processes run natively next to Linux processes on the same CPUs. Linux lends XNU its memory, timers, files, network and
GPU through a small interface module (`fki.ko`).

On x86-64 PCs, the active route uses QEMU TCG to run an ARM64 Linux guest, load `fki.ko` and `xnu.ko`, and reach the PureDarwin console. VT-x/AMD-V is not required. Full-route passes are recorded on hosted Ubuntu, Windows Server 2025, and Intel macOS runners; the Windows/macOS passes use the `cortex-a710` TCG profile once in PR #98 run #158. Those results do not establish physical-PC support or validate the PIE-enabled `-cpu max` path.

The repository also contains a separate QEMU/KVM macOS guest launcher and an experimental native x86_64 hosted-XNU port. Those workstreams do not replace the active QEMU TCG/PureDarwin route. Firmware, Linux userspace, module builds, the XNU
module probe, the PureDarwin console, and a macOS desktop are separate validation stages; see
[`docs/portability-validation.md`](docs/portability-validation.md).

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

## Current ARM64 hosting model

This diagram and the following CPU/memory details describe the existing ARM64 runtime. The native x86_64 port needs its
own architecture-specific entry, exception, register, and memory-management implementation.

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
- **Memory**: XNU manages a physically contiguous region supplied through FKI. Its source is host-profile-specific:
  a pre-reserved range configured with Linux `mem=` and FKI `carve_base`/`carve_mb` on DGX Spark, or contiguous pages
  from CMA where that allocation path is used. See the [host-preparation guide](docs/hosted-service.md) and
  [DGX Spark profile](tools/dgx-spark/README.md). XNU and Linux share the user half of the address space layout (4K pages).
- **Devices**: kexts in `kexts/` give macOS a disk (a file on the Linux side), a display (a framebuffer Linux reads), a
  keyboard and pointer, an Ethernet link (a Linux network interface), a keystore and a GPU bridge.
- **GPU**: Metal in the hosted macOS runs on `gpu/xnu-cpu-metal` (metal2vulkan over Vulkan). The DRM backend is host-specific:
  `xnudrmd` serves virtio-gpu/Venus render nodes, while the DGX Spark profile uses `xnudrmd_virgl.c`, virglrenderer, and
  Venus to reach its NVIDIA GB10 GPU. Hosts without a configured backend fall back to Mesa lavapipe on the CPU.

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
  - Boots to the full macOS desktop (Screen Sharing and SSH working) directly on bare-metal arm64 Linux with 4 hosted CPUs.
    The tested carve-out uses `mem=112G`, `carve_base=0x1E00000000`, and `carve_mb=8156`.
  - The recorded desktop run used CPU rendering (`GPU=off`). A separate DGX Spark configuration exposes the GB10 through
    virglrenderer 1.2.0 and Venus with `GPU=auto`; see [the DGX Spark guide](tools/dgx-spark/README.md). That GPU path is
    specific to this host/backend and was not exercised by the CPU-rendered desktop evidence above.

### Architecture support

| Linux host | Guest target | Status |
|---|---|---|
| ARM64 with PAC and 4 KiB pages | macOS 27 ARM64 (direct hosted XNU) | Existing supported path; tested on Apple Silicon Linux VMs and NVIDIA DGX Spark as described above. |
| x86-64 Linux under QEMU TCG (hosted CI VM) | ARM64 Linux → PureDarwin console | PR #98 run [#158](https://github.com/Personed/xnulinux/actions/runs/45911026412) passed the complete 6 GiB route on tested head `589c40e` with `cortex-a710`; Ubuntu 24.04/QEMU 8.2.2, Windows Server 2025/QEMU 11.1.0, and Intel macOS 15/QEMU 11.1.0 all reached Darwin userland and guest shutdown using the same artifacts. This records one hosted-runner pass per profile; it is not evidence for representative physical PCs or a physical RAM floor. `-cpu max`/PIE remains a separate compatibility profile. |
| Physical Intel x86-64 Linux under QEMU TCG | ARM64 Linux → PureDarwin console | Not validated; no bare-metal Intel PC is attached to the available executor. |
| Physical AMD x86-64 Linux under QEMU TCG | ARM64 Linux → PureDarwin console | Not validated; AMD EPYC virtual CPUs in CI do not establish bare-metal AMD support. |
| Windows Server 2025 x86-64 under QEMU TCG | ARM64 Linux → PureDarwin console | PR #89 run [#151](https://github.com/Personed/xnulinux/actions/runs/45904189412) failed on QEMU 11.1.0 with `-cpu max` at an ARM64 write-permission fault in XNU. PR #98 run [#158](https://github.com/Personed/xnulinux/actions/runs/45911026412), head `589c40e` on #89 head `3fbfa6a`, passed the full 6 GiB route once with QEMU 11.1.0 and `cortex-a710`, including console, guest halt, XNU/FKI unload, Linux power-off, and QEMU exit 0. The hosted Windows runner had 17,174,360,064 bytes total and 13,663,916,032 available before QEMU; process RSS was not measured. This validates one hosted CPU/OS profile, not physical Windows PCs or the PIE-enabled `-cpu max` path. |
| Intel macOS 15 under QEMU TCG | ARM64 Linux → PureDarwin console | PR #89 run [#151](https://github.com/Personed/xnulinux/actions/runs/45904189412) failed on QEMU 11.1.0 with `-cpu max` at an ARM64 write-permission fault in XNU. PR #98 run [#158](https://github.com/Personed/xnulinux/actions/runs/45911026412), head `589c40e` on #89 head `3fbfa6a`, passed the full 6 GiB route once with QEMU 11.1.0 and `cortex-a710`, including console, guest halt, XNU/FKI unload, Linux power-off, and QEMU exit 0. The hosted runner had 15,032,385,536 bytes total and 10,498,539,520 available before QEMU; peak QEMU RSS was 3,640,304 KiB (3.47 GiB). This validates one hosted CPU/OS profile, not physical Intel Macs or the PIE-enabled `-cpu max` path. |
| Intel/AMD x86-64 with QEMU/KVM | User-prepared macOS guest | Separate launcher experiment; it does not use the hosted-XNU modules or validate the TCG route. |
| Intel/AMD x86-64 Linux | Tahoe 26 through native hosted XNU | Separate experiment: FKI backend #48 builds, XNU stub #47 remains fail-closed, and #78 adds read-only Mach-O parsing; no native XNU execution is supported. |

The separate native x86_64 experiment remains incomplete: the FKI host backend exposes no XNU execution capabilities, and
its architecture-specific XNU execution path plus physical Intel and AMD desktop boots remain pending. The active TCG route
validates ARM64 guest integration; it does not establish native x86_64 hosted-XNU support. See
[`tests/x86_64/README.md`](tests/x86_64/README.md) and [`docs/portability-validation.md`](docs/portability-validation.md)
for complementary launcher/tests and stage-specific evidence.

### Active QEMU TCG and PureDarwin path

The historical full TCG reference in [#36](https://github.com/Personed/xnulinux/pull/36) assigns 16 GiB to the ARM64 guest and records a 20 GiB total / 4 GiB available host-capacity screen before artifact preparation. These are profile-specific measurements and planning guidance, not a universal or physical minimum.

The reduced profile passed the console and guest-initiated shutdown checks on hosted Ubuntu x86-64 VMs. PR #52 runs [#138](https://github.com/Personed/xnulinux/actions/runs/45891610133) and [#148](https://github.com/Personed/xnulinux/actions/runs/45902417451) passed the route; #148 is the current-head pass on #52 head `24e4a9c`. PR #72 run [#140](https://github.com/Personed/xnulinux/actions/runs/45891917494) exercised the TTBR1 guard on a tree identical to the earlier #52 head `3b47444`. PR #75 run [#143](https://github.com/Personed/xnulinux/actions/runs/45897387799) passed on an earlier tree and added fixed package-digest checks. The tested init emits `XNULINUX_E2E_XNU_UNLOADED` only after both `rmmod xnu` and `rmmod fki` succeed. The 6 GiB profile has repeated full-route passes; PR #96's hosted sweep [#157](https://github.com/Personed/xnulinux/actions/runs/45907949989) passed 5, 4, 3, 2.5, and 2 GiB profiles. A separate local run of all six profiles under QEMU 6.2.0 in an x86-64 KVM VM passed using the paired #157 artifacts; [`docs/portability-validation.md`](docs/portability-validation.md) records RSS and hashes. The two 2 GiB observations remain experimental virtualized results, not a guest minimum or physical-PC support claim; host RAM was not reported for the local VM.

Current-head run [#148](https://github.com/Personed/xnulinux/actions/runs/45902417451) passed all four image, disk/collection, guest-preparation, and TCG-boot jobs. The Ubuntu 24.04.5 Azure/Microsoft-hosted VM reported 4 vCPUs, virtual AMD EPYC 7763, `MemTotal: 16367500 kB`, `MemAvailable: 15418148 kB` before artifacts and `14965924 kB` at boot. The 6 GiB QEMU profile peaked at 3,833,380 KiB RSS (3.66 GiB) and reached the Darwin console, guest halt, XNU/FKI unload, Linux power-off, and successful QEMU completion. This is current-tree virtual-runner evidence, not a physical RAM floor.

PR #89 run [#151](https://github.com/Personed/xnulinux/actions/runs/45904189412) is historical evidence: on head `3fbfa6a`, Ubuntu/QEMU 8.2.2 passed, while Windows and Intel macOS/QEMU 11.1.0 with `-cpu max` faulted during XNU loading. PR #98 run [#158](https://github.com/Personed/xnulinux/actions/runs/45911026412) later passed all three hosted OS profiles once at 6 GiB using `cortex-a710`, with Darwin console, userland marker, guest halt, XNU/FKI unload, and successful QEMU completion. This counts as the complete hosted result for that tested CPU model; it does not isolate S1PIE, validate the separate PIE-enabled `-cpu max` profile, or establish physical-PC support. The #89 branch contains the #98 changes; its follow-up CI status is an integration record and is not a gate for counting run #158. PR #102 current head `7ee3e4f` adds strict QEMU 11.1 `--require-pie` support using `-cpu max` and direct-mode harness checks with `--require-direct`. Local tests (10 PIE/direct cases, RSS 5/5, kernel segments 10/10), cross-builds, patch checks, shell syntax checks, and actionlint passed, but no Actions run is associated with this head and Devin Review is the only green check visible for this SHA. A clean source XNU/DVM build and real PIE boot through table updates, console, and guest shutdown remain unvalidated. This separate profile does not gate the A710 result. Run #161 tested old head `9e15d68` and was canceled during disk/collection build. PR #89 current head `ac74d61` changes documentation only; the completed #158 A710 result remains hosted acceptance evidence for its tested profiles. Physical Intel/AMD validation and a RAM floor remain unknown; PR #96's 2 GiB result and the supplemental local 2 GiB run remain experimental, not a minimum.

To reproduce the tested TCG boot on an x86-64 Linux PC, use the harness/source at [PR #98 head `589c40e`](https://github.com/Personed/xnulinux/commit/589c40ead54a63946fb44e8bdb35eb6add9adffe) and download the paired [guest artifact](https://github.com/Personed/xnulinux/actions/runs/45911026412/artifacts/15161900498) and [PureDarwin disk artifact](https://github.com/Personed/xnulinux/actions/runs/45911026412/artifacts/15161666268) from run [#158](https://github.com/Personed/xnulinux/actions/runs/45911026412) before they expire on 2026-12-27. Then follow [the QEMU TCG procedure](docs/qemu.md#boot-the-puredarwin-arm64-guest-with-tcg), which gives the exact command and tested `cortex-a710` profile. This hosted-runner pass does not prove bare-metal PC support or `-cpu max`/PIE behavior.

Shipping it as a hosted service (what is distributed, what a user installs to run Apple's macOS) is in
[`docs/hosted-service.md`](docs/hosted-service.md).

What is still a stopgap is listed in [`docs/production.md`](docs/production.md). Notably: the OS cryptex is merged into
the system volume, AMFI is patched and SIP relaxed, and memory access is slower than a native VM (4K pages vs 16K).

## Requirements

### ARM64 direct path

Run `scripts/xnulinux-boot.sh --check` on the Linux host (inside the Linux VM if you use one). It checks the
architecture, page size, headers for the running kernel, and the compiler selected by Kbuild before you prepare a disk. It also
reports when Linux does not advertise address pointer authentication. Use the [host-preparation guide](docs/hosted-service.md#3-operator-preparing-a-host)
for FKI carve-out setup; booting XNU also depends on the FKI backend and its memory setup.

- An arm64 Linux host with PAC (`FEAT_PAuth`), 4K pages, and at least 16 GB of RAM (8 GB reserved for the hosted
  kernel). Tested on:
  - **NVIDIA DGX Spark** (bare-metal Ubuntu 24.04.5 LTS aarch64, Linux `7.0.0-1025-nvidia`; the recorded desktop used
    CPU rendering; an optional GB10/Venus backend is documented in [the DGX guide](tools/dgx-spark/README.md)).
  - **Apple Silicon Mac (M1 or later)** running an arm64 Linux VM (`krunkit` for GPU access via virtio-gpu Venus, or
    `tart`).
- A Mac with Xcode to make the system disk and build the GPU parts.
- A macOS 27 installation (an Apple Virtualization VM disk) of your own. **No Apple binaries are in this repository or
  distributed by it**: the kexts Apple ships, the macOS system disk and its trust cache come from your own installation.

### Separate experimental x86_64 native hosted-XNU port

- Linux on an Intel or AMD x86_64 PC. Kernel versions, CPU features, RAM, and page-size requirements are being established by the port.
- User-provided macOS Tahoe 26 x86_64 artifacts that match the selected XNU build. No Apple system image or binary is provided here.
- The x86_64 FKI host-backend build merged in [#48](https://github.com/Personed/xnulinux/pull/48), and the fail-closed x86_64 XNU stub merged in [#47](https://github.com/Personed/xnulinux/pull/47). Neither milestone provides native XNU execution. The QEMU/KVM launcher below is a separate optional guest experiment.

### Experimental Intel/AMD PC route via an ARM64 Linux VM (QEMU TCG)

The route boots ARM64 Linux on the x86-64 host with QEMU TCG, then runs the hosted-XNU modules inside that guest. It
does not require VT-x or AMD-V and is separate from the native x86_64 hosted-XNU target above. Run #158 passed the full
console-and-shutdown route once on hosted Ubuntu 24.04, Windows Server 2025, and Intel macOS 15 using a 6 GiB guest and
QEMU's `cortex-a710` profile. This records the completed hosted-runner profiles; physical Intel/AMD PC validation and a
physical host-memory minimum remain unvalidated. PIE-enabled `-cpu max` is a separate compatibility profile. See the
[QEMU guide](docs/qemu.md#boot-the-puredarwin-arm64-guest-with-tcg) for exact artifact and command steps.

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
   memory layout. For a CPU-rendered baseline, set `GPU=off`; the optional DGX GB10/Venus backend is documented in
   [the DGX Spark guide](tools/dgx-spark/README.md). Other bare-metal GPU models remain unvalidated.
5. `scripts/install-service.sh` (on the arm64 Linux host): runs it as `xnulinux.service`, configured in
   `/etc/xnulinux.conf` (CPUs, memory, disk, GPU). On the Mac, `scripts/install-krun-agent.sh` keeps the VM itself
   running.
6. GPU (optional, on the Mac): `gpu/venus/build-darwin.sh` builds the hosted Vulkan driver, `gpu/xnu-cpu-metal/` the
   Metal plugin (see its README).

### Experimental x86_64 macOS guest (QEMU/KVM)

This optional launcher is separate from the native hosted-XNU port and does not count as x86_64 module/runtime acceptance.
Its prerequisites are an Intel or AMD x86_64 Linux PC with VT-x or AMD-V and `/dev/kvm`, SSE4.2, AVX, FMA, MOVBE,
POPCNT, LZCNT, BMI1, AVX2 and BMI2; QEMU 10 or newer, `qemu-img`, `flock`, and OVMF 4 MiB firmware; a user-prepared
Tahoe 26 x86_64 disk with EFI/OpenCore (including AMD kernel patches on AMD); and a locally supplied 64-byte Apple SMC
OSK in `APPLE_SMC_OSK`. The launcher auto-detects common Debian/Ubuntu, Fedora/RHEL, Arch, and openSUSE OVMF layouts;
override `OVMF_CODE` and `OVMF_VARS_TEMPLATE`, or add a custom directory with `OVMF_DIR`, when needed. The default
guest allocation is 8 GiB and can be changed with `RAM_MB`; allow additional memory for Linux/QEMU.

The launcher requires a bootable image prepared separately by the user. QEMU writes macOS changes to a persistent qcow2
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
