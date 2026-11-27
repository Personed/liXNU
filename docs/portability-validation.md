<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Portability validation

Record the **host, runtime, and furthest verified stage** for each result. Booting an
ARM64 Linux VM does not mean that XNU ran. Building or probing a module does not mean
Darwin userspace ran. A PureDarwin shell does not establish macOS compatibility.

The project target is native hosted-XNU on x86_64 Linux PCs, targeting macOS Tahoe 26.7.3 while preserving the direct ARM64 hosted-XNU runtime and macOS 27 target. QEMU TCG → ARM64 Linux → FKI/XNU → PureDarwin and QEMU/KVM guest results are complementary profiles; neither establishes native x86_64 hosting.

| Stage | What counts as evidence |
| --- | --- |
| Firmware | The intended architecture reaches the firmware console (e.g. UEFI shell for the QEMU profile). |
| Linux host or guest | ARM64 Linux reaches the intended userspace; record whether this is a distro boot or a minimal test initramfs, plus the kernel, page size, and PAC availability. |
| FKI configuration | FKI loads with the requested `carve_base` and `carve_mb`; reading those parameters confirms configuration only, not that the reserved pages can be allocated. |
| FKI carve-out allocation | `tests/qemu/fki-carve-probe.c` requests a known size and verifies each returned physical interval `[pa, pa + size)` is fully inside the expected carve-out `[expected_base, expected_base + (expected_mb << 20))`. It frees the first allocation before requesting the second without reloading FKI. The probe logs interval containment for each allocation and reports same-PA reuse separately: two in-bounds markers establish allocation-in-bounds; `same-pa-reuse: yes` establishes reuse of the same PA; `same-pa-reuse: no` means reuse failed even if both intervals were in-bounds. It reports the returned physical ranges and does not inspect FKI's internal allocation handle. Its module parameters are `expected_base`, `expected_mb`, and `request_mb`; `expected_base` must meet FKI's 2 MiB alignment condition. Defaults (`0x70000000`, 256 MiB, and 16 MiB) preserve the QEMU fixture. A different host profile must pass that profile's bounds explicitly. Record the request, returned intervals, configured bounds, and same-PA result. This validates physical interval containment and reuse for the tested profile only. |
| XNU module probe | `xnu.ko` resolves its FKI dependencies and reaches the expected missing-image error. This checks the module and loader path without a real collection; it does not show that XNU allocated from the carve-out. |
| XNU uses the carve-out | With a compatible collection loaded, capture `xnu: contiguous DRAM at phys` and `xnu: contiguous DRAM size` from the host log. Mark this stage only when that complete returned interval lies inside the configured carve-out. The separate FKI probe above does not establish this stage. |
| PureDarwin userspace | The real, paired PureDarwin disk and free kernel collection boot; execute a command on the Darwin console and observe the result. Record a clean halt separately. |
| TCG guest shutdown | The ARM64 guest issues `halt`; XNU and FKI unload; Linux powers off; and QEMU exits with status 0. A host-issued stop is cleanup, not a guest shutdown pass. |
| ARM64 hosted macOS desktop | A user-provided compatible system disk and matching XNU collection reach an interactive desktop through the ARM64 FKI/XNU path; record input, rendering backend, and networking separately. |
| Native x86_64 hosted macOS desktop | Architecture-matched x86_64 `fki.ko` and `xnu.ko` host Tahoe 26 directly on Linux and reach the desktop on a physical Intel or AMD PC. Record host-integrity checks and guest-initiated clean shutdown; report Intel and AMD separately. |
| Supplemental QEMU/KVM guest desktop | A user-prepared Tahoe 26 x86_64 disk reaches the desktop in a QEMU/KVM guest. This optional launcher does not use the project's x86_64 hosted-XNU modules and is not evidence of the native port. |

## Native x86_64 hosted-XNU work status

Native x86_64 hosting of Tahoe 26.7.3 is the project target; direct ARM64 hosting of macOS 27 remains preserved. PR #48 builds the x86_64 FKI host backend, #47 keeps the x86_64 XNU target fail-closed, and #78 adds bounded, read-only Mach-O metadata parsing. These milestones do not execute XNU. Native runtime, physical Intel/AMD boots, host-integrity checks, and guest shutdown remain pending.

## Direct ARM64 hosted-XNU profiles

These profiles use the existing direct ARM64 Linux host path; they are separate from the
x86-64 host → QEMU TCG → ARM64 Linux guest route. Both tested ARM64 setups require an ARM64
Linux kernel with PAC support and 4 KiB pages, running at EL1. FKI's contiguous memory source
is host-specific: it can map a pre-reserved carve-out or request CMA allocations where
available. The DGX Spark profile reserves memory with Linux `mem=` and FKI `carve_base`/
`carve_mb`; that range is not allocated from CMA. Do not reuse its addresses or sizes on a
different host. These are tested configurations, not minimum host requirements.

