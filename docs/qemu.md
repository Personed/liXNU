<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Running the ARM64 Linux host in QEMU

`scripts/qemu-vm.py` starts the ARM64 Linux VM that hosts xnulinux. It uses QEMU TCG to emulate
an ARM64 CPU with pointer authentication, including on Intel and AMD PCs. The VM runs Linux at EL1
and keeps its system disk and UEFI settings across launches. Software emulation makes guest CPU
performance dependent on the host.

The launcher needs Python 3, `qemu-system-aarch64`, and a matching pair of AArch64 UEFI code and
variable-store images. Use one writable copy of the variables image per VM. The defaults are 4
guest CPUs and 16 GiB of guest RAM. Allow additional RAM for the physical host.

The current complete PureDarwin reference profile assigns 16 GiB to the ARM64 guest. Before
preparing its artifacts, record the total and available RAM visible to the operating system that
will run QEMU. The CI planning screen is at least 20 GiB total and 4 GiB available. On a
virtualized host, these values describe the runner OS and do not establish the outer machine's
physical RAM. This is a planning screen, not a measured minimum; the physical RAM floor is unknown.
See the [validation matrix](portability-validation.md) for the evidence and limits.

## Tested host and guest architectures

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

The QEMU launcher has been exercised from x86-64 Linux through the ARM64 Debian installer and in
CI by starting ARM64 guests through UEFI on the Linux, Windows, Intel macOS, and Apple Silicon
macOS hosts listed above. Ubuntu x86-64 also exercises the graphical path. Its argument
construction is checked on all host systems.
A test module allocated 2 MiB at the documented address in a 16 GiB guest booted directly
into Linux. A complete hosted PureDarwin or macOS boot using the UEFI profile remains unverified.


## Boot the PureDarwin ARM64 guest with TCG

This direct-boot profile uses the x86-64 host to run the ARM64 Linux guest under QEMU TCG,
then loads FKI/XNU and reaches the PureDarwin console. It is separate from the UEFI
installer-based Linux VM above. It does not require VT-x or AMD-V.

