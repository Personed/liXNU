<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Portability validation

Record the **host, runtime, and furthest verified stage** for each result. Booting an
ARM64 Linux VM does not mean that XNU ran. Building or probing a module does not mean
Darwin userspace ran. A PureDarwin shell does not establish macOS compatibility.

The active portability route is x86-64 Linux → QEMU TCG → ARM64 Linux → FKI/XNU → PureDarwin console shell. Native x86-64/Tahoe hosting and the QEMU/KVM macOS guest launcher are separate experiments. The full TCG route has passed on virtualized Ubuntu x86-64 runners; that does not establish physical-PC, Windows-host, or Intel-macOS-host support.

| Stage | What counts as evidence |
| --- | --- |
| Firmware | The intended architecture reaches the firmware console (e.g. UEFI shell for the QEMU profile). |
| Linux host or guest | ARM64 Linux reaches the intended userspace; record whether this is a distro boot or a minimal test initramfs, plus the kernel, page size, and PAC availability. |
| FKI configuration | FKI loads with the requested `carve_base` and `carve_mb`; reading those parameters confirms configuration only, not that the reserved pages can be allocated. |
| FKI carve-out allocation | A probe requests a known size and verifies the returned physical interval `[pa, pa + size)` is fully inside the configured carve-out `[carve_base, carve_base + (carve_mb << 20))`. It then frees and repeats the allocation without reloading FKI, verifying the interval is reused. Record the request, returned interval, and configured bounds. This validates allocation and reuse for the tested profile only. |
| XNU module probe | `xnu.ko` resolves its FKI dependencies and reaches the expected missing-image error. This checks the module and loader path without a real collection; it does not show that XNU allocated from the carve-out. |
| XNU uses the carve-out | With a compatible collection loaded, capture `xnu: contiguous DRAM at phys` and `xnu: contiguous DRAM size` from the host log. Mark this stage only when that complete returned interval lies inside the configured carve-out. The separate FKI probe above does not establish this stage. |
| PureDarwin userspace | The real, paired PureDarwin disk and free kernel collection boot; execute a command on the Darwin console and observe the result. Record a clean halt separately. |
| TCG guest shutdown | The ARM64 guest issues `halt`; XNU and FKI unload; Linux powers off; and QEMU exits with status 0. A host-issued stop is cleanup, not a guest shutdown pass. Run #140's initramfs requires successful `rmmod xnu` and `rmmod fki` before its success marker, although only XNU has a separate unload marker in the serial log. |
| ARM64 hosted macOS desktop | A user-provided compatible system disk and matching XNU collection reach an interactive desktop through the ARM64 FKI/XNU path; record input, rendering backend, and networking separately. |
| Native x86_64 hosted macOS desktop | Architecture-matched x86_64 `fki.ko` and `xnu.ko` host Tahoe 26 directly on Linux and reach the desktop on a physical Intel or AMD PC. Record host-integrity checks and guest-initiated clean shutdown; report Intel and AMD separately. |
| Supplemental QEMU/KVM guest desktop | A user-prepared Tahoe 26 x86_64 disk reaches the desktop in a QEMU/KVM guest. This optional launcher does not use the project's x86_64 hosted-XNU modules and is not evidence of the native port. |

Report the profile alongside the stage:

- **Native ARM64 Linux:** stock Linux with 4 KiB pages, PAC, and the required
  exception level. A result on one board/CPU or kernel does not validate all ARM64 devices.
