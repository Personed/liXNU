<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Portability validation

Record the **host, runtime, and furthest verified stage** for each result. Booting the
ARM64 Linux VM does not mean that XNU ran. Building or probing a module does not mean
Darwin userspace ran. A PureDarwin shell does not establish macOS compatibility.

| Stage | What counts as evidence |
| --- | --- |
| Firmware | The intended architecture reaches the firmware console (e.g. UEFI shell for the QEMU profile). |
| Linux host or guest | ARM64 Linux reaches the intended userspace; record whether this is a distro boot or a minimal test initramfs, plus the kernel, page size, and PAC availability. |
| FKI configuration | FKI loads with the requested `carve_base` and `carve_mb`; reading those parameters confirms configuration only, not that the reserved pages can be allocated. |
| FKI carve-out allocation | A probe requests a known size and verifies the returned physical interval `[pa, pa + size)` is fully inside the configured carve-out `[carve_base, carve_base + (carve_mb << 20))`. It then frees and repeats the allocation without reloading FKI, verifying the interval is reused. Record the request, returned interval, and configured bounds. This validates allocation and reuse for the tested profile only. |
| XNU module probe | `xnu.ko` resolves its FKI dependencies and reaches the expected missing-image error. This checks the module and loader path without a real collection; it does not show that XNU allocated from the carve-out. |
| XNU uses the carve-out | With a compatible collection loaded, capture `xnu: contiguous DRAM at phys` and `xnu: contiguous DRAM size` from the host log. Mark this stage only when that complete returned interval lies inside the configured carve-out. The separate FKI probe above does not establish this stage. |
| PureDarwin userspace | The real, paired PureDarwin disk and free kernel collection boot; a command produces output on the Darwin console. Record the command and output. This confirms console userspace, not clean shutdown. |
| TCG guest shutdown | The guest issues `halt`; XNU unloads; ARM64 Linux powers off (`reboot: Power down`); and QEMU exits with status 0. Record each marker. Stopping QEMU from the host is not a guest shutdown pass. |
| ARM64 hosted macOS desktop | A user-provided compatible system disk and matching XNU collection reach an interactive desktop through the ARM64 FKI/XNU path; record input, rendering backend, and networking separately. |
| Separate x86_64 Tahoe/KVM experiment | A user-prepared Tahoe 26 x86_64 disk with EFI/OpenCore reaches the desktop under QEMU/KVM on physical Intel or AMD hardware. This launcher does not use the project's ARM64 XNU collection; record KVM use, host-integrity checks, and guest-initiated clean shutdown. |

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
- **x86-64 macOS guest on Intel/AMD Linux:** the QEMU/KVM launcher in [#16] is a
  separate experiment, not the active x86-64 TCG/PureDarwin target and not a port of the ARM64 XNU/FKI modules. It remains experimental
  until physical Intel and AMD hosts reach the Tahoe 26 desktop and pass host-integrity
  and clean-shutdown checks. Report KVM and TCG separately; an x86_64 Linux TCG smoke is
  not evidence of macOS boot. For each physical host, record the CPU vendor/model/features,
  Linux kernel, QEMU version and accelerator, Tahoe build, OpenCore configuration, desktop
  result, host-integrity checks before and after, and guest-initiated shutdown. The ARM64
  collection and FKI carve-out fields below do not apply to this profile. See
  [`tests/x86_64/README.md`](../tests/x86_64/README.md) for host prerequisites and limits.
- **x86-64 PC → QEMU TCG → ARM64 Linux → PureDarwin console:** acceptance is tracked
  in [#36]. The reference job runs on Ubuntu 24.04 x86-64 with QEMU TCG and a 16 GiB
  ARM64 Linux guest using `mem=7G`, 4 KiB pages, PAC at EL1, an 8,156 MiB FKI carve-out
  at `0x200000000`, and 7,900 MiB for XNU. Record host CPU model/core count, OS/version,
  total and available physical RAM, QEMU version, guest kernel release, and the complete
  guest kernel `.config` or its SHA-256; report host RAM separately from the guest's
  16 GiB allocation. Before artifact preparation, apply the reference-profile capacity
  screen of at least 20 GiB total host RAM and 4 GiB available RAM, and record when those
  measurements were taken. This operational screen is not a measured universal minimum
  and does not establish practical performance on lower-memory PCs. Until PR #52 is
  integrated and its availability checks pass, the #36 workflow selects Ubuntu's latest
  generic ARM64 kernel package at run time; #52 proposes pinning package
  `7.0.0-44.44~24.04.1` and recording input hashes. The first run (#69) built the
  pinned image and HFS+ disk but failed at `kclink` on a `__TEXT_EXEC`/`__PRELINK_INFO`
  overlap. At head `5312b29`, [run #72](https://github.com/Personed/xnulinux/actions/runs/45845795576)
  completed the source build, guest preparation, and QEMU TCG boot on Ubuntu 24.04 x86-64 with QEMU 8.2.2. The
  runner reported an Intel Xeon 6975P-C, 4 CPUs, and `MemTotal: 16078396 kB`; the guest used Linux
  `7.0.0-44-generic`, 4 KiB pages, PAC at EL1, and the 16 GiB allocation. It emitted the Darwin command, guest halt,
  `xnu.ko` unload, Linux `reboot: Power down`, and QEMU exit markers. This is historical evidence only, not an
  acceptance pass: independent byte comparison confirmed that the historical collection's file-backed
  `__TEXT_EXEC`/`__KLD` overlap at `0x3854000-0x3858000` overwrote bytes; output matched `__KLD`, not `__TEXT_EXEC`.
  The runner's physical RAM was also below the guest allocation, so it does not establish that all guest RAM stayed
  resident or that the host avoided swap/overcommit. Current #36
  head `97727ac` adds a file-backed range check; run #74 failed in `kclink` before boot because file-backed
  `__TEXT_EXEC` and `__KLD` overlap at `0x3854000-0x3858000`, so run #72 is historical observation and not acceptance.
  PR #50 proposes relocating `__KLD`/`__LAST` and relinking; current head is `eb01aba` and run #89 is in progress.
  PR #52 pins the guest inputs and is rebased onto `eb01aba`; run #91 is in progress. It uses an immutable Ubuntu
  snapshot, but fixed expected hashes for downloaded packages and package-availability validation are pending.
  PR #57 is also rebased onto `eb01aba`; run #92 is in progress to validate retention of final post-relink segment-check
  output. Cross-host
  Windows and Intel-macOS checks are proposed in [#46](https://github.com/Personed/xnulinux/pull/46); its run #79
  built the guest image, then failed the XNU source build in `kclink` on the same file-backed `__TEXT_EXEC`/`__KLD`
  overlap. Guest preparation and host boots were skipped. It is based on the pre-relocation, pre-pin head and gives
  no host acceptance result. PR #58 adds pre-boot guest image/initramfs hash verification and host-only reports for early failures; its
  run #90 is building the guest image. Follow-up tests cover hash mismatch blocking the QEMU command, document that
  the Python RAM sample precedes artifact hash validation, and preserve raw-byte SHA-256 for malformed UTF-8
  manifests. Review still asks for coverage of the macOS `vm_stat` unavailable-pages case.
  After these changes are synchronized, repeat the
  console/halt flow and host matrix with matching guest input hashes. An earlier Windows job (#71) started QEMU 11.1.0 but hit an
  ARM64 write-permission fault in `macho_copy` while loading `xnu.ko` with the older #44 kernel layout; it did not
  reach a Darwin marker, so this is not a Windows host pass or a Windows host limitation. PR #50 proposes relocating
  The proposed public runners have 16 GB (Windows) and 14 GB (Intel macOS), below guest allocation
  plus host overhead; larger runners or a validated lower-memory profile are needed
  before those rows can be supported. This profile does not establish a universal
  minimum, performance on every PC, a desktop, GPU/networking, or macOS/KVM.

## First end-to-end check for a new ARM64 FKI host profile

This procedure runs XNU directly on an ARM64 Linux host. It verifies a guest-issued Darwin halt and XNU unload, but does
not power off the outer Linux host. The x86-64 TCG acceptance above separately requires power-off of its disposable
ARM64 Linux guest and a clean QEMU exit.

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
Profile (x86-64 TCG/PureDarwin, native ARM64, ARM64 hosted macOS desktop, or separate x86_64 Tahoe/KVM experiment):
Outer machine + CPU model/core count / OS + version:
Host physical RAM total / available RAM before the guest:
Host RAM measurement phase (native preflight before downloads/setup / Python runner before artifact hash checks and QEMU / final report):
20 GiB / 4 GiB capacity screen result and any limiting reason:
Runtime + version / accelerator:
TCG Linux stage and result (if applicable; do not label as a macOS result):

ARM64 FKI/PureDarwin profile (only when applicable):
Linux distro/image / kernel release / full .config or SHA-256 / page size / PAC / userspace type:
Guest RAM allocation / Linux mem= / FKI carve-out / hosted CPUs:
FKI configuration / carve-out allocation and reuse result:
XNU collection revision / digest and matching PureDarwin disk digest:
XNU stage and contiguous DRAM interval / configured carve-out comparison:
PureDarwin console command / guest-issued halt result:

x86_64 Tahoe/KVM profile (only when applicable):
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
[#36]: https://github.com/Personed/xnulinux/pull/36
