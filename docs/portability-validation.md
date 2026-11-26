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
- **x86-64 PC → QEMU TCG → ARM64 Linux → PureDarwin console:** this is the active PC
  compatibility route, tracked in [#36]. Its acceptance workflow builds a paired PureDarwin image
  and collection, requires a Darwin console command and guest-issued halt, then verifies XNU/FKI
  unload, Linux guest power-off, and clean QEMU exit. The reference profile assigns 16 GiB
  guest RAM with `mem=7G`, uses 4 KiB pages and PAC at EL1, reserves an 8,156 MiB FKI
  carve-out at `0x200000000`, and assigns 7,900 MiB to XNU. It screens for at least 20 GiB
  total host RAM and 4 GiB available before artifact preparation; record these host values
  separately from guest RAM and note the measurement phase. This is an operational screen
  for that profile, not a measured universal minimum. The #36 workflow still selects the
  latest Ubuntu generic ARM64 kernel package at runtime; PR #52 proposes a pinned package
  (`7.0.0-44.44~24.04.1`) and immutable repository snapshot, but fixed expected package
  digests and availability checks remain pending. Results do not establish native
  x86_64/Tahoe hosting, QEMU/KVM, a macOS desktop, or support on every PC.

  Run #72 on earlier #36 head `5312b29` emitted the Darwin command, guest halt, XNU unload,
  Linux power-off, and QEMU exit markers on Ubuntu 24.04 x86-64 with QEMU 8.2.2. It is not
  a valid acceptance pass: independent byte comparison confirmed file-backed `__TEXT_EXEC`
  and `__KLD` overlap at `0x3854000-0x3858000`, with output bytes matching `__KLD` instead
  of `__TEXT_EXEC`. That runner reported an Intel Xeon 6975P-C, 4 CPUs, and
  `MemTotal: 16078396 kB` while assigning 16 GiB to the guest; this does not show all guest
  memory remained resident or rule out host overcommit/swap. Current #36 head `97727ac`
  failed run #74 before boot on the same file-backed overlap. PR #50's segment relocation
  workflow run #89 passed its own end-to-end Linux x86-64 TCG route on the #36 head plus
  the #50 relocation: source build, guest preparation, Darwin console
  `XNULINUX_USERLAND_OK`, guest halt, XNU/FKI unload, Linux power-off, and QEMU exit all passed
  on Ubuntu 24.04.5 x86-64 with QEMU 8.2.2. Its nested KVM-backed Intel runner reported
  runner-OS `MemTotal` of 15.3 GiB, below the #36 20 GiB reference screen; outer physical
  RAM and available RAM before artifact preparation were not reported. Functional success
  does not show that all guest RAM stayed resident or rule out host overcommit/swap. This is
  The guest init runs `rmmod xnu` and then `rmmod fki`, and emits its unload
  marker only after both checks pass. The relocation code is integrated into the current
  #36 source branch at `93e3cf4`, but run #89 predates that head; no current-head
  end-to-end rerun is recorded yet. This remains virtualized TCG evidence, not physical-PC
  acceptance, native hosted-XNU evidence, or the synchronized #52/#62/#64/#58 host matrix.
  PR #52 run #91 passed the pinned-package build, guest-preparation, and TCG console/halt
  workflow using package `7.0.0-44.44~24.04.1`. Its Microsoft-hypervisor Ubuntu runner
  exposed an AMD EPYC 9V45 virtual CPU, 4 vCPU, and runner-OS `MemTotal` of 16,367,524 kB
  (about 15.6 GiB), also below the 20 GiB reference screen; outer physical and pre-artifact
  available RAM were not reported. This is virtualized AMD-model evidence, not physical
  AMD acceptance. Current #52 head `c360e99` includes #64; modules run #679 and launcher
  run #282 passed, while image run #104 is in progress. Candidate package hashes from run
  #91 still need comparison with #104 before adding fixed expected-digest checks. PR #57
  head `2a4a23f` image run #108 is in progress, with modules run #697
  queued and its launcher check passed. PR #58 image run #90 failed, with modules run #597
  queued; neither result provides a current cross-host acceptance pass. Follow-up PR #64's
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
into main. These are TCG results for the active PC route, limited to the exact heads and
virtualized runners recorded above; they do not establish bare-metal PC support or native
x86_64 acceptance.
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
