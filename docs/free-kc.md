<!-- SPDX-License-Identifier: GPL-2.0-only -->
# A kernel collection without Apple's kexts, and PureDarwin's userland

The kernel collection `scripts/vm-xnu-rebuild.sh` links is the source-built XNU and xnulinux's own kexts, plus Apple's
kexts from a macOS kernelcache (APFS, AMFI, IOKit families...). `scripts/free-kc/build.sh` links one with none of
Apple's kexts. That collection boots [PureDarwin](https://github.com/PureDarwin/PureDarwin)'s arm64 userland (its own
dyld, libSystem, launchd, zsh and toybox) to its console shell. xnulinux takes PureDarwin from its fork
[AFK-surf/PureDarwin](https://github.com/AFK-surf/PureDarwin), which has what this kernel needs of the userland:
`posix_spawnattr_init` zeroes the attributes (otherwise launchd's children come up with the resource limits `malloc` left
in them, a file descriptor hard limit of a few, and are killed when they pass it), and ICU's libraries are signed after
their install names change (otherwise their first page fails validation in every process that maps them).

## What is in it

| Kext | From | Built as |
|---|---|---|
| XNU, `XnuCPU`, `XnuDisk`, `XnuDrm` | this repository | as for macOS (`vm-xnu-rebuild.sh`) |
| `com.apple.kec.corecrypto` | xnulinux (`kexts/free/corecrypto`: Mbed TLS behind the corecrypto interface, `docs/corecrypto.md`) | |
| `com.apple.kec.amfi` | PureDarwin's permissive AMFI stub | |
| `com.apple.kec.pthread` | PureDarwin (Apple's libpthread kext) | |
| `com.apple.iokit.IOStorageFamily`, `HFSEncodings`, `hfs` | PureDarwin (Apple's open-source projects) | |

Every kext is compiled from source as arm64e with this kernel's own compile flags (`scripts/free-kc/kcc.py`), and
linked into the collection by `scripts/kclink` the way xnulinux's own kexts are.

Nothing of Apple's is read to make it. `kclink` links kexts into an existing collection, so `scripts/free-kc/mk-template.py`
first makes an empty one: an `MH_FILESET` with the kernel's segments at the addresses in `scripts/free-kc/layout.json`
(region sizes, nothing else), the kernel's pseudo-extensions (`com.apple.kpi.*`, from XNU's own
`config/System.kext/PlugIns/*/Info.plist`) as the prelink info, and empty chained fixups. Built with every
kernelcache and kernel collection of Apple's hidden from the build (a mount namespace with them replaced by empty files),
the result is the same byte for byte. It also boots with no static trust cache (`TRUSTCACHE=`).

PureDarwin's prebuilt kexts cannot be used. They are arm64 and this kernel is arm64e, so calls from the kernel through
their C++ vtables and function pointers would fail pointer authentication.

`kexts/free/compat/` fills the few gaps between those sources and this kernel: `OSMalloc`, which this kernel no longer
has; the kernel-internal `kalloc_type_impl` that pthread and AMFI reach for; `devfs_is_ready`, which only PureDarwin's
kernel has; and a kext's load tag.

## Using it

1. `scripts/puredarwin-root.sh IMAGE root.tar.gz` archives the PureDarwin arm64 root on Linux, including x86-64.
   It requires `fdisk` (`sfdisk`), `util-linux` (loop devices and mount tools), `e2fsprogs` (`tune2fs` and `e2fsck`),
   `python3`, `tar`, `coreutils` (`link`), and kernel support for ext4 and loop devices. Run as root or with `sudo`.
   The input image's ext4
   root must be clean and have no pending journal recovery; the exporter refuses filesystems that need repair. It opens
   the source image read-only, mounts with journal replay disabled, and preserves numeric owners in the archive. The
   output path must be new, its parent directory must exist, and that filesystem must support hard links. The archive
   is published without replacing an existing file or symlink and has mode `0644`. Build the input image with
   `nix build 'git+https://github.com/AFK-surf/PureDarwin?submodules=1&rev=b3b4b2812fdd1c5c251dc3089b5a1a9e00d29970#image-arm64-virt-minimal'` (PureDarwin's
   binary cache, https://hydra.lab004.dev, has its toolchain and what the fork did not change; the images there are
   upstream's).
   The [ARM64 image workflow](../.github/workflows/puredarwin-arm64-image.yml) builds this pinned AFK revision
   on an x86-64 runner, verifies the release SHA-256 and retains the compressed image artifact for three days. This
   records a pinned build input and checks artifact integrity; it does not establish byte-for-byte reproducibility
   between independent builds because image UUID/GUID values can vary. The full-run job below pairs it with the source-
   built collection and records the boot result.
2. Run the following on Linux or macOS to make the HFS+ disk and save its UUIDs after a successful build.

   ```sh
   scripts/mk-puredarwin-disk.sh root.tar.gz pd.img > pd.conf.new && mv pd.conf.new pd.conf
   ```

   Use a new output filename; the script refuses to replace an existing file, symlink, device or other destination. It
   publishes the completed image with the POSIX `link` utility, so that utility and hard links must be supported by the
   output directory's filesystem. On Linux, install `hfsprogs` and `sfdisk` (the `fdisk` package on Debian and Ubuntu). The Linux kernel needs loop devices and
   writable HFS+ support. The script uses root privileges for those operations and writes a non-journaled volume. After
   an unclean shutdown, check the HFS+ partition on Linux before booting again.

   ```sh
   (
     set -e
     if systemctl cat xnulinux.service >/dev/null 2>&1; then
       sudo systemctl stop xnulinux.service
     fi
     scripts/xnulinux-boot.sh stop
     loop=$(sudo losetup --find --show --partscan pd.img)
     trap 'sudo losetup --detach "$loop"' EXIT
     if ! sudo fsck.hfsplus -f "${loop}p1"; then
       echo "HFS+ check failed; do not boot this disk until it is repaired." >&2
       exit 1
     fi
     sudo losetup --detach "$loop"
     trap - EXIT
   )
   ```

   Stop the hosted system and any `xnulinux.service` supervisor so it cannot restart during the check, and ensure the
   partition is unmounted before checking it. The subshell keeps `set -e` and a failed check from changing the terminal
   session. The image uses GPT, so run
   `fsck.hfsplus` on the first partition exposed by `losetup --partscan`, not on `pd.img`. If the check fails, repair the
   volume before booting it again.
3. In the VM, after `scripts/vm-xnu-rebuild.sh`: `scripts/free-kc/build.sh`, which writes `/lib/firmware/xnu/kc-free.bin`.
   `scripts/vm-xnu-build.sh` checks out DroidVM at pinned revision
   `524d27bf57ae92375aa39e3a81e11727c08973ff`; set `DVM_REV` explicitly only when intentionally building against a
   different DroidVM commit, and record that commit with the output.
4. Boot it: `MAC_CONF=pd.conf DISK=pd.img IMAGE=xnu/kc-free.bin TRUSTCACHE= GPU=off scripts/xnulinux-boot.sh`. The console
   (`CONSOLE_IN`) is PureDarwin's zsh. Its `halt` and `reboot` end the run. For a service-managed boot, use
   `sudo systemctl start xnulinux.service` instead of the manual command, after configuring the service for this disk;
   this restores its supervision and automatic restart after a disk check.

The [PureDarwin ARM64 image workflow](../.github/workflows/puredarwin-arm64-image.yml) also builds the HFS+ disk on Linux,
builds XNU and `kc-free.bin`, then boots the real PureDarwin console in an ARM64 Linux guest under x86-64 QEMU TCG. The
CI summary records the exact guest profile and artifact digests; the serial log is retained for 30 days. This check
covers the console and a guest-requested clean halt, not a desktop, graphics, networking, or performance claim.

## Apple's kexts, linked on the user's machine

What can be distributed is the kernel, xnulinux's own kexts and `kc-free.bin`. A user who wants to boot macOS links
Apple's kexts from their own copy of macOS into a collection of their own, and chooses which collection to boot with
`IMAGE=` (`image=` of `xnu.ko`):

1. In the VM, after `scripts/vm-xnu-rebuild.sh`: `scripts/apple-kc/package.sh KIT` writes the kit: the kernel,
   xnulinux's kexts (objects and descriptions), `kc-free.bin`, and `kit.json` (the kernelcache it links, by sha256; the
   Apple kexts to keep, `scripts/apple-kc/keep.txt`; the pointers to rebind, `scripts/apple-kc/rebind.txt`; AMFI's
   platform version). Nothing of Apple's is in it.
2. On the machine that runs `xnu.ko` (python3, and `lzfse` for Apple's LZFSE: `apt install lzfse`):
   `scripts/apple-kc/link.sh KIT`. It reads macOS 27.2's `kernelcache.release.vma2` straight from Apple's IPSW (its URL
   is in `kit.json`; only that member is downloaded, with HTTP range requests), links the collection with
   `scripts/kclink` and installs it as `/lib/firmware/xnu/kc-apple.bin`. A second argument gives the
   kernelcache instead: an IPSW or its URL, the IM4P, or that unpacked (`scripts/apple-kc/im4p.py`). The collection is
   the one `vm-xnu-rebuild.sh` links from the same kernelcache, byte for byte. Another kernelcache is refused: the
   kernel's segment slots were laid out for that one.
3. `IMAGE=xnu/kc-apple.bin scripts/xnulinux-boot.sh` boots macOS on it; `IMAGE=xnu/kc-free.bin` boots PureDarwin.

An auxiliary collection (Apple's kexts as a layer over `kc-free.bin`, chosen when `xnu.ko` loads) is not used: on arm64
XNU takes one only from the booter, and adds its kexts only when macOS's `kernelmanagerd` asks, after the root (APFS)
is mounted, so APFS could not be in it.

## The linker

`scripts/kclink/kclink.py` links every collection here (`vm-xnu-rebuild.sh`, `free-kc/build.sh`, `apple-kc/link.sh`).
It is written from the formats alone (`<mach-o/loader.h>`, `<mach-o/fixup-chains.h>`, the arm64 relocations, the
prelink info XNU's `OSKext` reads):

- It takes a collection to lay the output out like (macOS's kernelcache, or `mk-template.py`'s empty one) and the
  kernel, linked at the addresses of its slots. The kernel's threaded rebases (`__TEXT,__thread_starts`) become the
  collection's chained fixups (`DYLD_CHAINED_PTR_64_KERNEL_CACHE`).
- `--keep` keeps kexts of that collection, with every kext they depend on, where they are and byte for byte. Their
  pointers into the old kernel go to the same symbols in this one (the old kernel's symbol table names them); `--rebind`
  sends one to another symbol.
- `--native` links kexts from arm64e objects (`scripts/kclink/kextld.py`: their sections laid out into a kext's
  segments, a GOT, `kmod_info`, the relocations) into space no kernel or kept kext uses. A C++ class whose superclass
  is in a kept kext (the spec's `vtable`) gets the superclass's vtable slots for methods no kext exports, and every slot
  it does not override is checked against the superclass's.

`scripts/kclink/kccompare.py` compares two collections (pointers decoded, each kext's bytes, the prelink info).

## Not done yet

- The kernel headers the kexts build against come from this kernel's `installhdrs`. PureDarwin's AMFI stub also needs
  PureDarwin's CoreEntitlements and TrustCache headers.
- Tested in the krunkit VM with 1 and 2 hosted CPUs: console shell, `launchctl list`, writes to the root, `halt`.
