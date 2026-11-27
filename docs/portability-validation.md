<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Portability validation

Record the **host, runtime, and furthest verified stage** for each result. Booting an
ARM64 Linux VM does not mean that XNU ran. Building or probing a module does not mean
Darwin userspace ran. A PureDarwin shell does not establish macOS compatibility.

**Goal:** make the system run on PCs by establishing reproducible host profiles. The current acceptance target is x86-64 Linux, Windows, and Intel macOS hosts using QEMU TCG to boot Linux ARM64, load FKI/XNU, reach the PureDarwin console, and shut down cleanly. A profile is accepted only after the complete route is reproduced; hosted-runner results do not establish support on every physical PC. The native x86_64/Tahoe port is separate work. Preserve the direct ARM64 hosted-XNU runtime and macOS 27 target.

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

## Direct ARM64 hosted-XNU profiles

These profiles use the existing direct ARM64 Linux host path; they are separate from the
x86-64 host → QEMU TCG → ARM64 Linux guest route. Both tested ARM64 setups require an ARM64
Linux kernel with PAC support and 4 KiB pages, running at EL1. FKI's contiguous memory source
is host-specific: it can map a pre-reserved carve-out or request CMA allocations where
available. The DGX Spark profile reserves memory with Linux `mem=` and FKI `carve_base`/
`carve_mb`; that range is not allocated from CMA. Do not reuse its addresses or sizes on a
different host. These are tested configurations, not minimum host requirements. See the
[ARM64 host setup](hosted-service.md) and [README evidence](../README.md#status).

| ARM64 host profile | Verified configuration and result | Hardware and evidence limits |
| --- | --- | --- |
| Apple Silicon Mac in an ARM64 Linux VM (`krunkit`) | macOS 27.2.1 reached the desktop with 1–6 hosted CPUs in 31–48 seconds. A 30-minute application and disk stress run completed without panic or crash report. The documented GPU bridge exposes the M5 Max through Virtio-GPU/Venus when a render node is available; CPU rendering remains the fallback. | No total host/VM RAM measurement or RAM floor is recorded. This result covers the tested Apple Silicon `krunkit` VM profile. [README evidence](../README.md#status). |
| NVIDIA DGX Spark, bare-metal ARM64 Linux | Ubuntu 24.04.5 LTS, Linux `7.0.0-1025-nvidia`; 4 hosted CPUs with `cpu_list=5,6,7,8,9` (four XNU CPUs, each assigned a dedicated NVMe queue, plus CPU 9 for the monitor); Linux `mem=112G`; FKI carve-out `carve_base=0x1E00000000`, `carve_mb=8156` (8,156 MiB). macOS 27 reached the desktop with Screen Sharing and SSH; About This Mac reports 7.80 GB memory. | Keep this affinity: the host has 15 NVMe I/O queues for 20 CPUs, and sharing a queue interrupt can deadlock XNU. The recorded desktop used CPU rendering. A separate DGX-specific setup exposes the GB10 via virglrenderer 1.2.0 and Venus; that optional path is documented in [`tools/dgx-spark/README.md`](../tools/dgx-spark/README.md) and was not exercised by this CPU-rendered evidence. Other bare-metal GPU models and a universal RAM floor remain unvalidated. [README evidence](../README.md#status). |

## Active QEMU TCG host matrix and resource evidence

The current complete acceptance run is [#172](https://github.com/Personed/xnulinux/actions/runs/45989875996), from PR #115 code commit [`0daaa50`](https://github.com/Personed/xnulinux/commit/0daaa50e4d2d363bd8d775131b3a298663cf26a2). All six jobs passed: ARM64 image build, PureDarwin HFS+ disk and XNU/free collection build, ARM64 guest preparation, and the three host boots. Each boot used QEMU TCG with ARM64 `virt`, `cortex-a710`, 6 GiB guest RAM, Linux `mem=2G`, a 3,072 MiB FKI carve-out at `0xc0000000`, and 2,816 MiB XNU RAM.

The three boot logs reached the PureDarwin console and ran the requested command, then recorded guest halt, successful XNU/FKI unload, Linux power-off, and a successful harness result that requires QEMU exit 0. Each host used the same inputs: Image SHA-256 `02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6`, initramfs SHA-256 `443333cfa8333e6fc7775b26641116827e3155ed61c78404ae2b652c0f0d84f4`, and PureDarwin disk SHA-256 `c97e031cfd4f9adb4b1539ed9153eaf4b7125e90743d971d922a90395fea5645`.

| Host profile | Verified result and resources before QEMU | Evidence limits |
| --- | --- | --- |
| Ubuntu 24.04 x86-64 hosted CI VM | [Boot log](https://github.com/Personed/xnulinux/actions/runs/45989875996/job/140506795752); QEMU 8.2.2; virtual Xeon 6975P-C, 4 vCPUs; 16,464,306,176 bytes total RAM and 15,138,226,176 available; peak QEMU RSS 3,844,588 KiB (3.67 GiB). | Hosted virtual machine; no physical Intel PC tested. |
| Windows Server 2025 x86-64 hosted VM | [Boot log](https://github.com/Personed/xnulinux/actions/runs/45989875996/job/140506795768); QEMU 11.1.0; virtual AMD64 Family 25 Model 17, 4 vCPUs; 17,174,360,064 bytes total RAM and 13,685,481,472 available. QEMU process RSS was not measured. | Hosted virtual machine; no physical Windows or AMD PC tested. |
| Intel macOS 15 hosted runner | [Boot log](https://github.com/Personed/xnulinux/actions/runs/45989875996/job/140506795714); QEMU 11.1.0; Intel Core i7-8700B, 4 CPUs; 15,032,385,536 bytes total RAM and 10,482,348,032 available; peak QEMU RSS 3,643,496 KiB (3.47 GiB). | Hosted runner; no physical Intel Mac or PC tested. |
| Physical Intel x86-64 PC, QEMU TCG | Not tested. | No bare-metal Intel result. |
| Physical AMD x86-64 PC, QEMU TCG | Not tested. | Virtual AMD runner evidence is not bare-metal evidence. |

This establishes one full pass for each named hosted OS/CPU profile, using `cortex-a710`. It does not test the separate PIE-enabled `-cpu max` profile, direct native ARM64 hosting, or physical PC hardware. The hosted machines showed about 15–17 GB total RAM and 10–15 GB available before QEMU; measured QEMU peaks were 3.47–3.67 GiB on Ubuntu/macOS, and Windows RSS was not captured. The 6 GiB guest setting is a tested configuration, not a minimum. The historical 16 GiB guest profile and 20 GiB total / 4 GiB available planning screen are not measured physical minimums. No universal host RAM floor is established.

Run #158 previously passed an earlier tree, but run #172 supersedes it for the corrected PR #115 code series. Run #170 also passed on an earlier, semantically equivalent patch state; its artifact hashes differ and are not used as current acceptance evidence. The open #115/#89 PR stack is not merged.

### Experimental guest memory profiles

PR #52 run #148 and PR #83 run #149 independently passed the 6 GiB guest profile. PR #96
run [#157](https://github.com/Personed/xnulinux/actions/runs/45907949989), head `e358351`,
then completed five experimental profiles on hosted Ubuntu 24.04 TCG runners using QEMU
8.2.2. Each job used paired artifacts from that workflow, required the PureDarwin console
and guest shutdown path, and retained host-memory and QEMU RSS evidence. The capacity
snapshot was taken before artifact downloads; runner VMs varied between Microsoft-hosted
AMD EPYC and KVM-hosted Intel Xeon profiles:

| QEMU guest RAM | Linux / FKI carve-out / XNU RAM | Ubuntu hosted VM; host RAM total / available before downloads | Peak QEMU RSS | Result |
| --- | --- | --- | --- | --- |
| 5 GiB | `mem=2G` / 3,072 MiB at `0xc0000000` / 2,816 MiB | 4-vCPU Azure/Microsoft VM, AMD EPYC 9V74; 16,367,504 / 15,420,624 kB | 3,820,952 KiB (3.64 GiB) | Full route passed once |
| 4 GiB | `mem=2G` / 1,792 MiB at `0xc0000000` / 1,536 MiB | 4-vCPU KVM VM, Intel Xeon 6975P-C; 16,078,420 / 15,153,160 kB | 2,513,672 KiB (2.40 GiB) | Full route passed once |
| 3 GiB | `mem=1G` / 1,792 MiB at `0x80000000` / 1,536 MiB | 4-vCPU Azure/Microsoft VM, AMD EPYC 9V45; 16,367,496 / 15,453,292 kB | 2,493,508 KiB (2.38 GiB) | Full route passed once |
| 2.5 GiB | `mem=512M` / 1,536 MiB at `0x60000000` / 1,280 MiB | 4-vCPU KVM VM, Intel Xeon 6975P-C; 16,078,416 / 15,147,636 kB | 2,212,348 KiB (2.11 GiB) | Full route passed once |
| 2 GiB | `mem=512M` / 1,280 MiB at `0x60000000` / 1,024 MiB | 4-vCPU KVM VM, Intel Xeon 6975P-C; 16,078,412 / 15,168,000 kB | 1,946,084 KiB (1.86 GiB) | Full route passed once |

The 2 GiB pass in run #157 was the lowest guest allocation observed in that hosted
workflow sweep. A second local harness pass was then made under QEMU 6.2.0 in an x86-64
KVM VM, reusing the paired image, initramfs, and disk from #157. It completed the full
console/shutdown route at all six allocations. The local VM's physical host RAM was not
reported, so this measures guest allocation and QEMU RSS only; it does not establish a
host-RAM floor or physical-PC support.

| QEMU guest RAM | Peak QEMU RSS | Result |
| --- | --- | --- |
| 6 GiB | 3,846,244 KiB | Full route passed once |
| 5 GiB | 3,832,984 KiB | Full route passed once |
| 4 GiB | 2,542,120 KiB | Full route passed once |
| 3 GiB | 2,504,468 KiB | Full route passed once |
| 2.5 GiB | 2,242,084 KiB | Full route passed once |
| 2 GiB | 1,976,184 KiB | Full route passed once |

The local run used `tests/puredarwin/run-tcg-e2e.py` at PR #99's rebased head
[`e1b70f8`](https://github.com/Personed/xnulinux/commit/e1b70f8e35a944eb374e2d37653d0b8cd6bc7420).
The six-profile shutdown and QEMU-exit summary is recorded in [PR #99 comment
7206519486](https://github.com/Personed/xnulinux/pull/99#issuecomment-7206519486). The raw
`run-*.log` and `serial-*.log` files for all six profiles, plus `EVIDENCE.txt` with their
SHA-256 values, are attached in [PR #99 comment
7206879507](https://github.com/Personed/xnulinux/pull/99#issuecomment-7206879507). The
runner's per-profile logs show the Darwin console and userland marker, guest halt,
`XNULINUX_E2E_XNU_UNLOADED`, Linux power-off, and QEMU exit status 0.

Inputs reused from run #157: image SHA-256
`02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6`, initramfs SHA-256
`b95568af05be7ada731758193599f839be555392135614888d3586645904b1b6`, and disk SHA-256
`9ee6ac83e443d0edb50e73169b6c0b26bdce194a7c83ec1a5a8938555ebc6280`. This is a
supplemental local run used the #99 head cited above, not a #99 workflow/build result. The
two virtualized 2 GiB observations remain experimental and do not establish a supported
guest minimum. Repeat the profiles on representative bare-metal Intel and AMD PCs before
publishing guest or host resource floors; the 6 GiB profile remains the repeated baseline.

## Reproducing the active QEMU TCG boot

On an x86-64 Linux PC, install QEMU ARM system emulation and Python 3. On Ubuntu, use
`sudo apt-get install qemu-system-arm python3 coreutils util-linux`. On Alpine, install
the QEMU package that provides `qemu-system-aarch64`, plus `python3`,
`coreutils` (for `sha256sum`) and `util-linux` (for `lscpu`).
The boot block checks all required executables before starting the VM, so it also works
without APT or `sudo` when they are already installed. The pinned-kernel implementation starts in [PR #52](https://github.com/Personed/xnulinux/pull/52) and its follow-ups. For the validated cross-host fixture, use the harness/source at PR #115 head [`0daaa50`](https://github.com/Personed/xnulinux/commit/0daaa50e4d2d363bd8d775131b3a298663cf26a2) and the paired `puredarwin-e2e-guest` and `puredarwin-e2e-disk` artifacts from run [#172](https://github.com/Personed/xnulinux/actions/runs/45989875996): [guest artifact](https://github.com/Personed/xnulinux/actions/runs/45989875996/artifacts/15196743551) and [disk artifact](https://github.com/Personed/xnulinux/actions/runs/45989875996/artifacts/15196177348). Both expire on 2026-12-27. Take both artifacts from that same run; do not mix artifacts across runs. The image workflow builds the pinned
PureDarwin ARM64 input, exports the real userland into an HFS+ disk, builds a matching
source XNU collection, pins the ARM64 Linux kernel packages, compiles FKI/XNU for that
guest, and prepares a direct-boot `Image` and initramfs. Do not mix artifacts across runs.

In a checkout at the tested PR #115 source commit above, use the guest `Image`,
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
  if [ "${CHECK_RUN_172:-0}" = 1 ]; then
    test "$image_sha" = "02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6" && \
    test "$initramfs_sha" = "443333cfa8333e6fc7775b26641116827e3155ed61c78404ae2b652c0f0d84f4" && \
    test "$disk_sha" = "c97e031cfd4f9adb4b1539ed9153eaf4b7125e90743d971d922a90395fea5645" || {
      echo "manifest hashes do not identify the run #172 input set" >&2
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
    --qemu-cpu cortex-a710 \
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
Export `CHECK_RUN_172=1` before running the block to require the exact Image, initramfs,
and disk hashes from run #172's table below. Leave it unset for a regenerated artifact set;
those files must still come from the same workflow run and match that run's manifests.
The plain `cp` works with BusyBox and GNU coreutils without the nonportable
`--reflink` option. Reserve the logical size of `pd.img` as a conservative
space budget; sparse-file or reflink behavior may make the actual allocated copy smaller.

The command uses ARM64 `virt`, multi-threaded TCG, 2 guest CPUs, the tested `cortex-a710` model, and the reduced 6 GiB profile.
It records host and QEMU facts, captures the serial console, sends the required Darwin
command and guest halt, checks XNU/FKI unload and Linux power-off, and returns success
only if QEMU exits 0 and RSS stays below the cap. The full 16 GiB profile is historical
reference evidence, not the current standard-runner target; test it only on a sufficiently
provisioned host. This acceptance workflow is on the #52 branch and is not yet in `main`.

### Run #172 artifact identity

The paired artifacts linked from [run #172](https://github.com/Personed/xnulinux/actions/runs/45989875996) provide the tested guest input set:
[puredarwin-e2e-guest](https://github.com/Personed/xnulinux/actions/runs/45989875996/artifacts/15196743551) and [puredarwin-e2e-disk](https://github.com/Personed/xnulinux/actions/runs/45989875996/artifacts/15196177348).
GitHub lists both as expiring on 2026-12-27. Their manifests record:

| File / input | SHA-256 |
| --- | --- |
| Guest kernel `Image` | `02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6` |
| Guest `initramfs.cpio.gz` | `443333cfa8333e6fc7775b26641116827e3155ed61c78404ae2b652c0f0d84f4` |
| Guest disk `pd.img` | `c97e031cfd4f9adb4b1539ed9153eaf4b7125e90743d971d922a90395fea5645` |

The run used QEMU `cortex-a710`, 6 GiB guest RAM, Linux `mem=2G`, FKI 3,072 MiB at
`0xc0000000`, and XNU RAM 2,816 MiB. This is the input set for the hosted route, not a
host memory minimum. The run's build artifacts include the matching collection and
manifests. Artifacts are not a permanent download mirror. The tested source revision is
[PR #115 head `0daaa50`](https://github.com/Personed/xnulinux/commit/0daaa50e4d2d363bd8d775131b3a298663cf26a2).

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
  acceptance. Status checked 2026-11-27 00:09 UTC: PR #52 head `24e4a9c` includes the
  #72 TTBR1 guard and #75 package digests; image run #148 passed all four image,
  disk/collection, guest-preparation, and 6 GiB boot jobs. PR #72 run #140 passed the
  reduced profile on a tree identical to #52's earlier head `3b47444`; PR #75 run #143
  passed the same profile plus package digests on its earlier head `5fa6947`. PR #3
  run #78 passed only the UEFI launcher smoke on hosted runners.

  PR #83 head `07d08c5` passed image run #149 through the complete 6 GiB TCG boot; its
  modules #922 and launcher #455 also passed. PR #83 was then merged into the #52/#50
  chain as `cc4574a`. This runner result is tied to head `07d08c5`, not bare-metal
  hardware. PR #89 is now retargeted to `codex/kclink-overlap-guard` head `cc4574a`.
  Its run #151 was executed before that retarget, on head `3fbfa6a` based on #83
  `07d08c5` / #52 `24e4a9c`: Ubuntu/QEMU 8.2.2 passed the 6 GiB console and shutdown
  route at 3,820,440 KiB peak RSS; Windows and Intel macOS/QEMU 11.1.0 both failed while
  loading XNU with the same ARM64 level-2 write-permission fault at
  `fffffff007004000`. QEMU exited 0 on those hosts, but the console, halt, and unload
  markers were absent, so neither is a boot pass. Image, disk/collection, guest
  preparation, modules #945, and launcher #476 passed. The Devin diagnosis in
  [PR #89 thread #4714128069](https://github.com/Personed/xnulinux/pull/89#discussion_r4714128069)
  is a hypothesis that the bootstrap window omits write permission bits under ARM64 PIE
  permission indirection; the evidence has not isolated this as the cause. QEMU 11.1.0
  `-cpu max` exposes S1PIE, but PR [#98](https://github.com/Personed/xnulinux/pull/98) run
  [#158](https://github.com/Personed/xnulinux/actions/runs/45911026412) passed all three
  hosted OS jobs once with `cortex-a710`. This CPU model changes other features too, so the
  pass validates that tested profile rather than proving S1PIE caused the fault. PR #102 current head `7ee3e4f` includes the dedicated QEMU 11.1.0 `--require-pie` job with `-cpu max`. Its real
  `TCR2_EL1.PIE=1` result through XNU table update, console, and shutdown remains pending; no
  Actions run is associated with this head. This is a separate PIE-compatibility result and
  does not gate the already passing hosted `cortex-a710` profile.

  PR #96 head `e358351` adds an experimental 5, 4, 3, 2.5, and 2 GiB guest sweep on the
  #52 base. Run [#157](https://github.com/Personed/xnulinux/actions/runs/45907949989)
  completed all image, disk/collection, guest-preparation, standard 6 GiB boot, and five
  experimental-profile jobs successfully. Every experimental profile reached the console
  and guest shutdown, with QEMU exit 0 required by the harness; the detailed measurements
  are recorded above. This is one hosted-VM sweep and does not establish a supported guest
  minimum. PR #98 run #158 passed the full 6 GiB matrix on Ubuntu, Windows Server 2025,
  and Intel macOS once with `cortex-a710`; the route and host measurements are recorded
  above. The run is on the older #89 code stack and does not validate PIE; that status does not invalidate its A710 hosted-runner pass. PR #99 rebases the
  memory sweep onto the integrated chain; run [#159](https://github.com/Personed/xnulinux/actions/runs/45911321313)
  remains queued after the initial disk build hit a remote SDK-fetch timeout and a retry was canceled. The
  supplemental local six-profile QEMU 6.2.0/KVM results using paired #157 artifacts are recorded above, with raw
  per-profile logs linked in PR #99 comment 7206879507. No current-head workflow memory results are available.
  PR #102 current head `7ee3e4f` includes the strict QEMU 11.1.0 `--require-pie` job with `-cpu max`, but no Actions run is associated with this head. Run [#161](https://github.com/Personed/xnulinux/actions/runs/45912879121) tested old head `9e15d68`, passed image build, and was canceled during disk/collection build. Local tests, cross-build checks, patch validation, shell syntax checks, and `actionlint` passed; the clean integrated XNU/DVM build and real PIE boot remain unvalidated. PR #89 run #162 uses old SHA `3020e23049f2752aafc970463850e38d76ce8fe0`, not its previous head `ac74d61`. That previous head differed from #158-tested revision `589c40e` only in this guide; the TCG harness was unchanged. Run #158 therefore validates the same A710 harness and hosted profiles at `589c40e`, but it does not validate the later integrated #89 head `763aef9` or the combined tree after integration with the then-current `main`. On merge commit `7974144`, local regression checks passed: shell syntax, QEMU marker parser (9/9), DVM/free-KC path tests (4/4), and PureDarwin layout/segment checks (19/19). These checks do not replace the full source build and boot; the current PR head still needs its own Actions run. The #158 A710 pass remains evidence for its tested revision and hosted profiles. Earlier run #147 belongs to a
  superseded #89 head. PR #36 run #118 passed on tested head `c55a700`, not current head
  `93e3cf4`; PR #46 run #79 failed in `kclink` before host preparation or boot.
  Earlier Windows matrix job [#71](https://github.com/Personed/xnulinux/actions/runs/45845436111)
  (job [`140017731012`](https://github.com/Personed/xnulinux/actions/runs/45845436111/job/140017731012))
  ran on synthetic merge commit [`3ee7ffb`](https://github.com/Personed/xnulinux/commit/3ee7ffb3ea60b0ffb5314152f4d0cac96e716691),
  combining then-current PR #46 head [`a227f9b`](https://github.com/Personed/xnulinux/commit/a227f9b104509d63080b47cbe1e46e5de9a9a70f)
  with base [`c1c0d39`](https://github.com/Personed/xnulinux/commit/c1c0d39d131933be3235aa78e56dcf4e8a291a6b)
  from the fixed-layout #44 branch. That older job faulted in `macho_copy+0x10c` and
  lacked Darwin, halt, and unload markers; it is separate from current run #151 evidence.

  Module jobs are stage smoke tests, not a full PureDarwin route. Run #158 is one full
  hosted pass per Ubuntu, Windows Server 2025, and Intel macOS profile with `cortex-a710`.
  This is the recorded complete result for those hosted configurations. The separate
  `-cpu max`/PIE profile and representative physical-PC validation remain open; they do
  not gate the A710 pass. No host RAM floor is established.

  The reduced 6 GiB guest profile passed on #52 head `8d41fa9` in run #138 and on the
  #52-equivalent tree `3421056` in #72 run #140; earlier #69 head `3eb5e0e` also passed.
  A separate local 6 GiB boot measured exact QEMU child RSS 3,851,024 KiB. These results
  demonstrate a repeatable guest configuration, not the minimum guest RAM or minimum host
  RAM. The CI machines are virtualized; the available executor is a Debian 13 x86-64
  container under KVM with 5 vCPUs and an AMD EPYC 9V74 virtual CPU. Hosted Windows Server
  2025 and Intel macOS 15 runners have one complete `cortex-a710` pass each in run #158,
  but no physical Intel or AMD PC is attached. Bare-metal hardware acceptance remains open, while PIE-enabled `-cpu max` is a separate
  compatibility profile. Neither changes the recorded A710 hosted-runner result; do not
  infer universal “any PC” support from virtual-runner evidence.

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