For the exact tested input set, check out PR #98 head
[`589c40e`](https://github.com/Personed/xnulinux/commit/589c40ead54a63946fb44e8bdb35eb6add9adffe).
From a clone of this repository, fetch that pull request and check out the tested revision:

```sh
git fetch origin pull/98/head:refs/remotes/origin/pr-98
git checkout --detach 589c40ead54a63946fb44e8bdb35eb6add9adffe
```

Download and extract the paired
[`puredarwin-e2e-guest` artifact](https://github.com/Personed/xnulinux/actions/runs/45911026412/artifacts/15161900498) and
[`puredarwin-e2e-disk` artifact](https://github.com/Personed/xnulinux/actions/runs/45911026412/artifacts/15161666268) from [run #158](https://github.com/Personed/xnulinux/actions/runs/45911026412).
Both artifacts expire on 2026-12-27. You can download them in the browser or with GitHub CLI:

```sh
gh run download 45911026412 --repo Personed/xnulinux --name puredarwin-e2e-guest --dir artifacts/guest
gh run download 45911026412 --repo Personed/xnulinux --name puredarwin-e2e-disk --dir artifacts/disk
```

Keep the artifacts from that run paired. The guest archive contains `Image`,
`initramfs.cpio.gz`, `guest-manifest.txt`, and `build-manifest.txt`;
the disk archive contains `pd.img`. The three boot inputs have these SHA-256 values:

| Input | SHA-256 |
| --- | --- |
| `Image` | `02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6` |
| `initramfs.cpio.gz` | `fd6aec2eac9b6a2d77e6c223869d2cf7cf1379150358267329c83c8f9588d667` |
| `pd.img` | `7c6e4e694e72a748b602142aebb02b06f5bec0d18b0ff9aed85cc4b816df662f` |

Install Python 3 and QEMU system AArch64 emulation 8.2 or later. QEMU 8.2 is the
minimum version with the tested `cortex-a710` CPU model.

- Ubuntu 24.04: `sudo apt-get install qemu-system-arm python3 coreutils util-linux`.
- Debian: install QEMU 8.2 or newer. Debian 12 ships QEMU 7.2, which is too old for
  this profile; use a newer Debian release or a newer QEMU build.
- Intel macOS: `brew install qemu python3`.
- Windows: install Python 3 with the Python launcher, then install QEMU with
  `choco install qemu --yes` or the [official Windows QEMU build](https://www.qemu.org/download/#windows).
  Add QEMU's executable directory to `PATH` (usually `C:\Program Files\qemu`).

Check that the installed QEMU exposes the tested model:

- POSIX: `qemu-system-aarch64 -cpu help | grep -F cortex-a710`
- Windows PowerShell: `qemu-system-aarch64.exe -cpu help | Select-String cortex-a710`.

Run the download commands from the repository root; they extract the files under `artifacts/guest` and `artifacts/disk`. The command below verifies the three hashes from run #158, then copies the disk so the source artifact remains unchanged.

```sh
(
  set -e
  python3 - <<'PY'
import hashlib
from pathlib import Path

expected = {
    "artifacts/guest/Image": "02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6",
    "artifacts/guest/initramfs.cpio.gz": "fd6aec2eac9b6a2d77e6c223869d2cf7cf1379150358267329c83c8f9588d667",
    "artifacts/disk/pd.img": "7c6e4e694e72a748b602142aebb02b06f5bec0d18b0ff9aed85cc4b816df662f",
}
for name, digest in expected.items():
    actual = hashlib.sha256(Path(name).read_bytes()).hexdigest()
    if actual != digest:
        raise SystemExit(f"{name}: expected {digest}, got {actual}")
    print(f"{name}: SHA-256 matches")
PY'
02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6  artifacts/guest/Image
fd6aec2eac9b6a2d77e6c223869d2cf7cf1379150358267329c83c8f9588d667  artifacts/guest/initramfs.cpio.gz
7c6e4e694e72a748b602142aebb02b06f5bec0d18b0ff9aed85cc4b816df662f  artifacts/disk/pd.img
EOF
cp artifacts/disk/pd.img artifacts/disk/pd-tcg-test.img
python3 tests/puredarwin/run-tcg-e2e.py \
  --timeout-seconds 5400 \
  --qemu-memory 6G \
  --qemu-cpu cortex-a710 \
  --linux-memory 2G \
  --fki-carve-base 0xc0000000 \
  --fki-carve-mb 3072 \
  --xnu-ram-mb 2816 \
  --max-rss-mib 6144 \
  artifacts/guest/Image \
  artifacts/guest/initramfs.cpio.gz \
  artifacts/disk/pd-tcg-test.img \
  artifacts/disk/puredarwin-tcg-serial.log
)
```


On Windows PowerShell, install QEMU, add its executable directory to the current session's
`PATH`, and confirm the QEMU binary is available. Install Python 3 with the Python launcher
if `py -3 --version` is not already available. The following commands use the same extracted
artifact directories:

```powershell
$env:Path += ";$env:ProgramFiles\qemu"
qemu-system-aarch64.exe --version
Get-FileHash .\artifacts\guest\Image -Algorithm SHA256
Get-FileHash .\artifacts\guest\initramfs.cpio.gz -Algorithm SHA256
Get-FileHash .\artifacts\disk\pd.img -Algorithm SHA256
Copy-Item .\artifacts\disk\pd.img .\artifacts\disk\pd-tcg-test.img
py -3 tests\puredarwin\run-tcg-e2e.py `
  --timeout-seconds 5400 `
  --qemu qemu-system-aarch64.exe `
  --qemu-memory 6G `
  --qemu-cpu cortex-a710 `
  --linux-memory 2G `
  --fki-carve-base 0xc0000000 `
  --fki-carve-mb 3072 `
  --xnu-ram-mb 2816 `
  --max-rss-mib 6144 `
  .\artifacts\guest\Image `
  .\artifacts\guest\initramfs.cpio.gz `
  .\artifacts\disk\pd-tcg-test.img `
  .\artifacts\disk\puredarwin-tcg-serial.log
```

The Windows runner did not measure QEMU RSS, so the harness reports that the optional
RSS ceiling was not evaluated on that platform.

The manifest-aware validation script and optional exact-hash guard are in
[`docs/portability-validation.md`](portability-validation.md#reproducing-the-active-qemu-tcg-boot).
The command runs TCG with a 6 GiB guest, Linux `mem=2G`, a 3,072 MiB FKI carve-out,
and 2,816 MiB XNU RAM. The harness requires the Darwin console and userland marker,
guest-requested halt, successful XNU/FKI unload, and QEMU exit 0. It enforces the
configured RSS limit on hosts where process RSS is measurable; when the runner cannot
measure it, the log reports that fact instead of treating it as zero. In run #158,
Windows boot passed but process RSS was not measured. This guest allocation is not a
physical host-memory minimum.

Run #158 passed once on each hosted Ubuntu, Windows Server 2025, and Intel macOS profile
with `cortex-a710`. This supports the tested hosted profiles only; representative
physical Intel/AMD PCs and PIE-enabled QEMU `-cpu max` remain separate validation items.
