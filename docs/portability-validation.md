<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Portability validation

Record the **host, runtime, and furthest verified stage** for each result. Booting an
ARM64 Linux VM does not mean that XNU ran. Building or probing a module does not mean
Darwin userspace ran. A PureDarwin shell does not establish macOS compatibility.

The active x86_64 target is native hosted XNU on Intel/AMD Linux with Tahoe 26; its port
and physical-host acceptance are not complete. The complementary x86-64 TCG → ARM64
Linux → PureDarwin console work in [PR #36](https://github.com/Personed/xnulinux/pull/36)
and the QEMU/KVM guest launcher do not establish the native port.

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
- **x86-64 PC → QEMU TCG → ARM64 Linux → PureDarwin console:** this complementary full
  route is tracked in [#36]. Its acceptance workflow builds a paired PureDarwin image and
  collection, requires a Darwin console command and guest-issued halt, then verifies XNU/FKI
  unload, Linux guest power-off, and clean QEMU exit. The reference profile assigns 16 GiB
  guest RAM with `mem=7G`, uses 4 KiB pages and PAC at EL1, reserves an 8,156 MiB FKI
  carve-out at `0x200000000`, and assigns 7,900 MiB to XNU. It screens for at least 20 GiB
  total host RAM and 4 GiB available before artifact preparation; record these host values
  separately from guest RAM and note the measurement phase. This is an operational screen
  for that profile, not a measured universal minimum. The #36 workflow selects the latest Ubuntu generic ARM64 kernel package at runtime.
  PR #52 pins the candidate package (`7.0.0-44.44~24.04.1`) and repository snapshot; follow-up
  PR #75 adds fixed package-digest and built-in VirtIO checks, but remains a separate branch
  rather than main-integrated evidence. TCG results do not establish native x86_64/Tahoe,
  QEMU/KVM, a macOS desktop, or support on every PC.

  Run #72 on earlier #36 head `5312b29` emitted the Darwin command, guest halt, XNU unload,
  Linux power-off, and QEMU exit markers on Ubuntu 24.04 x86-64 with QEMU 8.2.2. It is not
  a valid acceptance pass: independent byte comparison confirmed file-backed `__TEXT_EXEC`
  and `__KLD` overlap at `0x3854000-0x3858000`, with output bytes matching `__KLD` instead
  of `__TEXT_EXEC`. That runner reported an Intel Xeon 6975P-C, 4 CPUs, and
  `MemTotal: 16078396 kB` while assigning 16 GiB to the guest; this does not show all guest
  memory remained resident or rule out host overcommit/swap. Then-current #36 head `97727ac` failed run #74 before boot on the same file-backed overlap. PR #50's segment relocation
  workflow run #89 passed its own end-to-end Linux x86-64 TCG route on the #36 head plus
  the #50 relocation: source build, guest preparation, Darwin console
  `XNULINUX_USERLAND_OK`, guest halt, XNU unload, Linux power-off, and QEMU exit all passed
  on Ubuntu 24.04.5 x86-64 with QEMU 8.2.2. Its nested KVM-backed Intel runner reported
  runner-OS `MemTotal` of 15.3 GiB, below the #36 20 GiB reference screen; outer physical
  RAM and available RAM before artifact preparation were not reported. Functional success
  does not show that all guest RAM stayed resident or rule out host overcommit/swap. This is
  TCG evidence for that workflow, not native-hosted-XNU evidence or the synchronized
  cross-host acceptance. PR #50's relocation was merged into the #36 branch; the #36 chain
  remains a PR branch, not main-integrated evidence.
  PR #52 run #91 passed the pinned-package build, guest-preparation, and TCG console/halt
  workflow using package `7.0.0-44.44~24.04.1`. Its Microsoft-hypervisor Ubuntu runner
  exposed an AMD EPYC 9V45 virtual CPU, 4 vCPU, and runner-OS `MemTotal` of 16,367,524 kB
  (about 15.6 GiB), also below the 20 GiB reference screen; outer physical and pre-artifact
  available RAM were not reported. This is virtualized AMD-model evidence, not physical
  AMD acceptance. At that time #52 head `c360e99` included #64; modules run #679 and launcher run #282
  passed while image run #104 was in progress. That snapshot is historical, not the current
  #52 head. Candidate hashes from run #91 were still under review then; follow-up PR #75
  later adds fixed package digests and built-in VirtIO checks, separate from runs #138/#140.
  PR #57 head `2a4a23f` run #108 and PR #58 run #90 are also historical and do not
  constitute current cross-host acceptance. Follow-up PR #64's
  synchronized head `5570fae` passed run #95 on its #52 branch: all four jobs passed for the
  pinned image, real PureDarwin disk/HFS+ and collection build, ARM64 guest preparation, and
  x86-64 TCG boot. The Ubuntu 24.04.5 x86-64 AWS-hosted virtual runner reported 4 CPUs,
  `MemTotal: 16078424 kB`, and QEMU 8.2.2; the profile used 16 GiB guest RAM, Linux `mem=7G`,
  and an 8,156 MiB FKI carve-out at `0x200000000`. The console recorded `Darwin`,
  `XNULINUX_USERLAND_OK`, `XNULINUX_E2E_GUEST_HALT_OK`, `XNULINUX_E2E_XNU_UNLOADED`, and
  Linux `reboot: Power down`. The [run-tcg-e2e.py script at head `5570fae`](https://github.com/Personed/xnulinux/blob/5570faef5f457eb9426a00102db4d73062de5c64/tests/puredarwin/run-tcg-e2e.py)
  requires QEMU's return code to be 0, and the boot job passed. It does not check or emit a
  separate FKI-unload marker, so FKI unload is not verified by this run; the retained log
  also has no standalone QEMU exit-status line. Run [#95](https://github.com/Personed/xnulinux/actions/runs/45867603529)
  validates that synchronized #52/#64 branch on this virtual runner only. PR #64 merged into the #52
  branch as `c360e99`; this is not main-integrated or cross-host matrix acceptance. These
  stacked results do not replace a current #36/#46 console/halt run with the same manifest.
  PR #46 run #79 failed in `kclink` on the same overlap before guest preparation or the host
  matrix, so it provides no Windows or Intel-macOS boot result.
  The earlier Windows run #71 faulted while loading XNU with an older kernel layout and is
  not a Windows compatibility result. The proposed Windows (16 GB) and Intel-macOS (14 GB)
  runners are below the 16 GiB guest allocation plus host overhead; a larger runner or
  separately validated profile remains necessary. Status checked 2026-11-26 18:37 UTC.

Merged PR #62 records separate local evidence using the exact #52 kernel package
`7.0.0-44.44~24.04.1`: both ARM64 modules built against its headers, the prepared guest
inputs ran under QEMU TCG, and the harness reached the Darwin console, accepted guest
`halt`, unloaded `xnu.ko` and `fki.ko`, and powered Linux off. Those runs used a locally
relocated collection checked with #50's `--check`, not the synchronized #50/#52 CI
manifest or cross-host matrix. #62's modules run #613 passed Linux 6.12 and ARM64 smoke
jobs, while its x86-64 FKI, SPDX, and QEMU matrix jobs were cancelled. PR #64's separate
run #95 CI result is described above; it is on the #52 branch and has not been integrated
into main. These are useful complementary TCG results, not native x86_64 acceptance.

PR #52 head `8d41fa9` passed its current CI workflow in run [#138](https://github.com/Personed/xnulinux/actions/runs/45891610133). All four jobs succeeded: pinned PureDarwin image build, real HFS+ disk and source XNU collection build, ARM64 Linux/FKI/XNU guest preparation, and x86-64 QEMU TCG boot. The guest and disk artifacts were downloaded from that same workflow run. The boot ran on an Ubuntu 24.04.5 Azure-hosted VM under Microsoft virtualization with 4 vCPUs, Intel Xeon Platinum 8573C, QEMU 8.2.2, `MemTotal: 16366484 kB`, and `MemAvailable: 15398364 kB` before artifact downloads (`14878964 kB` at boot). It used 6 GiB QEMU RAM, Linux `mem=2G`, a 3,072 MiB FKI carve-out at `0xc0000000`, and 2,816 MiB XNU RAM. The log recorded the Darwin command, guest-requested halt, XNU unload, and Linux power-off; the harness required QEMU exit 0. Peak QEMU RSS was 3,835,984 KiB (3.66 GiB), below the 6,144 MiB cap. This is a successful virtualized CI profile, not physical Intel/AMD PC acceptance or a host/guest memory minimum. It predates #72's TTBR1 guard and #75's package/hash follow-up, and it does not validate the native x86_64/Tahoe runtime.
PR #72 then validated the ARM64 TTBR1 geometry guard in run [#140](https://github.com/Personed/xnulinux/actions/runs/45891917494): image, real HFS+ disk and source XNU collection, guest preparation, and the reduced TCG boot all succeeded. The boot used the same 6 GiB / `mem=2G` / 3,072 MiB FKI / 2,816 MiB XNU profile on an Ubuntu 24.04.5 AWS/KVM VM (4 vCPUs, Intel Xeon 6975P-C, QEMU 8.2.2). It reported `MemTotal: 16078392 kB`, `MemAvailable: 15173240 kB` before artifacts (`14787768 kB` at boot), and 3,826,452 KiB (3.65 GiB) peak QEMU RSS. The console command, guest `halt`, XNU unload, Linux power-off, and QEMU exit 0 passed. Run #140 tested commit `e61f578`; its Git tree `3421056` is identical to #52 branch commit `3b47444`, so this run validates the guard-enabled code in that #52 tree for the normal 4 KiB/48-bit guest geometry. The run did not inject an incompatible TTBR1 geometry and does not establish rejection behavior for invalid geometries. It remains virtualized branch evidence, not bare-metal or native x86_64 acceptance.

The retained same-run manifests record the input and output hashes used by both boots. These are the SHA-256 values from `guest-manifest.txt` and `build-manifest.txt` inside each run's `puredarwin-e2e-guest` artifact:

```text
#138 guest-manifest.txt
arm64_guest_kernel_image_deb_sha256=fa46ed1dc9e13d1c15772122e2cf18622e8cf0aa27b280f6fcb1ccfa196d9048
arm64_guest_kernel_modules_deb_sha256=27cdb8fd11379b622264b224b1f59ae7ecf2b27f387b6a2d0e4a383eae9c3688
arm64_guest_vmlinuz_sha256=d90fc73284ec528262411c81d4371a7c2543140a0561a166f6b80ff322cc25f3
arm64_guest_kernel_config_sha256=124f25a7f41f186fc8e9a9538e4df81147411ab87e0ebdc70cb3bbb04b998a15
arm64_guest_kernel_modules_tree_sha256=f64e7630e97992ef6f969c4e3b5430bf281360b65fc718720b36e452f8e9233b
arm64_guest_image_sha256=02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6
arm64_guest_initramfs_sha256=6cc95da4dcf89f7a1f6a0af138fac066fb302f4c32d39be888b2f7829abccf6a
arm64_guest_fki_module_sha256=ed380792ac2ddcfc798e523eb250930c7ff53a7c1533a5f95399b8107f23f806
arm64_guest_xnu_module_sha256=96295de4b1eaaf4767cf721d2c67ca41fe795336086528f0881e1ac11c4a7e7c
#138 build-manifest.txt
puredarwin_compressed_image_sha256=71a800d533015ef7c62bca0ca7cf27fcc4914a1fbb1d8b5fdb528d9bfca58335
puredarwin_disk_sha256=2f11444f34326be943c9062ca64497b6665b3eb827487cf65cb5179325809c8c
free_collection_sha256=dfb2344cc735e8534435bd87259208e7b4d4ba650c2a9352be017d41a4225879
xnu_kernel_sha256=0b108521db2990e3ca1727fd67fd4d0292b35414c07e770c81a10e9e18191958


#140 guest-manifest.txt
arm64_guest_kernel_image_deb_sha256=fa46ed1dc9e13d1c15772122e2cf18622e8cf0aa27b280f6fcb1ccfa196d9048
arm64_guest_kernel_modules_deb_sha256=27cdb8fd11379b622264b224b1f59ae7ecf2b27f387b6a2d0e4a383eae9c3688
arm64_guest_vmlinuz_sha256=d90fc73284ec528262411c81d4371a7c2543140a0561a166f6b80ff322cc25f3
arm64_guest_kernel_config_sha256=124f25a7f41f186fc8e9a9538e4df81147411ab87e0ebdc70cb3bbb04b998a15
arm64_guest_kernel_modules_tree_sha256=f64e7630e97992ef6f969c4e3b5430bf281360b65fc718720b36e452f8e9233b
arm64_guest_image_sha256=02d3f3935c6f91d19e6175d2f5105bb3afd95aadc6e6fb920baeb2a89555e9f6
arm64_guest_initramfs_sha256=431c6a2dcdbb77fc1131a8359149a59ad5f4aa5867eff3cf182175cf8a24e322
arm64_guest_fki_module_sha256=83f25673db60a4d3246f30c7e10d9f053334d7122d2384e2d9dc0e1551cbe20d
arm64_guest_xnu_module_sha256=96295de4b1eaaf4767cf721d2c67ca41fe795336086528f0881e1ac11c4a7e7c
#140 build-manifest.txt
puredarwin_compressed_image_sha256=66dd9f201ba4d0df24785865236ae3b252e6d0e5529fb8731a188e07b74f658b
puredarwin_disk_sha256=84fc7065d5a9bfb87a8db4fe1c3e7c5f485a0c1b23cdcb7beccdd426f03528a1
free_collection_sha256=a75c376745786254a827db587a96bd359ccc274db8685f9ed276c734d1536889
xnu_kernel_sha256=a708781c01b971a7837e70ae1118e70ccf5d513db64948c92b92f744c6e34f94
```

Both boot logs contain `XNULINUX_E2E_XNU_UNLOADED`. In the exact guest-init scripts from #138 head `8d41fa9` and #140 commit `e61f578`, init runs `rmmod xnu`, confirms `/sys/module/xnu` is absent, runs `rmmod fki`, confirms `/dev/xnu-hostcall` is absent, and emits the common success marker only after both unload checks pass. These runs therefore validate both unload commands; the serial log has no standalone FKI-unload marker.

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
