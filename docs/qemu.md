<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Running the ARM64 Linux host in QEMU

`scripts/qemu-vm.py` starts the ARM64 Linux VM that hosts xnulinux. It uses QEMU TCG to emulate
an ARM64 CPU with pointer authentication, including on Intel and AMD PCs. The VM runs Linux at EL1
and keeps its system disk and UEFI settings across launches. Software emulation makes guest CPU
performance dependent on the host.

The launcher needs Python 3, `qemu-system-aarch64`, and a matching pair of AArch64 UEFI code and
variable-store images. Use one writable copy of the variables image per VM. The defaults are 4
guest CPUs and 16 GiB of guest RAM. Allow additional RAM for the physical host.

## Install the Linux guest

Use an ARM64 Ubuntu Server ISO and a disk big enough for the Linux build environment and the
PureDarwin or macOS disk. On Ubuntu or Debian, install `qemu-system-arm`, `qemu-efi-aarch64` and
`qemu-utils`. For a graphical QEMU window, install `qemu-system-gui` too if your distribution
packages it separately. On macOS, install QEMU with Homebrew (`brew install qemu`). On Windows,
install a Windows QEMU distribution that includes AArch64 system emulation. The native Windows
build is available from the [QEMU download page](https://www.qemu.org/download/#windows). Add
QEMU’s executable directory to `PATH` before running the launcher.

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

`--graphical` opens a local QEMU window through Cocoa on macOS, GTK on Linux, or SDL on Windows.
Use a QEMU package with that display backend and run from a desktop session. For a terminal-only
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

The QEMU launcher has been exercised from x86-64 Linux through the ARM64 Debian installer.
A test module allocated 2 MiB at the documented address in a 16 GiB guest booted directly
into Linux. A complete hosted PureDarwin or macOS boot using the UEFI profile remains unverified.
