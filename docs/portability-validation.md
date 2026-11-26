<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Portability validation

Record the **host, runtime, and furthest verified stage** for each result. Booting an
ARM64 Linux VM does not mean that XNU ran. Building or probing a module does not mean
Darwin userspace ran. A PureDarwin shell does not establish macOS compatibility.

The active PC compatibility goal is x86-64 PC → QEMU TCG → ARM64 Linux → FKI/XNU →
PureDarwin console, alongside the direct hosted-XNU path on compatible ARM64 Linux hosts.
The full TCG route is tracked in [PR #36](https://github.com/Personed/xnulinux/pull/36)
and remains under validation. Native x86_64/Tahoe hosting and QEMU/KVM macOS guests are
separate experiments; neither substitutes for this route or establishes its acceptance.

| Stage | What counts as evidence |
| --- | --- |
| Firmware | The intended architecture reaches the firmware console (e.g. UEFI shell for the QEMU profile). |
| Linux host or guest | ARM64 Linux reaches the intended userspace; record whether this is a distro boot or a minimal test initramfs, plus the kernel, page size, and PAC availability. |
| FKI configuration | FKI loads with the requested `carve_base` and `carve_mb`; reading those parameters confirms configuration only, not that the reserved pages can be allocated. |
| FKI carve-out allocation | A probe requests a known size and verifies the returned physical interval `[pa, pa + size)` is fully inside the configured carve-out `[carve_base, carve_base + (carve_mb << 20))`. It then frees and repeats the allocation without reloading FKI, verifying the interval is reused. Record the request, returned interval, and configured bounds. This validates allocation and reuse for the tested profile only. |
| XNU module probe | `xnu.ko` resolves its FKI dependencies and reaches the expected missing-image error. This checks the module and loader path without a real collection; it does not show that XNU allocated from the carve-out. |
| XNU uses the carve-out | With a compatible collection loaded, capture `xnu: contiguous DRAM at phys` and `xnu: contiguous DRAM size` from the host log. Mark this stage only when that complete returned interval lies inside the configured carve-out. The separate FKI probe above does not establish this stage. |
| PureDarwin userspace | The real, paired PureDarwin disk and free kernel collection boot; execute a command on the Darwin console and observe the result. Record a clean halt separately. |
| TCG guest shutdown | The guest issues `halt`; in run #140, `rmmod xnu` and `rmmod fki` must both succeed before the existing XNU-unload marker is printed; Linux powers off and QEMU exits 0. The serial log has no separate FKI-unload marker. A host-issued stop is cleanup, not a guest shutdown pass. |
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
- **x86-64 PC → QEMU TCG → ARM64 Linux → PureDarwin console:** this is the active PC compatibility route tracked in [#36](https://github.com/Personed/xnulinux/pull/36). Acceptance requires a paired ARM64 Linux guest and PureDarwin disk to reach a Darwin command, accept guest-issued `halt`, unload XNU and FKI, power off Linux, and exit QEMU with status 0. TCG does not require VT-x or AMD-V.
  - **Reduced profile: PR #72 workflow run #140.** [Run #140](https://github.com/Personed/xnulinux/actions/runs/45891917494) passed all four jobs at tested commit `e61f578799257685b2c1741afd7fba43c947cf69`; its Git tree is identical to PR #52 commit `3b474448a1a9aab92c42ed377fd9839414485e03`. Runner: Ubuntu 24.04.5 on AWS/KVM virtualization, 4 vCPUs, Intel Xeon 6975P-C virtual CPU, QEMU 8.2.2. Profile: 6 GiB QEMU guest, Linux `mem=2G`, FKI `carve_base=0xc0000000 carve_mb=3072`, XNU RAM 2,816 MiB. It reached the PureDarwin command marker, guest halt, Linux power-off, and QEMU exit 0. Runner-visible `MemTotal` was 16,078,392 kB; `MemAvailable` was 15,173,240 kB before artifact preparation and 14,787,768 kB at boot; QEMU peak RSS was 3,826,452 KiB. These report the runner OS, not outer physical RAM or a physical-host minimum.
  - **Kernel input distinction:** the #36 workflow resolves its ARM64 kernel from APT availability at run time. Run #140 instead used pinned package `7.0.0-44.44~24.04.1` and APT snapshot `20261126T000000Z`; its manifest hashes apply to those exact inputs and do not validate #36's dynamic package selection.
  - PR #75 added fixed expected package digests and VirtIO checks. Its head `5fa6947` passed run #143: the 6 GiB route reached the Darwin userland marker, guest halt, successful `rmmod xnu`/`rmmod fki` control flow, Linux power-off, and QEMU exit 0. The KVM runner exposed an Intel Xeon 6975P-C; QEMU 8.2.2 peak RSS was 3,841,412 KiB (3.66 GiB). PR #75 was merged into the #52 branch as `24e4a9c`; this integrates the checks into that stack, not into main.
  - **Independent reduced run #138.** PR #52 workflow run #138 (workflow ID `45891610133`) also completed the 6 GiB route on Ubuntu 24.04.5 under a Microsoft/Azure virtual runner exposing an Intel Xeon Platinum 8573C and 4 vCPUs, with QEMU 8.2.2. Runner-visible `MemTotal` was 16,366,484 kB; `MemAvailable` was 15,398,364 kB before artifact preparation and 14,878,964 kB at boot; QEMU peak RSS was 3,835,984 KiB. This is a second virtualized Linux runner result, not a physical-host RAM measurement.
  - The run #140 initramfs `tests/puredarwin/init` executes `rmmod xnu` and then `rmmod fki`, aborting if either fails. `XNULINUX_E2E_XNU_UNLOADED` is printed only after both succeed; the serial log has no separate FKI-unload marker. The run validates the TTBR1 guard with its valid 4 KiB/48-bit profile, not rejection of invalid geometry.
  - The #140 guest and disk artifact ZIPs are `puredarwin-e2e-guest` (API digest `sha256:d7f7d21c025249655ab206ab79d208d3354866a1d21bfbabc25eb37a9194b196`) and `puredarwin-e2e-disk` (API digest `sha256:936add32c29fab498bba74181ede349431e7c84afb4be859cfd4eefa88183dfa`). These are ZIP digests; per-file hashes live in `guest-manifest.txt` and `build-manifest.txt`. The run #140 artifacts expire on 2026-11-29. A maintainer with Actions write permission can rerun run #140 (`gh run rerun 45891917494`) for up to 30 days after the original run; it recreates both artifacts from ref `codex/fki-arm64-va-guard`, head `e61f578799257685b2c1741afd7fba43c947cf69`. Confirm the head and attempt with `gh run view 45891917494 --json headBranch,headSha,attempt,status,conclusion`, then download both named artifacts from that attempt.
Readers without upstream write permission, including after the rerun window, can create the pair in their own fork. The workflow is in the PR #52 base used for #72; reproduce that base/head pair so the pull-request workflow builds the guest and disk together:

~~~sh
git remote add upstream https://github.com/Personed/xnulinux.git
git fetch upstream refs/pull/72/head:refs/remotes/upstream/pr72
git branch codex/pin-arm64-guest-kernel 8d41fa9299a63190235bbc9547b688ad9733cdb4
git branch codex/fki-arm64-va-guard refs/remotes/upstream/pr72
git push origin codex/pin-arm64-guest-kernel codex/fki-arm64-va-guard
~~~

Open a PR in the fork from `codex/fki-arm64-va-guard` to `codex/pin-arm64-guest-kernel`, with base SHA `8d41fa9299a63190235bbc9547b688ad9733cdb4` and head SHA `e61f578799257685b2c1741afd7fba43c947cf69`. Enable Actions and approve the workflow if GitHub prompts. The base branch carries the full artifact-producing workflow; do not use the image-only workflow on main.
Find the new run ID in the fork and verify its source ref:

~~~sh
TCG_REPO=your-login/xnulinux
gh run list --repo "$TCG_REPO" --workflow "PureDarwin ARM64 image" --branch codex/fki-arm64-va-guard --limit 5
~~~

Use the new run ID and the fork repo for both `gh run view` and `gh run download`; confirm head SHA `e61f578799257685b2c1741afd7fba43c947cf69`, then download `puredarwin-e2e-guest` and `puredarwin-e2e-disk` from that same run/attempt. Use its own manifests and hashes; do not mix artifacts or assume their digests match run #140.
  - **Historical 16 GiB profile:** PR #36's older reference assigned 16 GiB to the guest and screened for 20 GiB total runner-OS RAM and 4 GiB available before artifact preparation. This is an operational screen for that profile, not a physical-host minimum. Run #72 on old #36 head `5312b29` emitted boot markers but failed later byte inspection because file-backed `__TEXT_EXEC` and `__KLD` overlapped at `0x3854000-0x3858000`. Run #74 on then-current #36 head `97727ac` failed before boot on the same overlap.
  - PR #50 workflow run #89 passed a 16 GiB TCG route on its earlier #36-plus-relocation source line using a nested KVM-backed Intel runner; the `#50` relocation is now in current #36 source at `93e3cf4`, but run #89 predates that head and no current-head rerun is recorded. PR #52 run #91 reached console/halt on a Microsoft-hypervisor Ubuntu runner reporting AMD EPYC 9V45. PR #64 run #95 is a separate 16 GiB run on AWS-hosted Ubuntu with 4 vCPUs; its CPU model is not attributed here. Merged PR #62 records local console/halt and XNU/FKI-unload runs with a locally relocated collection. These results use older commits or different artifacts and are not physical-host validation or substitutes for run #140's paired artifacts.
  - **Integration and host matrix status, checked 2026-11-26 22:41 UTC:** PR #75 run #143 passed the 6 GiB guest preparation and TCG boot and is merged into #52 at `24e4a9c`. PR #36 head `93e3cf4` still has no current-head full-boot result; PR #50 run #89 predates the relocation's integration there. Cross-host PR #89 is synchronized to #83 head `07d08c5` over #52 head `24e4a9c`, with current head `3fbfa6a`. Launcher #476 passed. Modules run #945 passed build, SPDX, Linux 6.12, and Ubuntu, macOS, and Windows QEMU smoke jobs. PureDarwin run #151 is still building the pinned image; its disk, guest-preparation and full TCG boot jobs have not produced a result on this head. The matrix targets Ubuntu, Windows Server 2025, and Intel macOS 15. PR #96 head `192161f` separately proposes experimental 5/4/3/2.5/2 GiB profiles without changing the required 6 GiB gate; image #154, modules #965, and launcher #492 are queued, and no sub-6 GiB CI boot result is recorded yet. Bare-metal Intel/AMD acceptance and a physical-host RAM floor remain unvalidated.
  - The 6 GiB number is guest RAM for a demonstrated profile, not a measured host-RAM floor. Virtualized-runner results do not prove physical-PC compatibility, outer-host memory overhead, or practical performance; record physical hosts separately.

- **Separate native x86-64 hosted-XNU/Tahoe experiment:** this is not the active PC
  compatibility route. The x86_64 FKI host-backend build merged in
  [#48](https://github.com/Personed/xnulinux/pull/48) and passed compilation while preserving ARM64;
  it advertises no XNU execution capability.
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

## Reproducing the active QEMU TCG boot

This manual reproduction is for an x86-64 Linux host using QEMU TCG; it does not require VT-x or AMD-V. It downloads the paired guest and disk from workflow run #140 and uses the exact source revision tested by that run. Do not mix artifacts from different workflow runs. The same run's internal manifests provide file-level hashes; the GitHub artifact API digests below are ZIP checksums and do not replace those hashes.

The 6 GiB value is QEMU guest allocation. The cited run came from an AWS/KVM virtual runner; it does not measure the outer physical machine or establish minimum physical RAM. Windows, Intel macOS, and bare-metal Intel/AMD boots remain unvalidated on the current matrix head.

```sh
sudo apt-get update
sudo apt-get install -y python3 git gh qemu-system-arm
gh auth login
git clone https://github.com/Personed/xnulinux.git
cd xnulinux
git fetch origin refs/pull/72/head
git checkout e61f578799257685b2c1741afd7fba43c947cf69
mkdir -p work/tcg-run-140
TCG_REPO=Personed/xnulinux
TCG_RUN_ID=45891917494
# For a fork run, set TCG_REPO to your fork and TCG_RUN_ID to its new run ID.
gh run view "$TCG_RUN_ID" --repo "$TCG_REPO" --json headBranch,headSha,attempt,status,conclusion
gh run download "$TCG_RUN_ID" --repo "$TCG_REPO" --name puredarwin-e2e-guest --name puredarwin-e2e-disk --dir work/tcg-run-140
cp work/tcg-run-140/puredarwin-e2e-disk/pd.img work/tcg-run-140/pd-writable.img
cat work/tcg-run-140/puredarwin-e2e-guest/guest-manifest.txt
cat work/tcg-run-140/puredarwin-e2e-guest/build-manifest.txt
sha256sum work/tcg-run-140/puredarwin-e2e-guest/Image work/tcg-run-140/puredarwin-e2e-guest/initramfs.cpio.gz work/tcg-run-140/puredarwin-e2e-disk/pd.img
```

The original run #140 manifests record these file hashes: guest `Image` `02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6`, `initramfs.cpio.gz` `431c6a2dcdbb77fc1131a8359149a59ad5f4aa5867eff3cf182175cf8a24e322`, and `pd.img` `84fc7065d5a9bfb87a8db4fe1c3e7c5f485a0c1b23cdcb7beccdd426f03528a1`. Compare the local `sha256sum` output with the original run #140 manifest values before booting. For regenerated artifacts, use that successful attempt's own manifests and hashes instead of assuming the original file hashes or ZIP digests will repeat. The two archive digests are guest `sha256:d7f7d21c025249655ab206ab79d208d3354866a1d21bfbabc25eb37a9194b196` and disk `sha256:936add32c29fab498bba74181ede349431e7c84afb4be859cfd4eefa88183dfa`; these identify the ZIP artifacts, not files inside them. The artifacts expire on 2026-11-29. If they have expired, produce a fresh successful workflow run and download its matching guest/disk pair.

Run the harness with the tested profile:

```sh
python3 tests/puredarwin/run-tcg-e2e.py --timeout-seconds 5400 --qemu-memory 6G --linux-memory 2G --fki-carve-base 0xc0000000 --fki-carve-mb 3072 --xnu-ram-mb 2816 --max-rss-mib 6144 work/tcg-run-140/puredarwin-e2e-guest/Image work/tcg-run-140/puredarwin-e2e-guest/initramfs.cpio.gz work/tcg-run-140/pd-writable.img work/tcg-run-140/tcg-serial.log
```

A successful run must reach the PureDarwin command marker, accept guest-issued `halt`, complete both `rmmod xnu` and `rmmod fki` in the initramfs, power off Linux, and return QEMU status 0. The serial marker names XNU unload but is emitted after both module unloads; there is no separate FKI marker. Preserve `tcg-serial.log` and the command's exit status with the host profile. The harness's QEMU RSS ceiling is a limit for this run, not a performance or physical-memory claim.

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
Profile (direct ARM64 hosted XNU, active x86-64 PC → QEMU TCG → ARM64 Linux → FKI/XNU → PureDarwin, separate native x86_64/Tahoe experiment, or separate QEMU/KVM macOS guest):
Evidence type (physical PC / hosted CI runner / other VM):
Machine that runs QEMU (CPU / OS + version; if applicable):
Runtime + version / accelerator:
TCG Linux stage and result (if applicable; do not label as a macOS result):

TCG full-route host capacity (only when applicable; separate from guest RAM):
Outer physical host RAM total (if known; otherwise state unavailable):
RAM visible to the OS that runs QEMU (`MemTotal`) / available before artifact preparation / sample time:
Historical #36 16 GiB profile only: 20 GiB total / 4 GiB available operational screen using runner-visible RAM (not a physical minimum):
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
