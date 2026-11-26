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
| FKI carve-out allocation | A probe requests a known size and verifies the returned physical interval `[pa, pa + size)` is fully inside the configured carve-out `[carve_base, carve_base + (carve_mb << 20))`. It then frees and repeats the allocation without reloading FKI, verifying the interval is reused. Record the request, returned interval, and configured bounds. This validates allocation and reuse for the tested profile only. The existing `tests/qemu/fki-carve-probe.c` is pinned to the QEMU fixture base `0x70000000`; it does not validate another host's carve-out. |
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
| Ubuntu 24.04 x86-64 hosted CI VM, QEMU TCG | PR #52 run [#148](https://github.com/Personed/xnulinux/actions/runs/45902417451) passed all four jobs on head `24e4a9c`: image, disk/collection, guest preparation, and 6 GiB boot. The Azure/Microsoft VM had 4 vCPUs, virtual AMD EPYC 7763, QEMU 8.2.2, `MemTotal: 16367500 kB`, `MemAvailable: 15418148 kB` before artifacts and `14965924 kB` at boot; peak QEMU RSS was 3,833,380 KiB (3.66 GiB). It reached the Darwin console, guest halt, XNU/FKI unload, Linux power-off, and successful QEMU exit. This virtual-runner result is not a physical memory floor. Earlier runs #138, #140 and #143 remain historical virtual-runner evidence; #143 added fixed package-digest checks on an earlier tree. Older 16 GiB runs #67/#106, #36/#118 (tested head `c55a700`), and #69/#115 (tested head `3eb5e0e`) also remain historical evidence. |
| Physical Intel x86-64 Linux PC, QEMU TCG | Not tested; no physical Intel PC is attached to the available executor. |
| Physical AMD x86-64 Linux PC, QEMU TCG | Not tested; some CI runs used AMD EPYC virtual CPUs, which do not establish bare-metal AMD support. |
| Windows Server 2025 x86-64 host, QEMU TCG | PR #89 run [#151](https://github.com/Personed/xnulinux/actions/runs/45904189412) used head `3fbfa6a` on #83 head `07d08c5` / #52 head `24e4a9c`. Image, disk/collection, and guest preparation passed. QEMU 11.1.0 started the 6 GiB guest and exited 0, but XNU loading caused an ARM64 level-2 write-permission fault at `fffffff007004000`; the Darwin console, userland, halt, and unload markers were absent. Windows host memory before artifact downloads was 17,174,360,064 bytes total and 14,339,944,448 available; QEMU process RSS was not measured. This is a guest-load failure, not Windows support or a Windows-specific hardware limit. |
| Intel macOS 15 host, QEMU TCG | PR #89 run [#151](https://github.com/Personed/xnulinux/actions/runs/45904189412) used the same head and guest input hashes as the Windows row. QEMU 11.1.0 started the 6 GiB guest and exited 0, but the same ARM64 level-2 XNU write-permission fault at `fffffff007004000` prevented the console and shutdown markers. The Intel macOS runner reported 15,032,385,536 bytes total and 10,459,959,296 available before QEMU; peak QEMU RSS was 3,476,652 KiB (3.32 GiB). This is a guest-load failure, not Intel macOS support or a macOS-specific hardware limit. |


PR #52 run [#148](https://github.com/Personed/xnulinux/actions/runs/45902417451) passed all four image, disk/collection, guest-preparation, and TCG-boot jobs on current head `24e4a9c`. The boot ran on Ubuntu 24.04.5 in an Azure VM under Microsoft virtualization, with 4 vCPUs and a virtual AMD EPYC 7763. QEMU 8.2.2 used 6 GiB RAM, Linux `mem=2G`, a 3,072 MiB FKI carve-out at `0xc0000000`, and 2,816 MiB XNU RAM. Host `MemTotal` was 16,367,500 kB; `MemAvailable` was 15,418,148 kB before artifacts and 14,965,924 kB at boot. Peak QEMU RSS was 3,833,380 KiB (3.66 GiB). The log reached Darwin, `XNULINUX_USERLAND_OK`, guest halt, `XNULINUX_E2E_XNU_UNLOADED`, and Linux power-off. The successful boot job means the harness's zero-QEMU-exit requirement passed, although the retained log has no separate exit-status line. This confirms the 6 GiB profile on the current virtual runner, not a physical-PC or RAM floor.
PR #46's older cross-host branch used the historical 16 GiB profile and its earlier run #79 failed in `kclink` before guest boot; it is not evidence for the current reduced profile. PR #89 head `3fbfa6a` is based on #83 head `07d08c5` / #52 head `24e4a9c` and checks shared input hashes before boot. In run #151, the Windows and Intel macOS boots both reached XNU loading, then failed with the same guest write-permission fault; the Ubuntu boot was still queued at 23:16 UTC. The image, disk/collection, and guest preparation jobs passed, as did modules #945 and launcher #476. Current #52 image run #148 passed; #83 image run #149 was in progress, while modules #922 and launcher #455 passed. PR #96 adds a separate informational memory-profile sweep, with image run #157 in progress, modules #989 passed and launcher #511 passed. Its five experimental profiles range from 5 GiB to 2 GiB QEMU RAM; no profile result is available yet. Earlier run #147 belongs to a superseded #89 head and is not current-head evidence. Module jobs are stage smoke tests, not a full PureDarwin route. Do not claim cross-host acceptance until the synchronized #89 runs report the console and guest shutdown on each supported host.

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
exit 0 passed. This run used the valid 4 KiB / 48-bit TTBR1 geometry; it did not inject an
incompatible geometry and does not establish that the guard rejects one. The tested init
prints `XNULINUX_E2E_XNU_UNLOADED` only after `rmmod xnu`
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
and QEMU exit 0. PR #75 merged into #52 as `24e4a9c`; current head `24e4a9c` then passed
run #148 across all four image, disk/collection, guest-preparation, and boot jobs. Its host
measurements and console/shutdown evidence are recorded above.

