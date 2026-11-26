<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Portability validation

Record the **host, runtime, and furthest verified stage** for each result. Booting the
ARM64 Linux VM does not mean that XNU ran. Building or probing a module does not mean
Darwin userspace ran. A PureDarwin shell does not establish macOS compatibility.

| Stage | What counts as evidence |
| --- | --- |
| Firmware | The intended architecture reaches the firmware console (e.g. UEFI shell for the QEMU profile). |
| Linux host | An ARM64 Linux host or guest reaches userspace; report its kernel, page size, and PAC availability. |
| FKI/XNU module probe | FKI loads and exposes its host-call interface; XNU resolves its FKI dependencies and reaches the expected firmware request. A missing collection ends the probe here. |
| PureDarwin userspace | The real, paired PureDarwin disk and free kernel collection boot; execute a command on the Darwin console and observe the result. Record a clean halt separately. |
| macOS desktop | A user-provided compatible macOS disk and collection reach an interactive desktop; record input, rendering backend, and networking separately. |

Report the profile alongside the stage:

- **Native ARM64 Linux:** stock Linux with 4 KiB pages, PAC, and the required
  exception level. A result on one board/CPU or kernel does not validate all ARM64 devices.
- **ARM64 Linux guest hosted on another OS:** record the outer machine and OS separately
  from the Linux guest. The ARM64 QEMU path in [#3] uses TCG, including on Intel/AMD hosts;
  distinguish it from the Apple Silicon krunkit path. The module smoke test in [#1]
  is a separate check. Neither substitutes for the PureDarwin test below.
- **x86-64 guest on Intel/AMD Linux:** [#16] describes a distinct experimental QEMU/KVM
  path, not a port of the ARM64 XNU/FKI modules. Report KVM and TCG separately; a Linux
  test guest is not evidence of an x86-64 Darwin or macOS boot. The PureDarwin run below
  applies only to the ARM64 FKI path.

## First end-to-end check for a new FKI host profile

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
Outer machine + CPU / OS + version:
Runtime + version / accelerator:
ARM64 Linux distro / kernel / page size / PAC (if applicable):
Memory / FKI carve-out / hosted CPUs:
Guest OS + version / collection and pristine disk digests:
Stages reached / guest halt result:
Rendering / input / networking (tested, failed, or not tested):
CI run or sanitized logs:
```

Link the report from the relevant PR. Attach logs rather than disk images or Apple
artifacts. Remove machine identifiers, credentials and local paths before sharing.

[#1]: https://github.com/Personed/xnulinux/pull/1
[#3]: https://github.com/Personed/xnulinux/pull/3
[#4]: https://github.com/Personed/xnulinux/pull/4
[#5]: https://github.com/Personed/xnulinux/pull/5
[#7]: https://github.com/Personed/xnulinux/pull/7
[#16]: https://github.com/Personed/xnulinux/pull/16
