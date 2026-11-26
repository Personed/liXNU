#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# On Linux or macOS: PureDarwin's root file system (scripts/puredarwin-root.sh) as the hosted system's disk, a GUID partition
# table with one HFS+ volume (hfs.kext, Apple's or the free collection's, mounts it), and the ROOT_UUID xnu.ko boots it by.
# The kernel mounts the root read-only; PureDarwin's own images use ext4, whose kext clears that, and its mount(8) has no
# -uw for launchctl's remount, so /sbin/fsck (which launchctl runs first, on a read-only root) remounts it read-write as
# PureDarwin's HFS images do. The disk is checked on the Linux side before each boot (fsck.hfsplus), not here.
#   mk-puredarwin-disk.sh ROOT.tar.gz OUT.img [SIZE (1g)]   prints the disk's configuration for xnulinux-boot.sh (MAC_CONF):
#   ROOT_UUID and BOOT_UUID, the partition's GUID
set -euo pipefail
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "usage: ${0##*/} ROOT.tar.gz OUT.img [SIZE]" >&2
  exit 2
fi
ROOT=$1 OUT=$2 SIZE=${3:-1g}
as_root=()
if [ "$EUID" -ne 0 ]; then as_root=(sudo); fi

populate_root() {
  "${as_root[@]}" tar --numeric-owner -xzf "$ROOT" -C "$M"
  "${as_root[@]}" rm -rf "$M/.fseventsd" "$M/.Spotlight-V100"
  "${as_root[@]}" rm -f "$M/sbin/fsck"
  "${as_root[@]}" tee "$M/sbin/fsck" >/dev/null <<'FSCK'
#!/bin/zsh
# xnulinux: the disk was checked on the Linux host before boot (fsck.hfsplus); remount the HFS+ root read-write
dev=$(/bin/df / | /bin/awk 'NR==2 {print $1}')
/sbin/mount -t hfs -o update,rw "$dev" / && echo "xnulinux: root $dev remounted read-write"
exit 0
FSCK
  "${as_root[@]}" chown 0:0 "$M/sbin/fsck"
  "${as_root[@]}" chmod 755 "$M/sbin/fsck"
}

make_linux_disk() {
  local tool
  for tool in truncate sfdisk losetup mkfs.hfsplus mount umount "${as_root[@]}"; do
    if ! command -v "$tool" >/dev/null; then
      echo "PureDarwin disk creation on Linux requires $tool." >&2
      return 1
    fi
  done

  # Keep the output file intact if formatting, extraction or unmounting fails.
  D='' M='' loop='' mounted=''
  trap cleanup_linux_disk EXIT
  D=$(mktemp "$(dirname -- "$OUT")/.puredarwin-disk.XXXXXX")
  M=$(mktemp -d)
  truncate -s "$SIZE" -- "$D"
  sfdisk --quiet "$D" >/dev/null <<'GPT'
label: gpt

type=48465300-0000-11AA-AA11-00306543ECAC, name="PureDarwin"
GPT
  uuid=$(sfdisk --part-uuid "$D" 1 | tr '[:lower:]' '[:upper:]')
  loop=$("${as_root[@]}" losetup --find --show --partscan -- "$D")
  # Linux HFS+ supports writable mounts of non-journaled volumes.
  "${as_root[@]}" mkfs.hfsplus -v PureDarwin "${loop}p1" >&2
  "${as_root[@]}" mount -t hfsplus "${loop}p1" "$M"
  mounted=1
  populate_root
  "${as_root[@]}" umount "$M"
  mounted=
  "${as_root[@]}" losetup -d "$loop"
  loop=
  mv -T -- "$D" "$OUT"
}

cleanup_linux_disk() {
  if [ -n "$mounted" ]; then "${as_root[@]}" umount "$M" || return; fi
  if [ -n "$loop" ]; then "${as_root[@]}" losetup -d "$loop" || return; fi
  if [ -n "$M" ]; then rmdir -- "$M"; fi
  if [ -n "$D" ]; then rm -f -- "$D"; fi
}

make_macos_disk() {
  M=$(mktemp -d); D="$OUT.dmg"
  rm -f "$D"
  hdiutil create -quiet -size "$SIZE" -fs "Journaled HFS+" -volname PureDarwin -layout GPTSPUD "$D"
  hdiutil attach -quiet -owners on -nobrowse -mountpoint "$M" "$D"
  trap 'hdiutil detach -quiet "$M" 2>/dev/null || true' EXIT
  populate_root
  local plist
  plist=$(mktemp)
  diskutil info -plist "$M" > "$plist"
  uuid=$(plutil -extract DiskUUID raw "$plist")
  rm -f "$plist"
  sync; hdiutil detach -quiet "$M"; trap - EXIT; rmdir "$M"
  mv "$D" "$OUT"
}

case "$(uname -s)" in
  Linux) make_linux_disk ;;
  Darwin) make_macos_disk ;;
  *) echo "PureDarwin disk creation requires Linux or macOS." >&2; exit 1 ;;
esac
printf 'ROOT_UUID=%s\nBOOT_UUID=%s\n' "$uuid" "$uuid"