The 6 GiB guest allocation is the lowest full-route profile demonstrated in completed
runs. PR #96 (head `e358351`, based on #52 `24e4a9c`) adds an informational sweep at 5, 4,
3, 2.5, and 2 GiB. It reuses the same-run image and disk artifacts, checks the full guest
console/halt/XNU-FKI unload/Linux power-off/QEMU-exit path, and retains host-memory and QEMU
RSS evidence per profile. At 23:05 UTC image run #157 was still in progress, while modules
#989 and launcher #511 passed; no below-6-GiB results had been reported. Even a CI pass would identify a virtual-runner guest profile,
not a physical PC support floor. The guest minimum and physical host-RAM minimum remain
unmeasured; compare repeated results on representative physical Intel and AMD PCs before
setting a published floor.

## Reproducing the active QEMU TCG boot

On an x86-64 Linux PC, install QEMU ARM system emulation and Python 3. On Ubuntu, use
`sudo apt-get install qemu-system-arm python3 coreutils util-linux`. On Alpine, install
the QEMU package that provides `qemu-system-aarch64`, plus `python3`,
`coreutils` (for `sha256sum`) and `util-linux` (for `lscpu`).
The boot block checks all required executables before starting the VM, so it also works
without APT or `sudo` when they are already installed. The current
pinned-kernel implementation is on [PR #52](https://github.com/Personed/xnulinux/pull/52)
and its follow-ups; its `puredarwin-e2e-guest` and `puredarwin-e2e-disk` artifacts must
come from the same successful workflow run. The image workflow builds the pinned
PureDarwin ARM64 input, exports the real userland into an HFS+ disk, builds a matching
source XNU collection, pins the ARM64 Linux kernel packages, compiles FKI/XNU for that
guest, and prepares a direct-boot `Image` and initramfs. Do not mix artifacts across runs.

In a checkout of the pinned-kernel branch, use the guest `Image`,
`initramfs.cpio.gz`, `guest-manifest.txt`, and `build-manifest.txt`
from `puredarwin-e2e-guest`, plus `pd.img` from
`puredarwin-e2e-disk`; keep all files from the same workflow run. The build manifest
records the disk hash from the paired disk build. Replace each `/path/to/...`
placeholder below with the actual downloaded file path. Make a writable copy of the disk
for every run:

```sh
(
  set +e
  for program in qemu-system-aarch64 python3 timeout sha256sum lscpu awk; do
    command -v "$program" >/dev/null 2>&1 || {
      echo "$program is not available on PATH; install the required host packages first." >&2
      exit 1
    }
  done
  input_hashes=$(mktemp) || exit 1
  trap 'rm -f "$input_hashes"' EXIT
  image_sha=$(awk -F= '$1 == "arm64_guest_image_sha256" {print $2; exit}' /path/to/guest-manifest.txt)
  initramfs_sha=$(awk -F= '$1 == "arm64_guest_initramfs_sha256" {print $2; exit}' /path/to/guest-manifest.txt)
  disk_sha=$(awk -F= '$1 == "puredarwin_disk_sha256" {print $2; exit}' /path/to/build-manifest.txt)
  valid_sha256() {
    [ "${#1}" -eq 64 ] || return 1
    case "$1" in *[!0123456789abcdefABCDEF]*) return 1 ;; esac
  }
  valid_sha256 "$image_sha" && valid_sha256 "$initramfs_sha" && valid_sha256 "$disk_sha" || {
    echo "paired manifests must provide three complete 64-digit SHA-256 values" >&2
    exit 1
  }
  if [ "${CHECK_RUN_140:-0}" = 1 ]; then
    test "$image_sha" = "02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6" && \
    test "$initramfs_sha" = "431c6a2dcdbb77fc1131a8359149a59ad5f4aa5867eff3cf182175cf8a24e322" && \
    test "$disk_sha" = "84fc7065d5a9bfb87a8db4fe1c3e7c5f485a0c1b23cdcb7beccdd426f03528a1" || {
      echo "manifest hashes do not identify the run #140 input set" >&2
      exit 1
    }
  fi
  printf '%s  %s\n' "$image_sha" /path/to/Image \
    "$initramfs_sha" /path/to/initramfs.cpio.gz \
    "$disk_sha" /path/to/pd.img > "$input_hashes" || exit 1
  sha256sum --check "$input_hashes" || exit 1
  cp /path/to/pd.img /path/to/pd-tcg-test.img || exit 1
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
  sha256sum --check "$input_hashes"
  input_hash_status=$?
  test "$boot_status" -eq 0 && test "$input_hash_status" -eq 0
)
```

The guest writes to the HFS+ disk. Keep the downloaded `pd.img` as the pristine input;
the checksum preflight confirms the Image, initramfs, and disk match their paired manifests
before QEMU starts, and the final command confirms none of those original inputs changed.
Export `CHECK_RUN_140=1` before running the block to require the exact Image, initramfs,
and disk hashes from run #140's table below. Leave it unset for a regenerated artifact set;
those files must still come from the same workflow run and match that run's manifests.
The plain `cp` works with BusyBox and GNU coreutils without the nonportable
`--reflink` option. Reserve the logical size of `pd.img` as a conservative
space budget; sparse-file or reflink behavior may make the actual allocated copy smaller.

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
pinned workflow after expiry. The reproducer reads hashes from the manifests downloaded
with that same run, so a regenerated disk with new GPT identifiers is checked against its
own input hash. The table identifies the #140 input set, not a permanent download mirror.

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
  when module files are absent. The #36 workflow selects the latest Ubuntu generic ARM64
  kernel package at runtime; the #52 profile uses its pinned version and snapshot. Treat
  those as different kernel inputs and compare each run's manifests rather than assuming
  their results use the same kernel.

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
  acceptance. Status checked 2026-11-26 23:16 UTC: PR #52 head `24e4a9c` includes the
  #72 TTBR1 guard and #75 package digests; image run #148 passed, as did modules #920 and launcher #454. PR #72 run #140 passed the full reduced profile on
  commit `e61f578`, whose Git tree matches #52's earlier head `3b47444`; PR #75 run #143
  passed the same profile plus package digests on its head `5fa6947`, based on that earlier
  tree. PR #3
  run #78 passed only the UEFI launcher smoke on Linux x86-64/ARM64, Windows Server 2025
  x86-64, and macOS 15 Intel/Apple Silicon. PR #83 is head `07d08c5` based on current #52
  `24e4a9c`; image run #149 was in progress, while modules #922 and launcher #455 passed. PR #89 head `3fbfa6a`, based on current #83 / #52, is the current
  cross-host matrix. In image run #151, Windows and Intel macOS booted QEMU but failed at
  the same XNU level-2 write-permission fault at `fffffff007004000`; the Ubuntu guest boot
  was queued. Image, disk/collection, guest-preparation, modules #945, and launcher #476
  passed. PR #96 head `e358351` adds the experimental below-6-GiB sweep on the #52 base;
  image run #157 was in progress, while modules #989 and launcher #511 passed. The
  The Windows and Intel macOS runs did not reach the PureDarwin console; they are failed
  guest boots rather than host-specific support results. The Ubuntu boot and all memory-sweep
  profile outcomes remain pending. PR #36 run #118 passed on
  tested head `c55a700`, not current head `93e3cf4`. PR #46 run #79 failed in `kclink`
  before host preparation or boot. Earlier Windows matrix job [#71](https://github.com/Personed/xnulinux/actions/runs/45845436111) (job [`140017731012`](https://github.com/Personed/xnulinux/actions/runs/45845436111/job/140017731012)) ran on synthetic merge commit [`3ee7ffb`](https://github.com/Personed/xnulinux/commit/3ee7ffb3ea60b0ffb5314152f4d0cac96e716691), combining then-current PR #46 head [`a227f9b`](https://github.com/Personed/xnulinux/commit/a227f9b104509d63080b47cbe1e46e5de9a9a70f) with base [`c1c0d39`](https://github.com/Personed/xnulinux/commit/c1c0d39d131933be3235aa78e56dcf4e8a291a6b) from the fixed-layout #44 branch. QEMU 11.1.0 and Linux exited 0 after booting the ARM64 guest, but XNU loading faulted at `macho_copy+0x10c` while writing to read-only memory at `fffffff007004000`; Darwin console, halt, and unload markers were absent. This records an old kernel artifact/layout failure, not Windows incompatibility or a current-head pass. Keep every result tied to its tested SHA and base.

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
   These values confirm FKI configuration only. The existing `tests/qemu/fki-carve-probe.c` is specific to the QEMU
   fixture at `0x70000000`; do not use it to claim allocation on another host profile. Use a probe that checks the full
   interval against that host's configured bounds and repeats allocation after freeing. If no matching probe is
   available, report configuration only and leave allocation/reuse unverified.
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
