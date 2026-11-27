<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Running the ARM64 Linux host in QEMU

`scripts/qemu-vm.py` starts the ARM64 Linux VM that hosts xnulinux. It uses QEMU TCG to emulate
an ARM64 CPU with pointer authentication, including on Intel and AMD PCs. The VM runs Linux at EL1
and keeps its system disk and UEFI settings across launches. Software emulation makes guest CPU
performance dependent on the host.

The launcher needs Python 3, `qemu-system-aarch64`, and a matching pair of AArch64 UEFI code and
variable-store images. Use one writable copy of the variables image per VM. The defaults are 4
guest CPUs and 16 GiB of guest RAM. Allow additional RAM for the physical host.

This persistent, UEFI-installed Linux VM path is distinct from the **headless full-route TCG test** documented below. The tested x86-64 route uses `tests/puredarwin/run-tcg-e2e.py` with the `puredarwin-e2e-guest` and `puredarwin-e2e-disk` artifacts from one successful workflow run. It boots an ARM64 Linux guest directly into the PureDarwin console; it does not use this persistent VM installer or provide a macOS desktop.

The persistent VM example below uses a 16 GiB guest by default. That is separate from the
validated headless cross-host profile, which assigns 6 GiB to its ARM64 guest. No physical host
RAM minimum has been established. Measure available RAM before starting QEMU and leave headroom
for the host OS; hosted-runner measurements and their limits are in the
[validation matrix](portability-validation.md).

## Reproduce the validated headless TCG route

The successful cross-host workflow builds a matching ARM64 Linux kernel and initramfs, FKI/XNU
modules, a free kernel collection, and a PureDarwin system disk. To reproduce its full console
and shutdown route on an x86-64 host:

1. Use one successful run of the **PureDarwin ARM64 image** workflow, such as [run #158](https://github.com/Personed/xnulinux/actions/runs/45911026412). Download the `puredarwin-e2e-guest` and `puredarwin-e2e-disk` artifacts from that same run. Extract them into these directories; the guest artifact must keep both manifests beside `Image`:

   ```text
   artifacts/
   ├── puredarwin-e2e-guest/
   │   ├── Image
   │   ├── initramfs.cpio.gz
   │   ├── guest-manifest.txt
   │   └── build-manifest.txt
   └── puredarwin-e2e-disk/
       └── pd.img
   ```

   The two artifacts are retained for 30 days. The harness checks that Image, initramfs, and disk match their manifests before it starts QEMU. Always obtain both artifacts from one workflow run.

2. Install Python 3.12 and a QEMU build with AArch64 system emulation (QEMU 8.2 or newer for the tested `cortex-a710` CPU model):

   - Ubuntu/Debian: `sudo apt-get install qemu-system-arm python3`
   - macOS: `brew install qemu python3`
   - Windows: install QEMU with `choco install qemu --yes` or the [official Windows QEMU build](https://www.qemu.org/download/#windows), add its executable directory to `PATH`, and use the Python launcher `py -3`.

3. From a checkout of the same repository revision that produced the artifacts, run the headless boot with the tested 6 GiB profile. The script defaults to QEMU's `cortex-a710` TCG CPU model.

   On Linux or macOS:

   ```sh
   python3 tests/puredarwin/run-tcg-e2e.py \
     artifacts/puredarwin-e2e-guest/Image \
     artifacts/puredarwin-e2e-guest/initramfs.cpio.gz \
     artifacts/puredarwin-e2e-disk/pd.img \
     puredarwin-tcg-serial.log \
     --qemu-memory 6G \
     --linux-memory 2G \
     --fki-carve-base 0xc0000000 \
     --fki-carve-mb 3072 \
     --xnu-ram-mb 2816 \
     --max-rss-mib 6144
   ```

   On Windows PowerShell, with the same artifact directory layout:

   ```powershell
   $env:ARTIFACTS = (Resolve-Path .\artifacts).Path
   py -3 tests\puredarwin\run-tcg-e2e.py `
     "$env:ARTIFACTS\puredarwin-e2e-guest\Image" `
     "$env:ARTIFACTS\puredarwin-e2e-guest\initramfs.cpio.gz" `
     "$env:ARTIFACTS\puredarwin-e2e-disk\pd.img" `
     puredarwin-tcg-serial.log `
     --qemu-memory 6G `
     --linux-memory 2G `
     --fki-carve-base 0xc0000000 `
     --fki-carve-mb 3072 `
     --xnu-ram-mb 2816 `
     --max-rss-mib 6144
   ```

The command uses QEMU TCG, so VT-x/AMD-V is not required. It requires 6 GiB of guest address
space plus host memory for QEMU and the host OS. The tested hosted runners had 10.5–15.2 GB
available before QEMU; peak QEMU RSS was 3.47 GiB on Intel macOS and 3.67 GiB on Ubuntu, and
was not measured on Windows. Those are hosted-runner observations, not a physical RAM minimum.
No physical-PC compatibility or minimum RAM has been established. This headless route reaches
the PureDarwin shell and verifies guest halt, module unload, Linux power-off, and QEMU exit; it
does not test a desktop, networking, or graphics.

## Persistent VM launcher host checks

CI starts the ARM64 guest through UEFI on Ubuntu 24.04 x86-64 and ARM64, Windows Server 2025
x86-64, macOS 15 on Intel, and macOS 15 on Apple Silicon. Ubuntu x86-64 also starts the graphical
guest under Xvfb with both GTK and SDL and checks that the QEMU window appears. This confirms the
launcher reaches the UEFI shell on these hosted runner profiles; it does not validate installing
Linux or booting PureDarwin on each host. Windows ARM64 is not in the current matrix.

## Install the Linux guest

Use an ARM64 Ubuntu Server ISO and a disk big enough for the Linux build environment and the
PureDarwin or macOS disk. On Ubuntu or Debian, install `qemu-system-arm`, `qemu-efi-aarch64` and
`qemu-utils`. For a graphical QEMU window, install `qemu-system-gui` too if your distribution
packages it separately. On macOS, install QEMU with Homebrew (`brew install qemu`). On Windows,
install a Windows QEMU distribution that includes AArch64 system emulation. By default, Windows
`--graphical` mode selects SDL, so that QEMU build needs SDL support; otherwise omit `--graphical`
to run the guest headless. If the build exposes another display backend, select it explicitly with
`--display-backend`. The native Windows build is available from the
[QEMU download page](https://www.qemu.org/download/#windows). Add QEMU’s executable directory to
`PATH` before running the launcher.

On macOS and Windows, `qemu-system-aarch64 -L help` lists directories where QEMU looks for data.
Use the AArch64 code image and ARM variables template shipped with the same QEMU distribution,
typically `edk2-aarch64-code.fd` and `edk2-arm-vars.fd`, in place of the AAVMF paths below. From
PowerShell, use `Copy-Item` to copy the variables file and `py -3` to run the Python launcher.

The following example uses the AAVMF files from Ubuntu or Debian:

```sh
qemu-img create -f qcow2 linux.qcow2 128G
cp /usr/share/AAVMF/AAVMF_VARS.fd linux-vars.fd
python3 scripts/qemu-vm.py linux.qcow2 \
  --firmware-code /usr/share/AAVMF/AAVMF_CODE.fd --firmware-vars linux-vars.fd \
  --installer /path/to/ubuntu-server-arm64.iso --graphical --ssh-port 2222
```

In the installer, select the virtual disk and enable OpenSSH Server if you want to connect from
the physical host. Power off when installation is complete. Start the VM again with the same
disk and firmware files, omitting `--installer`. Connect using `ssh -p 2222 USER@127.0.0.1` once
the guest's SSH service is available. The forwarding rule listens only on the host loopback
address. Omit `--ssh-port` when you do not need it.

`--graphical` checks the installed QEMU display backends, then prefers Cocoa on macOS, GTK on Linux,
or SDL on Windows. Linux and macOS can fall back to SDL when their preferred backend is not present.
With `--graphical`, use `--display-backend sdl` to force SDL when testing a backend or when the host
needs it. The launcher reports an unavailable requested backend before starting the VM and rejects
`--display-backend` without `--graphical`. Run from a desktop session. For a terminal-only
guest, omit `--graphical` and configure Linux or its installer to use `console=ttyAMA0`. The QEMU
terminal can switch between guest serial and the QEMU monitor with `Ctrl+A`, then `C`.

## Prepare xnulinux in the guest

Inside the Linux VM, check for `aarch64` from `uname -m`, `4096` from `getconf PAGE_SIZE`, and
`paca` in `/proc/cpuinfo` before building the modules. Use the guest's running Linux version for
the kernel headers. Prepare the kernel collection and disk using the [PureDarwin guide](free-kc.md)
or the [macOS guide](hosted-service.md).

For the experimental 16 GiB VM profile, the QEMU `virt` RAM starts at `0x40000000`. The standard
`scripts/vm-install-fki-service.sh` configuration limits Linux to 7 GiB and reserves a region at
`0x200000000` for XNU. Build `fki.ko` first, then install that service and reboot inside the VM.
Use `GPU=off` for the hosted system. The optional QEMU graphical display is for Linux's console.

The persistent UEFI VM launcher has been exercised through the firmware shell and a Linux boot
on the host profiles listed above; a 2 MiB test module allocation also passed in its 16 GiB guest.
Those checks do not validate a complete PureDarwin boot through this persistent-install route. Use
the headless artifact-based TCG route above for the validated x86-64-to-PureDarwin-shell flow.
