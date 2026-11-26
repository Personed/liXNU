<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Portability validation

Record the **host, runtime, and furthest verified stage** for each result. Booting an
ARM64 Linux VM does not mean that XNU ran. Building or probing a module does not mean
Darwin userspace ran. A PureDarwin shell does not establish macOS compatibility.

The active portability target is an x86-64 PC using QEMU TCG to boot Linux ARM64, load FKI/XNU, and reach the PureDarwin console. The native x86_64/Tahoe port is separate work. Preserve the direct ARM64 hosted-XNU runtime and macOS 27 target.

| Stage | What counts as evidence |
| --- | --- |
| Firmware | The intended architecture reaches the firmware console (e.g. UEFI shell for the QEMU profile). |
| Linux host or guest | ARM64 Linux reaches the intended userspace; record whether this is a distro boot or a minimal test initramfs, plus the kernel, page size, and PAC availability. |
| FKI configuration | FKI loads with the requested `carve_base` and `carve_mb`; reading those parameters confirms configuration only, not that the reserved pages can be allocated. |
| FKI carve-out allocation | A probe requests a known size and verifies the returned physical interval `[pa, pa + size)` is fully inside the configured carve-out `[carve_base, carve_base + (carve_mb << 20))`. It then frees and repeats the allocation without reloading FKI, verifying the interval is reused. Record the request, returned interval, and configured bounds. This validates allocation and reuse for the tested profile only. |
| XNU module probe | `xnu.ko` resolves its FKI dependencies and reaches the expected missing-image error. This checks the module and loader path without a real collection; it does not show that XNU allocated from the carve-out. |
| XNU uses the carve-out | With a compatible collection loaded, capture `xnu: contiguous DRAM at phys` and `xnu: contiguous DRAM size` from the host log. Mark this stage only when that complete returned interval lies inside the configured carve-out. The separate FKI probe above does not establish this stage. |
| PureDarwin userspace | The real, paired PureDarwin disk and free kernel collection boot; execute a command on the Darwin console and observe the result. Record a clean halt separately. |
| TCG guest shutdown | The ARM64 guest issues `halt`; XNU and FKI unload; Linux powers off; and QEMU exits with status 0. A host-issued stop is cleanup, not a guest shutdown pass. |
| ARM64 hosted macOS desktop | A user-provided compatible system disk and matching XNU collection reach an interactive desktop through the ARM64 FKI/XNU path; record input, rendering backend, and networking separately. |
| Separate native x86_64 hosted macOS desktop experiment | Architecture-matched x86_64 `fki.ko` and `xnu.ko` host Tahoe 26 directly on Linux and reach the desktop on a physical Intel or AMD PC. Record host-integrity checks and guest-initiated clean shutdown; report Intel and AMD separately. |
| Supplemental QEMU/KVM guest desktop | A user-prepared Tahoe 26 x86_64 disk reaches the desktop in a QEMU/KVM guest. This optional launcher does not use the project's x86_64 hosted-XNU modules and is not evidence of the native port. |

## Separate native x86_64 hosted-XNU work status

Native x86_64 hosting of Tahoe 26 remains a separate workstream; direct ARM64 hosting of macOS 27 remains preserved. PR #48 builds the x86_64 FKI host backend, #47 keeps the x86_64 XNU target fail-closed, and #78 adds bounded, read-only Mach-O metadata parsing. These milestones do not execute XNU. Native runtime, physical Intel/AMD boots, host-integrity checks, and guest shutdown remain pending and are not acceptance criteria for the active TCG route.

## Active QEMU TCG host matrix and resource evidence