- **ARM64 Linux guest hosted on another OS:** record the outer machine and OS separately
  from the Linux guest. The ARM64 QEMU path in [#3] uses TCG, including on Intel/AMD hosts;
  distinguish it from the Apple Silicon krunkit path. The module smoke test in [#1]
  is a separate check. Neither substitutes for the PureDarwin test below.
- **x86-64 PC → QEMU TCG → ARM64 Linux:** keep the PC's CPU/OS/QEMU details separate from
  the ARM64 guest. The reference FKI carve-out profile is tracked in [#23]: x86-64 Linux,
  QEMU 10.0.13 TCG, an ARM64 `virt` guest with 1 GiB of RAM and `mem=768M`, and FKI
  configured with `carve_base=0x70000000 carve_mb=256`. Its probe allocates and frees
  16 MiB twice, requiring physical address `0x70000000` on both passes. The guest uses
  a minimal test initramfs, not a full installed Linux distribution. This validates FKI's
  mapping and reuse in that profile; it is not a physical-PC compatibility claim, an XNU
  carve-out use test, or a PureDarwin boot.
- **x86-64 PC → QEMU TCG → ARM64 Linux → PureDarwin console:** this is the active full
  route. PR #72 run [#140](https://github.com/Personed/xnulinux/actions/runs/45891917494)
  passed with paired PureDarwin disk and guest artifacts from that run, a Darwin console
  command, guest `halt`, XNU/FKI unload, Linux power-off, and QEMU exit 0. The reduced
  profile assigns 6 GiB QEMU RAM, Linux `mem=2G`, a 3,072 MiB FKI carve-out at
  `0xc0000000`, and 2,816 MiB to XNU. It ran on a virtualized Ubuntu x86-64 runner; the
  recorded OS-visible memory and QEMU RSS are evidence for that runner only, not a physical
  host-memory minimum or PC compatibility claim. Windows and Intel macOS full-route runs
  remain pending.

  The historical #36 reference profile assigns 16 GiB to the guest, Linux `mem=7G`, an
  8,156 MiB FKI carve-out, and 7,900 MiB XNU RAM. Its 20 GiB total / 4 GiB available host
  screen describes that profile only. Earlier #36/#50/#52/#64 and local #62 runs have
  different commits, manifests, and runner conditions; see their history below. Do not
  combine them into one current acceptance result. The separate #75 follow-up adds fixed
  package-digest and built-in VirtIO checks, but its integration and boot on the current
  tree remain pending. None of these profiles establishes native x86-64/Tahoe hosting,
  QEMU/KVM macOS, a macOS desktop, or support on every PC.

  Historical CI context: a run on old #36 head `5312b29` emitted console and shutdown markers, but a later byte comparison found a file-backed `__TEXT_EXEC`/`__KLD` overlap; it is not an acceptance pass. #36 run #74 failed before boot on that overlap. PR #50 run #89, PR #52 run #91, and PR #64 run #95 passed their respective TCG workflows on virtualized runners, with different branch states and guest profiles. PR #64's #95 result was on the #52 branch and was not a main-integrated cross-host matrix. PR #62 is separate local evidence using a locally relocated collection, not the synchronized CI artifact manifest. PR #46 run #79 failed in `kclink` before guest preparation; an earlier Windows run #71 faulted with an older kernel layout. Neither establishes Windows or Intel-macOS compatibility. These results remain historical evidence and are not combined into the newer reduced-memory profile.

PR #52 head `8d41fa9` passed its current CI workflow in run [#138](https://github.com/Personed/xnulinux/actions/runs/45891610133). All four jobs succeeded: pinned PureDarwin image build, real HFS+ disk and source XNU collection build, ARM64 Linux/FKI/XNU guest preparation, and x86-64 QEMU TCG boot. The guest and disk artifacts were downloaded from that same workflow run. The boot ran on an Ubuntu 24.04.5 Azure-hosted VM under Microsoft virtualization with 4 vCPUs, Intel Xeon Platinum 8573C, QEMU 8.2.2, `MemTotal: 16366484 kB`, and `MemAvailable: 15398364 kB` before artifact downloads (`14878964 kB` at boot). It used 6 GiB QEMU RAM, Linux `mem=2G`, a 3,072 MiB FKI carve-out at `0xc0000000`, and 2,816 MiB XNU RAM. The log recorded the Darwin command, guest-requested halt, XNU unload, and Linux power-off; the harness required QEMU exit 0. Peak QEMU RSS was 3,835,984 KiB (3.66 GiB), below the 6,144 MiB cap. This is a successful virtualized CI profile, not physical Intel/AMD PC acceptance or a host/guest memory minimum. It predates #72's TTBR1 guard and #75's package/hash follow-up, and it does not validate the native x86_64/Tahoe runtime.
PR #72 then validated the ARM64 TTBR1 geometry guard in run [#140](https://github.com/Personed/xnulinux/actions/runs/45891917494): image, real HFS+ disk and source XNU collection, guest preparation, and the reduced TCG boot all succeeded. The boot used the same 6 GiB / `mem=2G` / 3,072 MiB FKI / 2,816 MiB XNU profile on an Ubuntu 24.04.5 AWS/KVM VM (4 vCPUs, Intel Xeon 6975P-C, QEMU 8.2.2). It reported `MemTotal: 16078392 kB`, `MemAvailable: 15173240 kB` before artifacts (`14787768 kB` at boot), and 3,826,452 KiB (3.65 GiB) peak QEMU RSS. The console command, guest `halt`, XNU unload, Linux power-off, and QEMU exit 0 passed. Run #140 tested commit `e61f578`; its Git tree `3421056` is identical to #52 branch commit `3b47444`, so this run validates the guard-enabled code in that #52 tree for the normal 4 KiB/48-bit guest geometry. The run did not inject an incompatible TTBR1 geometry and does not establish rejection behavior for invalid geometries. It remains virtualized branch evidence, not bare-metal or native x86_64 acceptance.

## Reproducing the active QEMU TCG boot

This procedure uses the exact paired artifacts from PR #72 run [#140](https://github.com/Personed/xnulinux/actions/runs/45891917494), which tested commit `e61f578799257685b2c1741afd7fba43c947cf69`. Its Git tree matches PR #52 commit `3b47444`. Run #140 was a virtualized Ubuntu x86-64 runner, not a physical PC. Its artifacts are scheduled to expire on 2026-11-29; if they are no longer available, choose a later successful run and download both artifacts from that same run.

1. On an x86-64 Linux host, install Python 3, GitHub CLI, Git, and QEMU's ARM system emulator. Log in to GitHub CLI if needed so it can read Actions artifacts.
   ```sh
   sudo apt-get update
   sudo apt-get install -y python3 git gh qemu-system-arm
   gh auth login
   ```
2. Check out the exact tested script revision:
   ```sh
   git clone https://github.com/Personed/xnulinux.git
   cd xnulinux
   git fetch origin refs/pull/72/head
   git checkout e61f578799257685b2c1741afd7fba43c947cf69
   ```
3. Download the guest and disk artifacts from the same run. The extracted files are `Image`, `initramfs.cpio.gz`, and `pd.img`:
   ```sh
   mkdir -p work/tcg-run-140
   gh run download 45891917494 \
     --name puredarwin-e2e-guest \
     --name puredarwin-e2e-disk \
     --dir work/tcg-run-140
   ```
4. Keep the artifact pristine and boot a writable copy of the disk:
   ```sh
   cp work/tcg-run-140/puredarwin-e2e-disk/pd.img work/tcg-run-140/pd-writable.img
   python3 tests/puredarwin/run-tcg-e2e.py \
     --timeout-seconds 5400 \
     --qemu-memory 6G \
     --linux-memory 2G \
     --fki-carve-base 0xc0000000 \
     --fki-carve-mb 3072 \
     --xnu-ram-mb 2816 \
     --max-rss-mib 6144 \
     work/tcg-run-140/puredarwin-e2e-guest/Image \
     work/tcg-run-140/puredarwin-e2e-guest/initramfs.cpio.gz \
     work/tcg-run-140/pd-writable.img \
     work/tcg-run-140/tcg-serial.log
   ```
   A successful exit requires the Darwin console command and `XNULINUX_USERLAND_OK`, guest-requested `halt`, successful XNU/FKI unloads, Linux `reboot: Power down`, and QEMU exit status 0. The initramfs checks both module unloads before emitting its success marker; only XNU has a separate unload marker in the serial log. The `--max-rss-mib` argument caps measured QEMU-process RSS; it is not a host RAM minimum. QEMU TCG is software emulation and needs no VT-x/AMD-V.

The local HFS+ disk helper supports Linux and macOS; its Linux prerequisites and PureDarwin root export are in [the free collection guide](free-kc.md#using-it). The full boot procedure above is currently documented for Linux x86-64. Windows and Intel macOS host full boots still need validation.

- **Native x86-64 hosted XNU on Intel/AMD Linux:** this is the project target for Tahoe 26.
  The x86_64 FKI host-backend build merged in [#48](https://github.com/Personed/xnulinux/pull/48) and passed compilation while preserving ARM64; it advertises no XNU execution capability.
  The fail-closed x86_64 XNU stub merged in [#47](https://github.com/Personed/xnulinux/pull/47) still provides no execution support.
  The architecture-specific XNU entry/world switch and physical desktop acceptance are still pending.
  For each host, record the CPU vendor/model/features, Linux kernel and configuration,
  memory, page size, source and system build, desktop result, host-integrity checks before
  and after, and guest-issued shutdown. Keep physical Intel and AMD evidence separate.
- **Supplemental x86-64 macOS guest on Intel/AMD Linux:** the QEMU/KVM launcher in [#16]
  is an optional guest path, not a port of the FKI/XNU modules. Record QEMU, accelerator,
  Tahoe build, OpenCore configuration and guest result as launcher evidence only. An
  x86_64 Linux TCG smoke is not evidence of macOS boot. See
  [`tests/x86_64/README.md`](../tests/x86_64/README.md) for launcher prerequisites and limits.

## First end-to-end check for a new ARM64 FKI host profile

1. Use a disposable machine or Linux VM and a writable copy of the test disk.
   Do not start the service before this manual run. If `/etc/xnulinux.conf`
   exists, check it first: it is sourced by the boot script and can replace
   the values supplied on the command line.
   These scripts load Linux kernel modules and make host-wide changes. Do not
   run this on a production host. Start with one hosted CPU and GPU disabled
   so basic userspace boot does not depend on SMP or the graphics bridge.
2. Follow [host preparation](hosted-service.md#3-operator-preparing-a-host) for the
   Linux kernel, headers and FKI carve-out. Before rebooting into the reserved memory
   configuration, build FKI in the same checkout that the FKI service will use:
   ```sh
   make -C "/lib/modules/$(uname -r)/build" M="$PWD/fki-ko" modules
   ```
   This assumes you will reboot into the same kernel; if selecting a different
   kernel, build against its headers instead. After reboot, verify the service
   succeeded and check the active carve-out against the values used to configure
   it. Stop here if FKI did not load or the values do not match.
   ```sh
   systemctl is-active xnulinux-fki.service
   cat /sys/module/fki/parameters/carve_base /sys/module/fki/parameters/carve_mb
   ```
   These values confirm the FKI configuration only. Continue to the allocation probe
   before marking the carve-out usable.
   Inside that ARM64 Linux instance, collect
   `uname -rm`, `getconf PAGE_SIZE`, `lscpu` (including PAC features) and the FKI
   carve-out values. Record the outer host, hypervisor and accelerator separately
   if running a VM. Do not copy memory offsets from another board or VM.
3. Follow only the preparation steps (1–3) of [building the free collection and disk](free-kc.md#using-it)
   for a real PureDarwin image and an `xnu/kc-free.bin` built from compatible revisions.
   Skip its boot command; start capturing the console before booting in step 4 below.
   Record the repository revision(s), the PureDarwin revision and SHA-256 digests
   of the collection and pristine disk. Use the disk's matching UUID file.
   [#4], [#5] and [#7] track work on build and image preparation; a fixture disk
   used to test extraction or partitioning is not a bootable test image.
4. In one Linux terminal, capture output that originates from the hosted console:
   ```sh
   sudo journalctl -kf -n 0 -o cat | grep --line-buffered 'fki: darwin console:'
   ```
   In a second terminal at the checkout, point the boot script to the absolute
   paths for your disk and UUID file. Set `RAM_MB` to a tested value that fits
   your carve-out; 7900 MiB is the existing example for an 8 GiB carve-out.
   ```sh
   export RAM_MB=7900  # example only; use the value for this host
   MAC_CONF=/absolute/path/pd.conf DISK=/absolute/path/pd.img \
     IMAGE=xnu/kc-free.bin TRUSTCACHE= GPU=off CPUS=1 CONSOLE_IN=/var/tmp/console-in \
     bash scripts/xnulinux-boot.sh --wait
   ```
5. After the Darwin shell prompt appears, send these lines from a third Linux terminal:
   ```sh
   printf '%s\n' 'uname -s' 'printf "XNULINUX_%s\\n" USERLAND_OK' \
     >> /var/tmp/console-in
   ```
   Require separate Darwin console output lines `Darwin` and `XNULINUX_USERLAND_OK`;
   an echo of the input is not a pass. Send `halt` from
   the same console input and verify that `--wait` returns 0 with the explicit
   hosted shutdown message and that `xnu` unloads. A host-issued stop is cleanup,
   not a guest halt pass. If the shell never appears, save the boot log and record
   the last stage reached rather than marking the whole profile as unsupported.
6. Repeat against the physical or virtual configurations you want to claim.
   Use separate results for smaller memory, more CPUs, GPU and networking. Keep
   a known-good one-CPU/CPU-rendering or console run as a baseline.

## Report template

```text
Repo commit(s) / PR(s):
Profile (direct ARM64 hosted XNU, active x86-64 QEMU TCG/PureDarwin route, separate native x86_64 experiment, or supplemental QEMU/KVM guest):
Outer machine + CPU / OS + version:
Runtime + version / accelerator:
TCG Linux stage and result (if applicable; do not label as a macOS result):

TCG full-route host capacity (only when applicable; separate from guest RAM):
Outer physical host RAM total (if known; otherwise state unavailable):
RAM visible to the OS that runs QEMU (`MemTotal`) / available before artifact preparation / sample time:
20 GiB total / 4 GiB available reference-screen result using runner-visible RAM / capacity or overcommit caveat:
Virtualization or nested-hypervisor details / cgroup or VM memory limit:
QEMU version / host CPU model and core count:
Pinned guest kernel package/version / APT snapshot / each package `.deb` SHA-256:
Guest kernel release / `.config` SHA-256 (configuration identity only):
Guest manifest SHA-256 (artifact-set identity):
Image, initramfs, modules and collection SHA-256 values from the manifest:
ARM64 guest assigned RAM / mem= / FKI carve-out / XNU RAM:
Darwin console command and output / guest-issued halt:
XNU unload / FKI unload / Linux guest power-off / QEMU exit status:

ARM64 FKI/PureDarwin profile (only when applicable):
Linux distro / kernel / page size / PAC / userspace type:
Memory / FKI carve-out / hosted CPUs:
FKI configuration / carve-out allocation and reuse result:
XNU collection revision / digest and matching PureDarwin disk digest:
XNU stage and contiguous DRAM interval / configured carve-out comparison:
PureDarwin console command / guest-issued halt result:

Native x86_64 hosted-XNU profile (only when applicable):
CPU vendor/model/features / Linux kernel release and .config or SHA-256 / page size / RAM:
Tahoe build / matching XNU source and collection revision:
Native hosted desktop / disk, input, network, rendering / result:
Last boot stage reached if the desktop does not start / failure evidence:
Linux host-integrity checks before, during and after shutdown / result:
Guest-issued shutdown / module unload / result:

Supplemental QEMU/KVM guest profile (only when applicable):
Physical CPU vendor/model/features / Linux kernel / RAM:
QEMU version / KVM active (yes/no):
Tahoe build / prepared disk SHA-256 / OpenCore version and configuration:
Tahoe desktop reached / display / input / networking result:
Host-integrity checks before and after / result:
Source disk and OVMF template unchanged (yes/no):
Guest-issued shutdown / QEMU exit result:

ARM64 hosted macOS desktop (only when applicable):
macOS product version / build:
Linux host distribution / kernel / page size / PAC / userspace type:
Compatible disk and XNU collection revision / digests:
Interactive desktop reached / display backend:
Keyboard and mouse input / networking result:

ARM64 hosted rendering / input / networking (tested, failed, or not tested):
CI run or sanitized logs:
```

Link the report from the relevant PR. Attach logs rather than disk images or Apple
artifacts. Remove machine identifiers, credentials and local paths before sharing.

[#1]: https://github.com/Personed/xnulinux/pull/1
[#3]: https://github.com/Personed/xnulinux/pull/3
[#4]: https://github.com/Personed/xnulinux/pull/4
[#5]: https://github.com/Personed/xnulinux/pull/5
[#7]: https://github.com/Personed/xnulinux/pull/7
[#23]: https://github.com/Personed/xnulinux/pull/23
[#16]: https://github.com/Personed/xnulinux/pull/16