In the #89 FKI code, `fki_window_l1()` also rejects unsupported `TCR_EL1` geometry:
`T1SZ=16` (48-bit virtual addresses), `TG1=2` (4 KiB granule),
`IPS<=5` (at most 48-bit physical addresses), and `DS=0` (LPA2 descriptor format
disabled). Other values return `-EOPNOTSUPP`. The same guard is present in the #172
test merge, whose TCG ARM64 guest passed the PureDarwin route; this confirms that guest
geometry only. The #172 result does not revalidate the direct-host Krunkit or DGX
profiles above against the guard. Check each host's actual `TCR_EL1` before treating an
older direct-host result as evidence for this #89 revision.

These are historical maintainer-reported results from the [README status](../README.md#status). PR #113 was documentation-only and its public discussion contains no boot logs or attached artifacts; raw logs are not archived in that record. Neither direct-host profile has been revalidated against the `TCR_EL1` guard described above.

See the [ARM64 host setup](hosted-service.md).

| ARM64 host profile | Reported configuration and result | Hardware and evidence limits |
| --- | --- | --- |
| Apple Silicon Mac in an ARM64 Linux VM (`krunkit`) | The maintainer reported macOS 27.2.1 reaching the desktop with 1–6 hosted CPUs in 31–48 seconds. A 30-minute application and disk stress run completed without panic or crash report. The documented GPU bridge exposes the M5 Max through Virtio-GPU/Venus when a render node is available; CPU rendering remains the fallback. | Historical maintainer report; PR #113 has no public boot logs or attached artifacts. No total host/VM RAM measurement or RAM floor is recorded. This covers the reported Apple Silicon `krunkit` VM profile only. |
| NVIDIA DGX Spark, bare-metal ARM64 Linux | The maintainer reported Ubuntu 24.04.5 LTS, Linux `7.0.0-1025-nvidia`; 4 hosted CPUs with `cpu_list=5,6,7,8,9` (four XNU CPUs, each assigned a dedicated NVMe queue, plus CPU 9 for the monitor); Linux `mem=112G`; FKI carve-out `carve_base=0x1E00000000`, `carve_mb=8156` (8,156 MiB). macOS 27 reached the desktop with Screen Sharing and SSH; About This Mac reports 7.80 GB memory. | Historical maintainer report; PR #113 has no public boot logs or attached artifacts. Keep this affinity: the host has 15 NVMe I/O queues for 20 CPUs, and sharing a queue interrupt can deadlock XNU. The recorded desktop used CPU rendering. A separate DGX-specific setup exposes the GB10 via virglrenderer 1.2.0 and Venus; that optional path is documented in [`tools/dgx-spark/README.md`](../tools/dgx-spark/README.md) and was not exercised by this CPU-rendered report. Other bare-metal GPU models and a universal RAM floor remain unvalidated. |

## Complementary QEMU TCG host matrix and resource evidence

At the time of this update, the attached executor is a Debian 13 x86-64 VM under KVM with a virtual AMD EPYC 9V74, 5 vCPUs, and 18,440,100 kB of visible RAM. It does not have `qemu-system-aarch64` installed, so it cannot provide a local TCG boot or bare-metal Intel/AMD evidence. See the [executor access note on PR #107](https://github.com/Personed/xnulinux/pull/107#issuecomment-7207300173).

| Host profile | Current status |
| --- | --- |
| Ubuntu 24.04.5 x86-64 hosted CI VM, QEMU TCG | PR #115 run [#172](https://github.com/Personed/xnulinux/actions/runs/45989875996) records the resource measurements for the 6 GiB route on #115 head `0daaa50` / tree `5f9b1e5` over #89 base `763aef9`: QEMU 8.2.2, `cortex-a710`; host Intel Xeon 6975P-C, 4 logical CPUs; 16,464,306,176 bytes total / 15,138,226,176 available before QEMU; peak RSS 3,844,588 KiB (3.67 GiB). This hosted VM result is not a physical-PC RAM floor. The later integrated run #184 also passed the Ubuntu boot; see below. |
| Physical Intel x86-64 Linux PC, QEMU TCG | Not tested; no physical Intel PC is attached to the available executor. |
| Physical AMD x86-64 Linux PC, QEMU TCG | Not tested; some CI runs used AMD EPYC virtual CPUs, which do not establish bare-metal AMD support. |
| Windows Server 2025 x86-64 hosted VM, QEMU TCG | PR #115 run [#172](https://github.com/Personed/xnulinux/actions/runs/45989875996) passed on the same tested stack (`1b3beb5`); QEMU 11.1.0, `cortex-a710`, guest 6 GiB; AMD64 Family 25 Model 17 Stepping 1, 4 logical CPUs; 17,174,360,064 bytes total / 13,685,481,472 available before QEMU; RSS not measured. This is a hosted VM result, not a physical Windows PC result. |
| Intel macOS 15 hosted runner, QEMU TCG | PR #115 run [#172](https://github.com/Personed/xnulinux/actions/runs/45989875996) passed on the same tested stack (`1b3beb5`); macOS 15.7.9, QEMU 11.1.0, `cortex-a710`, guest 6 GiB; Intel Core i7-8700B, 4 logical CPUs; 15,032,385,536 bytes total / 10,482,348,032 available before QEMU; peak RSS 3,643,496 KiB (3.47 GiB). This is a hosted Intel macOS VM result, not a physical-PC result. |
| Physical Windows 11 x86-64 PC, QEMU TCG | Not tested; the Windows Server 2025 result is from a hosted VM and does not establish Windows client support. |
| Physical Intel macOS host, QEMU TCG | Not tested; the macOS 15 runner is hosted and does not establish a physical Intel Mac profile. |

Run #172 used the same paired ARM64 Linux guest, initramfs, and PureDarwin disk across
all three hosts. All six workflow jobs passed: image creation, real HFS+ disk and
source-built XNU/free collection, ARM64 guest preparation, and three TCG boots. The
common guest profile was 6 GiB QEMU RAM, Linux `mem=2G`, FKI carve-out 3,072 MiB at
`0xc0000000`, and XNU RAM 2,816 MiB. The logs and harness report each validation stage separately:

| Stage | Run #172 evidence and result |
| --- | --- |
| Firmware | Not applicable: QEMU directly loaded the ARM64 Linux kernel and initramfs with `-kernel` and `-initrd`; this profile does not boot firmware. |
| ARM64 Linux guest boot | Passed through the test initramfs path: it loaded the modules and relayed the Darwin console. This is a minimal test initramfs, not a full installed Linux distribution. |
| FKI configuration | Passed: `fki.ko` loaded with `carve_base=0xc0000000` and `carve_mb=3072`. |
| FKI carve-out allocation | Not separately measured: #172 did not run the interval-checking allocation probe. |
| FKI allocation reuse | Not measured: #172 did not free and repeat an allocation. |
| Standalone XNU module probe | Not run: the test loaded `xnu.ko` with the matching real collection rather than performing a missing-image probe. |
| XNU use of FKI carve-out | Partial: all three logs report `xnu: contiguous DRAM at phys 0x00000000c0000000`, matching the configured start. The returned interval size is absent, so containment of the full XNU range is unverified. |
| PureDarwin console and userland | Passed: the console returned `Darwin` and `XNULINUX_USERLAND_OK`. |
| Guest-requested halt | Passed: each log records `fki: xnu: the hosted system halted: run ended` before module unloading. The later `XNULINUX_E2E_GUEST_HALT_OK` summary marker follows module unload and is corroborating evidence, not the independent halt signal. |
| XNU unload | Passed: `rmmod xnu` succeeded after the guest halt. |
| FKI unload | Passed: `rmmod fki` succeeded after XNU unload; the combined `XNULINUX_E2E_XNU_UNLOADED` marker followed both. |
| Linux guest power-off | Passed: the guest powered Linux off and the log reported `reboot: Power down`. |
| QEMU exit | Passed: QEMU returned status 0, required by the harness. |

The three boot logs report the same SHA-256 values for the inputs used by QEMU.

| Boot input | SHA-256 |
| --- | --- |
| ARM64 Linux `Image` | `02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6` |
| `initramfs.cpio.gz` | `443333cfa8333e6fc7775b26641116827e3155ed61c78404ae2b652c0f0d84f4` |
| PureDarwin `pd.img` | `c97e031cfd4f9adb4b1539ed9153eaf4b7125e90743d971d922a90395fea5645` |

Paired artifacts: [guest inputs ZIP](https://github.com/Personed/xnulinux/actions/runs/45989875996/artifacts/15196743551)
(SHA-256 `4f799ad8146a6a90c94a5963eb20f9247e9929154c499149d7c7b8dd6f9788d7`) and
[PureDarwin disk ZIP](https://github.com/Personed/xnulinux/actions/runs/45989875996/artifacts/15196177348)
(SHA-256 `5f0fcda34d96a36702fea01a36c2649f09088a4892f09f7e68ceea2b45865f6e`). The guest
artifact expires 2026-12-27 and the disk artifact expires 2026-12-27; treat them as
short-lived run evidence, not permanent download mirrors. The per-host logs are
[Ubuntu](https://github.com/Personed/xnulinux/actions/runs/45989875996/job/140506795752),
[Windows](https://github.com/Personed/xnulinux/actions/runs/45989875996/job/140506795768),
and [Intel macOS](https://github.com/Personed/xnulinux/actions/runs/45989875996/job/140506795714).

The 6 GiB value is the standard guest configuration, not a measured minimum; lower guest
allocations remain experimental. The measurements above and run #184 are from hosted VMs.
Physical Intel/AMD PCs and their host-RAM floors remain untested, and direct XNU on a Linux
ARM64 host has not been revalidated against this revision.

PR #89 was merged into `main` at `09428b5`; subsequent merges have advanced `main` further.
Run [#184](https://github.com/Personed/xnulinux/actions/runs/46000976799) completed at
14:04:56 UTC on 2026-11-27, before that merge at 14:06 UTC. It passed all six jobs on
synthetic merge `5159a3e` (base `ed620d2`, #89 head `cb4fed3`): image, HFS+ disk/source
XNU collection, ARM64 guest preparation, and three TCG boots on hosted Ubuntu, Windows
Server 2025, and Intel macOS. Each boot reached PureDarwin userland, guest halt, XNU/FKI
unload, Linux power-off, and QEMU exit 0. This is hosted-VM evidence for that tested
pre-merge integration, not an exact-head run of current `main` or physical-PC evidence.

Run #184 artifacts: [guest inputs](https://github.com/Personed/xnulinux/actions/runs/46000976799/artifacts/15204110848),
[disk](https://github.com/Personed/xnulinux/actions/runs/46000976799/artifacts/15202357394),
[Ubuntu log](https://github.com/Personed/xnulinux/actions/runs/46000976799/artifacts/15203931172),
[Windows log](https://github.com/Personed/xnulinux/actions/runs/46000976799/artifacts/15202637581),
and [Intel macOS log](https://github.com/Personed/xnulinux/actions/runs/46000976799/artifacts/15203784361).
These artifacts expire 2026-12-27.

PR #89 run [#151](https://github.com/Personed/xnulinux/actions/runs/45904189412) used the paired [guest/build manifest artifact](https://github.com/Personed/xnulinux/actions/runs/45904189412/artifacts/15158822689) and [disk artifact](https://github.com/Personed/xnulinux/actions/runs/45904189412/artifacts/15159442035). Both Windows and macOS boot logs recorded the same inputs: Image SHA-256 `02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6`, initramfs SHA-256 `a548f8fc4075fd535fece9648c808d97a9460ad520f67a750f221b6ba18babe8`, and disk SHA-256 `269cc4ba7a8ec973cc672d50bffeebb84b9e3a5f9051f567b152d566d8b9acb5`.

PR #98 run [#158](https://github.com/Personed/xnulinux/actions/runs/45911026412) completed image build, disk/collection build, guest preparation, and all three 6 GiB host boot jobs successfully. The jobs used QEMU 8.2.2 on Ubuntu and QEMU 11.1.0 on Windows/macOS, all with `cortex-a710`; each reached the PureDarwin console, guest halt, XNU/FKI unload, Linux power-off, and QEMU exit 0. This is one complete hosted-runner pass per OS for the `cortex-a710` profile. It is the recorded hosted acceptance result for that CPU/OS configuration. It does not isolate S1PIE or exercise the separate PIE-enabled `-cpu max` compatibility profile tracked in PR #102; that profile and physical-PC validation remain separate limitations, not gates for the A710 result.

All three run #158 boot logs record identical input SHA-256 values: ARM64 image
`02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6`, initramfs
`fd6aec2eac9b6a2d77e6c223869d2cf7cf1379150358267329c83c8f9588d667`, and PureDarwin disk
`7c6e4e694e72a748b602142aebb02b06f5bec0d18b0ff9aed85cc4b816df662f`. The same hashes appear in the Ubuntu
[#158 boot log](https://github.com/Personed/xnulinux/actions/runs/45911026412/job/140229870570), Windows
[#158 boot log](https://github.com/Personed/xnulinux/actions/runs/45911026412/job/140229870588), and Intel macOS
[#158 boot log](https://github.com/Personed/xnulinux/actions/runs/45911026412/job/140229870640).

PR #83 run [#149](https://github.com/Personed/xnulinux/actions/runs/45902502697), head `07d08c5`, independently passed the 6 GiB route on a hosted Ubuntu x86-64 VM with QEMU 8.2.2. Host `MemTotal` was 16,078,424 kB and `MemAvailable` was 14,784,852 kB before QEMU. The profile used Linux `mem=2G`, a 3,072 MiB FKI carve-out at `0xc0000000`, and 2,816 MiB XNU RAM; peak QEMU RSS was 3,843,952 KiB (3.67 GiB). The boot reached the PureDarwin console and completed guest halt, XNU/FKI unload, Linux power-off, and QEMU exit 0. This is a second hosted-runner measurement of the 6 GiB profile, not a physical host-RAM floor.


PR #52 run [#148](https://github.com/Personed/xnulinux/actions/runs/45902417451) passed all four image, disk/collection, guest-preparation, and TCG-boot jobs on current head `24e4a9c`. The boot ran on Ubuntu 24.04.5 in an Azure VM under Microsoft virtualization, with 4 vCPUs and a virtual AMD EPYC 7763. QEMU 8.2.2 used 6 GiB RAM, Linux `mem=2G`, a 3,072 MiB FKI carve-out at `0xc0000000`, and 2,816 MiB XNU RAM. Host `MemTotal` was 16,367,500 kB; `MemAvailable` was 15,418,148 kB before artifacts and 14,965,924 kB at boot. Peak QEMU RSS was 3,833,380 KiB (3.66 GiB). The log reached Darwin, `XNULINUX_USERLAND_OK`, guest halt, `XNULINUX_E2E_XNU_UNLOADED`, and Linux power-off. The successful boot job means the harness's zero-QEMU-exit requirement passed, although the retained log has no separate exit-status line. This confirms the 6 GiB profile on the current virtual runner, not a physical-PC or RAM floor.
PR #46's older cross-host branch used the historical 16 GiB profile and its earlier run #79 failed in `kclink` before guest boot; it is not evidence for the current reduced profile. PR #89 run #151 used head `3fbfa6a` on the earlier #83/#52 stack, before PR #89 was retargeted to integrated base `cc4574a`. Ubuntu/QEMU 8.2.2 passed the 6 GiB route; Windows and Intel macOS/QEMU 11.1.0 `-cpu max` failed at the same XNU write-permission fault. PR #98 run #158 then passed all three hosted OS jobs once with `cortex-a710`; its base is still the older #89 head, and the changed CPU model does not isolate S1PIE. This is positive evidence for the tested hosted OS/CPU profiles, not physical-PC support; the A710 result is separate from the PIE profile.

Current #52 run #148 and PR #83 run #149 independently passed the 6 GiB Linux profile. PR #96 run [#157](https://github.com/Personed/xnulinux/actions/runs/45907949989) passed the standard 6 GiB route and all five experimental profiles from 5 GiB to 2 GiB. A supplemental local QEMU 6.2.0/KVM run using the paired #157 artifacts passed all six profiles; see the [experimental guest-memory results](#experimental-guest-memory-profiles) below for RSS, hashes, and per-profile logs, also linked in PR #99 comment 7206879507. Both 2 GiB observations remain experimental, not a supported minimum. PR #99 head `b186f13` passed run [#186](https://github.com/Personed/liXNU/actions/runs/46004186107) on synthetic merge `a7460df` over overlap-guard base `17d4852`; see the current-head profile table below. This result is hosted-VM evidence on a separate base, not validation of exact current `main`.

PR #102 was merged into the overlap-guard branch at `17d4852`. Its run [#161](https://github.com/Personed/xnulinux/actions/runs/45912879121) tested superseded head `9e15d68`, passed image build, and was canceled during disk/collection build. A full strict PIE boot later passed in PR #99 run #186 on head `b186f13` over that overlap-guard base; this validates the tested #99 branch and hosted profile, not exact current `main`.

PR #89 run #162 was queued on obsolete SHA `3020e23`. A manual dispatch on the then-current head `763aef9` built the pinned ARM64 image but failed while applying the source XNU/free-collection patch series; guest preparation and boots were skipped. PR #115 restored the corrected patch series, and run [#172](https://github.com/Personed/xnulinux/actions/runs/45989875996) passed the image, disk/collection, guest, and all three hosted TCG boot jobs on synthetic merge `1b3beb5` (head `0daaa50` into base `763aef9`). PR #115 was merged into #89 at `cb4fed3`. Run [#184](https://github.com/Personed/xnulinux/actions/runs/46000976799) finished at 14:04:56 UTC on 2026-11-27, before PR #89's 14:06 UTC merge into `main` at `09428b5`. #184 passed all six jobs on synthetic merge `5159a3e` with base `ed620d2` and #89 head `cb4fed3`: image, disk and XNU/free collection, guest preparation, and three hosted TCG boots. The #89 merge was integrated into `main` afterward; subsequent commits have advanced that branch. #184 is not an exact-head validation of current `main`. PR #98 run #158 remains historical evidence for its tested A710 revision. Module jobs are stage smoke tests, not a full route. Physical Intel/AMD PC validation and host-RAM floors remain outstanding; lower guest profiles remain experimental.

The historical full-route TCG reference assigns 16 GiB to the ARM64 guest, with Linux
`mem=7G`, an 8,156 MiB FKI carve-out at `0x200000000`, and 7,900 MiB XNU RAM. The
20 GiB total / 4 GiB available before artifact preparation screen is planning guidance,
not a measured physical minimum or an acceptance threshold. The complementary QEMU TCG
workflow introduced in PR #52 uses 6 GiB as its default guest profile. This profile is
specific to that workflow and is not a merge gate for the native x86_64 port. The full
route passed in
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

#### Follow-up sweep on PR #99 head `b186f13` (run #186)

Run [#186](https://github.com/Personed/liXNU/actions/runs/46004186107) tested synthetic merge `a7460df` of PR #99 head
`b186f13` over overlap-guard base `17d4852`. All ten jobs passed, including image/disk/guest
preparation, the standard 6 GiB direct profile, strict PIE, and five experimental RAM profiles.
The x86-64 runner was an Ubuntu 24.04.5 KVM VM with four vCPUs and an Intel Xeon 6975P-C.
All seven boots reached PureDarwin userland, guest halt, XNU/FKI unload, Linux power-off, and
QEMU exit 0. These are hosted-VM observations; they do not establish a physical host-RAM floor.
Profiles below 6 GiB remain experimental, and this synthetic merge is not current `main`.

| Profile | Host `MemTotal` / `MemAvailable` (kB; sample phase) | QEMU / Linux / FKI carve-out / XNU RAM | Peak QEMU RSS | Result |
| --- | --- | --- | --- | --- |
| Standard 6 GiB direct | 16,078,396 / 15,162,388 (before downloads) | QEMU 8.2.2, `cortex-a710`; `mem=2G`, 3,072 MiB at `0xc0000000`, 2,816 MiB | 3,840,560 KiB (3.66 GiB) | Full route passed |
| 5 GiB | 16,367,500 / 15,396,820 (before downloads) | QEMU 8.2.2, `cortex-a710`; `mem=2G`, 3,072 MiB at `0xc0000000`, 2,816 MiB | 3,840,716 KiB (3.66 GiB) | Experimental full route passed |
| 4 GiB | 16,367,500 / 15,427,728 (before downloads) | QEMU 8.2.2, `cortex-a710`; `mem=2G`, 1,792 MiB at `0xc0000000`, 1,536 MiB | 2,537,076 KiB (2.42 GiB) | Experimental full route passed |
| 3 GiB | 16,078,396 / 15,176,416 (before downloads) | QEMU 8.2.2, `cortex-a710`; `mem=1G`, 1,792 MiB at `0x80000000`, 1,536 MiB | 2,506,876 KiB (2.39 GiB) | Experimental full route passed |
| 2.5 GiB | 16,366,488 / 15,429,628 (before downloads) | QEMU 8.2.2, `cortex-a710`; `mem=512M`, 1,536 MiB at `0x60000000`, 1,280 MiB | 2,235,268 KiB (2.13 GiB) | Experimental full route passed |
| 2 GiB | 16,367,500 / 15,376,684 (before downloads) | QEMU 8.2.2, `cortex-a710`; `mem=512M`, 1,280 MiB at `0x60000000`, 1,024 MiB | 1,963,028 KiB (1.87 GiB) | Experimental full route passed |
| Strict PIE, 6 GiB | 16,078,392 / 14,698,976 (at QEMU launch, after downloads and build) | QEMU 11.1.0, `-cpu max`; `TCR2_EL1.PIE=1`, XNU PIE mode 1 | 3,631,400 KiB (3.46 GiB) | Strict PIE boot passed |

The six direct and experimental memory-profile jobs recorded host capacity before artifact
downloads. The strict PIE job sampled at QEMU launch, after downloads and QEMU build; its
value is shown with that later phase and should not be compared as a pre-download snapshot.

Per-profile artifacts from run #186: [standard](https://github.com/Personed/liXNU/actions/runs/46004186107/artifacts/15203454277),
[PIE](https://github.com/Personed/liXNU/actions/runs/46004186107/artifacts/15203584963),
[5 GiB](https://github.com/Personed/liXNU/actions/runs/46004186107/artifacts/15204297821),
[4 GiB](https://github.com/Personed/liXNU/actions/runs/46004186107/artifacts/15202928989),
[3 GiB](https://github.com/Personed/liXNU/actions/runs/46004186107/artifacts/15203339809),
[2.5 GiB](https://github.com/Personed/liXNU/actions/runs/46004186107/artifacts/15203019421),
and [2 GiB](https://github.com/Personed/liXNU/actions/runs/46004186107/artifacts/15203599168).
These log artifacts expire 2026-12-27; the paired guest and disk inputs expire 2026-11-30.

The paired run #186 inputs are [guest ZIP](https://github.com/Personed/liXNU/actions/runs/46004186107/artifacts/15203163361)
(SHA-256 `6edd870db5d8cfd23fc45bb2fd02c9489c98a0d792e49d29ef22a57d46faf687`) and
[disk ZIP](https://github.com/Personed/liXNU/actions/runs/46004186107/artifacts/15202825945)
(SHA-256 `5948892ba98e89bdaaa3ea6a0e9289faf423bcfd15a034818519ac8198bb25a8`). These archive
digests verify the complete ZIPs, not individual files inside them. The guest manifest records the ARM64 Linux `Image` SHA-256
`02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6` and
`initramfs.cpio.gz` SHA-256
`2b20a4e96765e8b20033682fa510c20535957b296ebc0ac44821cf5f2a5b5715`. The guest ZIP's
`build-manifest.txt` records the `pd.img` SHA-256
`9b9b9c2970c17ebf7f9db72560ff291b2664d9537d206a8609ed48367843691a`. These per-file values
come from the published manifests; the retained boot logs do not show a separate re-hash
after extraction. Keep these file hashes distinct from the guest and disk ZIP archive
digests.

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

## Reproducing the complementary QEMU TCG boot profile

On an x86-64 Linux PC, install QEMU ARM system emulation and Python 3. On Ubuntu, use
`sudo apt-get install qemu-system-arm python3 coreutils util-linux`. On Alpine, install
the QEMU package that provides `qemu-system-aarch64`, plus `python3`,
`coreutils` (for `sha256sum`) and `util-linux` (for `lscpu`).
The boot block checks all required executables before starting the VM, so it also works
without APT or `sudo` when they are already installed. The pinned-kernel implementation starts in [PR #52](https://github.com/Personed/xnulinux/pull/52) and its follow-ups. For the validated cross-host fixture, run [#172](https://github.com/Personed/xnulinux/actions/runs/45989875996) used
GitHub's synthetic merge [`1b3beb5`](https://github.com/Personed/xnulinux/commit/1b3beb5088c5f29be0888b41a526ff1fadd83aef), combining
PR #115 head [`0daaa50`](https://github.com/Personed/xnulinux/commit/0daaa50e4d2d363bd8d775131b3a298663cf26a2) with #89 base
`763aef9`. A fresh clone may not be able to fetch that synthetic merge object. The reachable
PR head has Git tree `5f9b1e5`, which is the tree used by the tested merge. Fetch that PR ref
and check out the fixed head SHA:

```sh
git fetch origin refs/pull/115/head:refs/remotes/origin/pr-115
git checkout --detach 0daaa50e4d2d363bd8d775131b3a298663cf26a2
```

Use that revision and the paired `puredarwin-e2e-guest` and `puredarwin-e2e-disk` artifacts
from run #172: [guest artifact](https://github.com/Personed/xnulinux/actions/runs/45989875996/artifacts/15196743551)
and [disk artifact](https://github.com/Personed/xnulinux/actions/runs/45989875996/artifacts/15196177348).
Both expire on 2026-12-27. Take both artifacts from that same run; do not mix artifacts across
runs.

The image workflow builds the pinned
PureDarwin ARM64 input, exports the real userland into an HFS+ disk, builds a matching
source XNU collection, pins the ARM64 Linux kernel packages, compiles FKI/XNU for that
guest, and prepares a direct-boot `Image` and initramfs. Do not mix artifacts across runs.

In a checkout at the reachable PR-head commit above, use the guest `Image`,
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
provisioned host. The workflow and harness have been in `main` since PR #89 was merged at `09428b5`, including the corrected patch series from PR #115; subsequent commits have advanced `main` since that integration. Run #184 completed at 14:04:56 UTC on 2026-11-27, before the 14:06 UTC merge, and passed the full route on synthetic merge `5159a3e` over base `ed620d2`; it is evidence for that tested integration, not an exact-head run of current `main`.

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
manifests. Artifacts are not a permanent download mirror. The tested CI merge commit is
[`1b3beb5`](https://github.com/Personed/xnulinux/commit/1b3beb5088c5f29be0888b41a526ff1fadd83aef), combining PR #115 head
[`0daaa50`](https://github.com/Personed/xnulinux/commit/0daaa50e4d2d363bd8d775131b3a298663cf26a2) with the then-current #89 base
[`763aef9`](https://github.com/Personed/xnulinux/commit/763aef99796fd49930745be8b15d3c8a07b5ee61).

Report the profile alongside the stage:

- **Native ARM64 Linux:** stock Linux with 4 KiB pages, PAC, and the required
  exception level. A result on one board/CPU or kernel does not validate all ARM64 devices.
- **ARM64 Linux guest hosted on another OS:** record the outer machine and OS separately
  from the Linux guest. The ARM64 QEMU path in [#3] uses TCG, including on Intel/AMD hosts;
  distinguish it from the Apple Silicon krunkit path. The module smoke test in [#1]
  is a separate check. Neither substitutes for the PureDarwin test below.
- **Native x86-64 hosted XNU on Intel/AMD Linux:** directly build and execute architecture-matched `fki.ko` and `xnu.ko` on Linux while hosting Tahoe 26.7.3. The ARM64/macOS 27 path remains preserved. #48 builds the FKI backend, #47 is fail-closed, and #78 adds read-only Mach-O metadata parsing only; none executes XNU. Native runtime and physical Intel/AMD acceptance remain pending. Keep host results separate and record host-integrity checks and guest-initiated shutdown.
- **x86-64 PC → QEMU TCG → ARM64 Linux:** keep the PC's CPU/OS/QEMU details separate from
  the ARM64 guest. The reference FKI carve-out profile is tracked in [#23]: x86-64 Linux,
  QEMU 10.0.13 TCG, an ARM64 `virt` guest with 1 GiB of RAM and `mem=768M`, and FKI
  configured with `carve_base=0x70000000 carve_mb=256`. Its probe allocates and frees
  16 MiB twice, requiring physical address `0x70000000` on both passes. The guest uses
  a minimal test initramfs, not a full installed Linux distribution. This validates FKI's
  mapping and reuse in that profile; it is not a physical-PC compatibility claim, an XNU
  carve-out use test, or a PureDarwin boot.
- **Complementary x86-64 PC → QEMU TCG → ARM64 Linux → PureDarwin console:** this full
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
  hardware. Before merge, PR #89 targeted `codex/kclink-overlap-guard` head `cc4574a`; it was merged into `main` at `09428b5`.
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
  pass validates that tested profile rather than proving S1PIE caused the fault. PR #102 was merged into the overlap-guard branch at `17d4852`. The strict PIE path later
  passed in PR #99 run #186 on that branch; that hosted-VM result is recorded above and is
  separate from the `cortex-a710` matrix pass and from exact-current-main validation.

  PR #96 head `e358351` adds an experimental 5, 4, 3, 2.5, and 2 GiB guest sweep on the
  #52 base. Run [#157](https://github.com/Personed/xnulinux/actions/runs/45907949989)
  completed all image, disk/collection, guest-preparation, standard 6 GiB boot, and five
  experimental-profile jobs successfully. Every experimental profile reached the console
  and guest shutdown, with QEMU exit 0 required by the harness; the detailed measurements
  are recorded above. This is one hosted-VM sweep and does not establish a supported guest
  minimum. PR #98 run #158 passed the full 6 GiB matrix on Ubuntu, Windows Server 2025,
  and Intel macOS once with `cortex-a710`; the route and host measurements are recorded
  above. The run is on the older #89 code stack and does not validate PIE; that status does not invalidate its A710 hosted-runner pass. PR #99 head `b186f13` passed all ten jobs in run [#186](https://github.com/Personed/liXNU/actions/runs/46004186107) on synthetic merge `a7460df` over overlap-guard base `17d4852`: image/disk/guest preparation, the standard 6 GiB route, strict PIE, and five experimental memory profiles. The profile measurements and retained logs are recorded above. This is hosted-VM evidence for that branch, not current `main`. The supplemental local six-profile QEMU 6.2.0/KVM results using paired #157 artifacts are also recorded above, with raw per-profile logs linked in PR #99 comment 7206879507. PR #102 was merged into the overlap-guard branch at `17d4852`; its earlier run [#161](https://github.com/Personed/xnulinux/actions/runs/45912879121) used superseded head `9e15d68`, passed image build, and was canceled during disk/collection build. The #99 strict PIE pass is the completed full boot result for that branch; it does not validate exact current `main`. PR #89 run #162 used old SHA `3020e23049f2752aafc970463850e38d76ce8fe0`; it does not validate the integrated branch. PR #115 was merged into #89, then run [#184](https://github.com/Personed/xnulinux/actions/runs/46000976799) completed at 14:04:56 UTC on 2026-11-27, before #89 merged into `main` at 14:06 UTC as `09428b5`. #184 passed all six jobs on synthetic merge `5159a3e` with base `ed620d2` and #89 head `cb4fed3`. This is full-route evidence for that tested integration on hosted Ubuntu, Windows Server 2025, and Intel macOS VMs; it is not an exact-head run of current `main`. Physical Intel/AMD and direct ARM64-host boot validation remain outstanding. The #158 A710 pass remains historical evidence for its tested revision and hosted profiles. Earlier run #147 belongs to a
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
  `-cpu max`/PIE boot passed in PR #99 run #186 over overlap-guard base `17d4852`; validation
  of that profile on exact current `main` and representative physical PCs remains open.
  Neither pending profile gates the A710 pass. No host RAM floor is established.

  The reduced 6 GiB guest profile passed on #52 head `8d41fa9` in run #138 and on the
  #52-equivalent tree `3421056` in #72 run #140; earlier #69 head `3eb5e0e` also passed.
  A separate local 6 GiB boot measured exact QEMU child RSS 3,851,024 KiB. These results
  demonstrate a repeatable guest configuration, not the minimum guest RAM or minimum host
  RAM. The CI machines are virtualized; the available executor is a Debian 13 x86-64
  container under KVM with 5 vCPUs and an AMD EPYC 9V74 virtual CPU. Hosted Windows Server
  2025 and Intel macOS 15 runners have one complete `cortex-a710` pass each in run #158,
  but no physical Intel or AMD PC is attached. Bare-metal hardware acceptance remains open.
  The PIE-enabled `-cpu max` boot passed on the #99 branch in run #186, but is not yet
  validated on exact current `main` or physical hosts. Neither changes the recorded A710
  hosted-runner result; do not infer universal “any PC” support from virtual-runner evidence.

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
   These values confirm FKI configuration only. Build the reusable probe against the same kernel headers as FKI, then
   pass the active carve-out values rather than the QEMU defaults. `expected_base` must be 2 MiB aligned, as required
   for FKI's configured carve-out path. The probe validates the full returned interval for both allocations and
   reports whether the second allocation reused the first PA:
   ```sh
   make -C "/lib/modules/$(uname -r)/build" ARCH=arm64 M="$PWD/tests/qemu" \
     KBUILD_EXTRA_SYMBOLS="$PWD/fki-ko/Module.symvers" modules
   carve_base=$(cat /sys/module/fki/parameters/carve_base)
   carve_mb=$(cat /sys/module/fki/parameters/carve_mb)
   probe_mark="xnulinux-fki-probe-start-$(cat /proc/sys/kernel/random/uuid)"
   printf '<6>%s\n' "$probe_mark" | sudo tee /dev/kmsg >/dev/null || {
     echo "Could not write the FKI probe log marker" >&2
     exit 1
   }
   if sudo insmod tests/qemu/fki-carve-probe.ko \
     expected_base="$carve_base" expected_mb="$carve_mb" request_mb=16; then
     sudo rmmod fki-carve-probe
   fi
   if ! probe_log=$(sudo dmesg); then
     echo "FKI probe evidence unverified: could not read the kernel log" >&2
   elif ! printf '%s\n' "$probe_log" | grep -Fq -- "$probe_mark"; then
     echo "FKI probe evidence unverified: marker missing (logging disabled or record evicted)" >&2
   else
     probe_lines=$(printf '%s\n' "$probe_log" | sed -n "/$probe_mark/,\$p" | grep -F 'fki-carve-probe:' || true)
     if [ -n "$probe_lines" ]; then
       printf '%s\n' "$probe_lines"
     else
       echo "FKI probe evidence unverified: marker present but no probe messages follow it" >&2
     fi
   fi
   ```
   Record the two returned intervals and the `same-pa-reuse` result separately. If `insmod` fails after both
   in-bounds markers and `same-pa-reuse: no`, record allocation-in-bounds as passed and reuse as failed. Otherwise
   report the last completed stage and the probe's failure reason; do not infer a full allocation/reuse pass. If the
   UUID is missing from the captured kernel log, or no probe messages follow it, mark allocation and reuse evidence
   for this attempt as unverified. This includes systems with `printk.devkmsg=off` and logs where the marker or
   subsequent probe records were evicted; do not use unscoped lines from an earlier attempt as evidence.
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