| Host profile | Current status |
| --- | --- |
| Ubuntu 24.04 x86-64 hosted CI VM, QEMU TCG | PR #75 run [#143](https://github.com/Personed/xnulinux/actions/runs/45897387799) passed all jobs on head `5fa6947`, based on #52 tree `3b47444`, with fixed package-digest checks and the reduced 6 GiB boot. It reached the Darwin console, guest halt, XNU/FKI unload, Linux power-off and QEMU exit 0. The AWS/KVM Ubuntu 24.04.5 VM had 4 vCPUs, Intel Xeon 6975P-C virtual CPU, QEMU 8.2.2, `MemTotal: 16078412 kB`, `MemAvailable: 15085424 kB` before artifacts and `14695776 kB` at boot; peak QEMU RSS was 3,841,412 KiB (3.66 GiB). PR #75 has since merged into #52 as `24e4a9c`, which also contains the #72 TTBR1 guard; current-head image run #148 is in progress, modules #920 are queued, and launcher #454 passed. The repeated #143 boot is evidence for its tested tree, not yet the current #52 head. These are virtual-runner results, not a physical memory floor. Older 16 GiB runs #67/#106, #36/#118 (tested head `c55a700`), and #69/#115 (tested head `3eb5e0e`) remain historical virtual-runner evidence. |
| Physical Intel x86-64 Linux PC, QEMU TCG | Not tested; no physical Intel PC is attached to the available executor. |
| Physical AMD x86-64 Linux PC, QEMU TCG | Not tested; some CI runs used AMD EPYC virtual CPUs, which do not establish bare-metal AMD support. |
| Windows Server 2025 x86-64 host, QEMU TCG | PR #3 run #78 passed the UEFI launcher smoke only. PR #89 head `97b5929` is running the full-route matrix with the reduced profile and same-run artifacts; image run #147 is in progress and modules #905 / launcher #441 passed. No Windows PureDarwin boot result is available yet. #89 still needs synchronization with current #83 / #52 before its result can validate the integrated head. |
| Intel macOS 15 host, QEMU TCG | PR #3 run #78 passed the UEFI launcher smoke only. PR #89 head `97b5929` is running the full-route matrix with the reduced profile and same-run artifacts; image run #147 is in progress and modules #905 / launcher #441 passed. No Intel macOS PureDarwin boot result is available yet. #89 still needs synchronization with current #83 / #52 before its result can validate the integrated head. |

PR #46's older cross-host branch used the historical 16 GiB profile and its earlier run #79 failed in `kclink` before guest boot; it is not evidence for the current reduced profile. PR #89 (head `97b5929`, based on older #83 head `27f0801` and #52 tree `3b47444`) is the active matrix for Ubuntu, Windows Server 2025, and Intel macOS 15. It downloads shared workflow artifacts and checks the guest input hashes before QEMU. Status checked 2026-11-26 22:12 UTC: image run #147 was in progress; modules #905 and launcher #441 passed. No full-route boot result was available. PR #75's package digest checks and repeated boot passed in #143; #75 has since merged into #52 as `24e4a9c`. PR #83 is now head `07d08c5` based on #52 head `24e4a9c`, but #89 has not synchronized to those bases. Current #52 image run #148 and #83 image run #149 were in progress, with module runs #920 / #922 queued and launchers #454 / #455 passed. Do not claim cross-host acceptance until #89 is rebased onto the current stack and its boots pass on that head.

The historical full-route TCG reference assigns 16 GiB to the ARM64 guest, with Linux
`mem=7G`, an 8,156 MiB FKI carve-out at `0x200000000`, and 7,900 MiB XNU RAM. The
20 GiB total / 4 GiB available before artifact preparation screen is planning guidance,
not a measured physical minimum or an acceptance threshold. Current required CI uses a
separate reduced 6 GiB profile. The full route passed in
three recent virtualized Ubuntu jobs: PR #67 run [#106](https://github.com/Personed/xnulinux/actions/runs/45874167659)
reported AMD EPYC 9V74, 4 vCPUs, and `MemTotal: 16367528 kB`; PR #36 run [#118](https://github.com/Personed/xnulinux/actions/runs/45881289094)
reported `MemTotal: 16078392 kB`; and PR #69 run [#115](https://github.com/Personed/xnulinux/actions/runs/45880581266)
reported `MemTotal: 16367504 kB`, `MemAvailable: 14766412 kB`, and exact QEMU peak RSS
9,202,816 KiB (8.78 GiB). They reached the Darwin console command, guest `halt`, XNU/FKI
unload, Linux power-off, and clean QEMU completion. All are VM profiles; the guests were
assigned 16 GiB while runner-visible `MemTotal` was lower, so these runs do not establish
that all guest memory was resident or give a physical host RAM floor.

PR #52 run [#113](https://github.com/Personed/xnulinux/actions/runs/45880530584) failed
before boot on an Ubuntu 24.04.5 KVM runner reporting an Intel Xeon 6975P-C, 4 CPUs, and
`MemTotal: 16078420 kB`: QEMU 8.2.2 reported `Cannot allocate memory` while creating the
configured 16 GiB guest, then exited 1. Image and guest-input preparation passed, but no
guest console or shutdown stage ran. That job did not record `MemAvailable` or its cgroup
memory limit. PR #36's passing #118 runner reported almost the same `MemTotal`, but its
available/cgroup memory was not captured, so total memory alone cannot explain or predict
the allocation result. The minimum host memory remains unmeasured.

PR #72 run #140 on the tree identical to #52 head `3b47444` passed the reduced
profile with 6 GiB QEMU RAM, Linux `mem=2G`, a 3,072 MiB FKI carve-out at `0xc0000000`,
and 2,816 MiB XNU RAM. It reported host `MemTotal: 16078392 kB`,
`MemAvailable: 15173240 kB` before artifacts and `14787768 kB` at boot, plus QEMU peak RSS
3,826,452 KiB (3.65 GiB). The console, guest halt, XNU/FKI unload, Linux power-off, and QEMU
exit 0 passed. The tested init prints `XNULINUX_E2E_XNU_UNLOADED` only after `rmmod xnu`
and `rmmod fki` both succeed ([source at the run's head](https://github.com/Personed/xnulinux/blob/e61f578799257685b2c1741afd7fba43c947cf69/tests/puredarwin/init#L101-L109)). Earlier #52 run #138
and #69 run #115 also passed this profile, with #138 reporting XNU/FKI unload. A separate
local 6 GiB boot measured 3,851,024 KiB (3.67 GiB) RSS. These results establish a
repeatable guest configuration on the named CI/local systems, not a minimum guest or host
requirement; no physical-host test has been run at either memory setting.

PR #75 run [#143](https://github.com/Personed/xnulinux/actions/runs/45897387799) repeated
the complete 6 GiB route after adding fixed package digests. Its Ubuntu 24.04.5 AWS/KVM
runner reported `MemTotal: 16078412 kB`, `MemAvailable: 15085424 kB` before artifact
preparation and `14695776 kB` at boot; QEMU 8.2.2 peaked at 3,841,412 KiB (3.66 GiB).
The run reached the Darwin console command, guest halt, XNU/FKI unload, Linux power-off,
and QEMU exit 0. PR #75 merged into #52 as `24e4a9c`; the new #52 current-head image run
#148 was in progress at 22:12 UTC, modules run #920 remained queued, and launcher #454
passed. The current #52 head therefore awaits a full boot rerun.

The 6 GiB guest allocation is the lowest full-route profile demonstrated in the recorded
successful runs. No run below 6 GiB has established a guest minimum, and the virtual-runner
host measurements do not establish a physical host-RAM minimum.

## Reproducing the active QEMU TCG boot

On an x86-64 Linux PC, install QEMU ARM system emulation and Python 3. On Ubuntu, use
`sudo apt-get install qemu-system-arm python3`; on other distributions install the
equivalent packages. The boot block below only checks for the executables, so it also
works without APT or `sudo` when they are already installed. The current
pinned-kernel implementation is on [PR #52](https://github.com/Personed/xnulinux/pull/52)
and its follow-ups; its `puredarwin-e2e-guest` and `puredarwin-e2e-disk` artifacts must
come from the same successful workflow run. The image workflow builds the pinned
PureDarwin ARM64 input, exports the real userland into an HFS+ disk, builds a matching
source XNU collection, pins the ARM64 Linux kernel packages, compiles FKI/XNU for that
guest, and prepares a direct-boot `Image` and initramfs. Do not mix artifacts across runs.

In a checkout of the pinned-kernel branch, use the guest `Image` and
`initramfs.cpio.gz` from `puredarwin-e2e-guest` and `pd.img` from
`puredarwin-e2e-disk`. Make a writable copy of the disk for every run:

```sh
(
  set +e
  for program in qemu-system-aarch64 python3 timeout; do
    command -v "$program" >/dev/null 2>&1 || {
      echo "$program is not available on PATH; install the required host packages first." >&2
      exit 1
    }
  done
  checksum_file=$(mktemp) || exit 1
  trap 'rm -f "$checksum_file"' EXIT
  sha256sum /path/to/pd.img > "$checksum_file" || exit 1
  cp --reflink=auto /path/to/pd.img /path/to/pd-tcg-test.img || exit 1
  python3 tests/puredarwin/run-tcg-e2e.py \
    --timeout-seconds 5400 \
    --qemu-memory 6G \
    --linux-memory 2G \
    --fki-carve-base 0xc0000000 \
    --fki-carve-mb 3072 \
    --xnu-ram-mb 2816 \
    --max-rss-mib 6144 \
    /path/to/Image \
    /path/to/initramfs.cpio.gz \
    /path/to/pd-tcg-test.img \
    /path/to/puredarwin-tcg-serial.log
  boot_status=$?
  sha256sum --check "$checksum_file"
  disk_status=$?
  test "$boot_status" -eq 0 && test "$disk_status" -eq 0
)
```

The guest writes to the HFS+ disk. Keep the downloaded `pd.img` as the pristine input;
the final command confirms its original SHA-256 did not change.

The command uses ARM64 `virt`, multi-threaded TCG, 2 guest CPUs, and the reduced 6 GiB profile.
It records host and QEMU facts, captures the serial console, sends the required Darwin
command and guest halt, checks XNU/FKI unload and Linux power-off, and returns success
only if QEMU exits 0 and RSS stays below the cap. The full 16 GiB profile is historical
reference evidence, not the current standard-runner target; test it only on a sufficiently
provisioned host. This acceptance workflow is on the #52 branch and is not yet in `main`.

### Run #140 artifact identity

The paired artifacts linked from [run #140](https://github.com/Personed/xnulinux/actions/runs/45891917494)
are a reproducible input set while GitHub retains them. Their manifests record:

| File / input | SHA-256 |
| --- | --- |
| Guest kernel `Image` | `02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6` |
| Guest `initramfs.cpio.gz` | `431c6a2dcdbb77fc1131a8359149a59ad5f4aa5867eff3cf182175cf8a24e322` |
| Guest disk `pd.img` | `84fc7065d5a9bfb87a8db4fe1c3e7c5f485a0c1b23cdcb7beccdd426f03528a1` |
| Free kernel collection `kc-free.bin` | `a75c376745786254a827db587a96bd359ccc274db8685f9ed276c734d1536889` |
| Darwin kernel `kernel.macho` | `a708781c01b971a7837e70ae1118e70ccf5d513db64948c92b92f744c6e34f94` |
| Ubuntu kernel `.deb` | `fa46ed1dc9e13d1c15772122e2cf18622e8cf0aa27b280f6fcb1ccfa196d9048` |
| Ubuntu modules `.deb` | `27cdb8fd11379b622264b224b1f59ae7ecf2b27f387b6a2d0e4a383eae9c3688` |

The guest manifest also records kernel package version `7.0.0-44.44~24.04.1`, kernel
release `7.0.0-44-generic`, 4 KiB pages, runtime PAC, EL1, and source revisions
`xnulinux=7971b6e`, `DroidVM=524d27b`, and `PureDarwin=b3b4b28`. GitHub's API reports
artifact archive digests `sha256:d7f7d21c025249655ab206ab79d208d3354866a1d21bfbabc25eb37a9194b196`
for `puredarwin-e2e-guest` and
`sha256:936add32c29fab498bba74181ede349431e7c84afb4be859cfd4eefa88183dfa` for
`puredarwin-e2e-disk`. Both artifacts expire on 2026-11-29; regenerate the set from the
pinned workflow after expiry. The hashes identify this tested input set, not a permanent
download mirror.

Report the profile alongside the stage:

- **Native ARM64 Linux:** stock Linux with 4 KiB pages, PAC, and the required
  exception level. A result on one board/CPU or kernel does not validate all ARM64 devices.
- **ARM64 Linux guest hosted on another OS:** record the outer machine and OS separately
  from the Linux guest. The ARM64 QEMU path in [#3] uses TCG, including on Intel/AMD hosts;
  distinguish it from the Apple Silicon krunkit path. The module smoke test in [#1]
  is a separate check. Neither substitutes for the PureDarwin test below.
- **Separate native x86-64 hosted XNU work on Intel/AMD Linux:** directly build and execute architecture-matched `fki.ko` and `xnu.ko` on Linux while hosting Tahoe 26. The ARM64/macOS 27 path remains preserved. #48 builds the FKI backend, #47 is fail-closed, and #78 adds read-only Mach-O metadata parsing only; none executes XNU. Native runtime and physical Intel/AMD acceptance remain pending. Keep host results separate and record host-integrity checks and guest-initiated shutdown.
- **x86-64 PC → QEMU TCG → ARM64 Linux:** keep the PC's CPU/OS/QEMU details separate from
  the ARM64 guest. The reference FKI carve-out profile is tracked in [#23]: x86-64 Linux,
  QEMU 10.0.13 TCG, an ARM64 `virt` guest with 1 GiB of RAM and `mem=768M`, and FKI
  configured with `carve_base=0x70000000 carve_mb=256`. Its probe allocates and frees
  16 MiB twice, requiring physical address `0x70000000` on both passes. The guest uses
  a minimal test initramfs, not a full installed Linux distribution. This validates FKI's
  mapping and reuse in that profile; it is not a physical-PC compatibility claim, an XNU
  carve-out use test, or a PureDarwin boot.
- **Active x86-64 PC → QEMU TCG → ARM64 Linux → PureDarwin console:** this full
  route is tracked in [#36](https://github.com/Personed/xnulinux/pull/36). Its acceptance
  workflow builds a paired PureDarwin image and collection, requires a Darwin console
  command and guest-issued halt, then verifies XNU/FKI unload, Linux guest power-off, and
  clean QEMU exit. The reference profile assigns 16 GiB guest RAM with `mem=7G`, uses
  4 KiB pages and PAC at EL1, reserves an 8,156 MiB FKI carve-out at `0x200000000`, and
  assigns 7,900 MiB to XNU. Record host total/available RAM separately from guest RAM.
  The 20 GiB total / 4 GiB available screen is planning guidance, not a measured universal
  minimum. The #36 workflow is an end-to-end build/boot reference; it does not test
  native x86_64 runtime. Current #52 CI uses the reduced profile (`6G`, Linux `mem=2G`,
  3,072 MiB FKI at `0xc0000000`, 2,816 MiB XNU, and a 6,144 MiB QEMU RSS cap). PR #52 pins
  the guest package `7.0.0-44.44~24.04.1` and snapshot `20261126T000000Z`; follow-up #75
  contains fixed `.deb` SHA-256 checks and validation of built-in VirtIO configuration
  when module files are absent.

  Earlier #36 run #72 on head `5312b29` printed console and shutdown markers, but is not
  a valid pass because the built collection contained a file-backed `__TEXT_EXEC` / `__KLD`
  overlap at `0x3854000-0x3858000`. Run #74 on head `97727ac` failed before boot on that
  overlap. PR #50's relocation and the pinned #52 descendant fixed the build path. PR #67
  run #106 then passed the full 16 GiB flow on its #52-based branch: real disk and
  source-built collection, ARM64 guest preparation, Darwin console command, guest halt,
  XNU/FKI unload, Linux power-off, and QEMU exit 0. The runner was Ubuntu 24.04.5 x86-64,
  virtualized, reported AMD EPYC 9V74, 4 vCPUs, and `MemTotal: 16367528 kB`. PR #64 run
  #95 is additional branch evidence; its composite `XNULINUX_E2E_XNU_UNLOADED` marker is
  emitted after both XNU and FKI unload, and the runner requires a zero QEMU return code.
  The retained log does not include a standalone QEMU exit-status line.

  These PR-branch runs are not integrated in `main` and do not establish physical-PC
  acceptance. Status checked 2026-11-26 22:12 UTC: PR #52 head `24e4a9c` includes the
  #72 TTBR1 guard and #75 package digests; image run #148 is in progress, modules run #920
  is queued, and launcher #454 passed. PR #72 run #140 passed the full reduced profile on
  commit `e61f578`, whose Git tree matches #52's earlier head `3b47444`; PR #75 run #143
  passed the same profile plus package digests on its head `5fa6947`, based on that earlier
  tree. PR #3
  run #78 passed only the UEFI launcher smoke on Linux x86-64/ARM64, Windows Server 2025
  x86-64, and macOS 15 Intel/Apple Silicon. PR #83 is head `07d08c5` based on current #52
  `24e4a9c`; image run #149 is in progress, modules #922 are queued, and launcher #455 passed.
  PR #89 head `97b5929` is the active cross-host matrix; image run #147 is in progress and
  modules #905 / launcher #441 passed. It remains based on older #83/#52 heads and needs
  synchronization and a rerun. The Windows and macOS full-route jobs therefore remain
  pending. PR #36 run #118 passed on
  tested head `c55a700`, not current head `93e3cf4`. PR #46 run #79 failed in `kclink`
  before host preparation or boot. Keep every result tied to its tested SHA and base.

  The reduced 6 GiB guest profile passed on #52 head `8d41fa9` in run #138 and on the
  #52-equivalent tree `3421056` in #72 run #140; earlier #69 head `3eb5e0e` also passed.
  A separate local 6 GiB boot measured exact QEMU child RSS 3,851,024 KiB. These results
  demonstrate a repeatable guest configuration, not the minimum guest RAM or minimum host
  RAM. The CI machines are virtualized; the available executor is a Debian 13 x86-64
  container under KVM with 5 vCPUs and an AMD EPYC 9V74 virtual CPU. No bare-metal Intel or
  AMD PC, Windows Server 2025 host, or Intel macOS 15 host is attached. Those are the
  precise hardware/OS validation blockers; do not infer universal “any PC” support from
  virtual-runner results.

  Merged PR #62 records separate local evidence using the exact #52 kernel package
  `7.0.0-44.44~24.04.1`: the prepared guest reached the Darwin console, accepted guest
  `halt`, unloaded `xnu.ko` and `fki.ko`, and powered Linux off. Those runs used a locally
  relocated collection checked with #50's `--check`, not the synchronized CI manifest or
  cross-host matrix. #64 run #95 is also branch evidence, not current-main validation.
  These TCG results do not establish native x86_64 hosting, Tahoe/KVM, a macOS desktop,
  or practical speed on every PC.
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
Profile (ARM64 hosted XNU, native x86_64 hosted XNU, supplemental QEMU/KVM guest, or TCG test):
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
