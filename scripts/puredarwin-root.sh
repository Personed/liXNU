#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# On Linux, archive the "Darwin ext4 Root" partition from a PureDarwin arm64 image.
#   puredarwin-root.sh IMAGE OUT.tar.gz
set -euo pipefail
if [ "$#" -ne 2 ]; then
  echo "Usage: ${0##*/} IMAGE OUT.tar.gz" >&2
  exit 2
fi
image=$1 output=$2
if [ ! -f "$image" ]; then
  echo 'Expected an image file.' >&2
  exit 2
fi
case "$output" in
  /*) ;;
  *) output=$PWD/$output ;;
esac
output_dir=$(dirname -- "$output")
if [ ! -d "$output_dir" ]; then
  echo "Output directory does not exist: $output_dir" >&2
  exit 1
fi
if [ -e "$output" ] || [ -L "$output" ]; then
  echo "Refusing to replace existing output path: $output" >&2
  exit 1
fi

as_root=()
if [ "$EUID" -ne 0 ]; then
  if ! command -v sudo >/dev/null; then
    echo 'Run as root or install sudo for loop and mount operations.' >&2
    exit 1
  fi
  as_root=(sudo)
fi
export PATH="$PATH:/usr/sbin:/sbin"
export LC_ALL=C
for tool in sfdisk python3 losetup mount mountpoint umount e2fsck tune2fs tar mktemp link; do
  if ! command -v "$tool" >/dev/null; then
    echo "PureDarwin root export requires $tool." >&2
    exit 1
  fi
done

partition_table=$(sfdisk --json -- "$image")
partition=$(python3 -c '
import json
import sys
from pathlib import Path

table = json.load(sys.stdin)["partitiontable"]
if table["label"] != "gpt":
    sys.exit("Expected a GPT PureDarwin image")
roots = [part for part in table.get("partitions", []) if part.get("name") == "Darwin ext4 Root"]
if len(roots) != 1:
    sys.exit("Expected one partition named Darwin ext4 Root")
sector_size = table["sectorsize"]
offset = roots[0]["start"] * sector_size
length = roots[0]["size"] * sector_size
if offset <= 0 or length <= 0 or offset + length > Path(sys.argv[1]).stat().st_size:
    sys.exit("PureDarwin root partition extends outside the image")
print(offset, length)
' "$image" <<< "$partition_table")
read -r offset length <<< "$partition"

mount_dir='' archive='' loop='' check_log=''
cleanup() {
  trap '' INT TERM
  if [ -n "$mount_dir" ]; then
    if mountpoint -q -- "$mount_dir"; then
      if ! "${as_root[@]}" umount -- "$mount_dir"; then
        echo "Could not unmount $mount_dir; leaving the loop device and temporary archive for recovery." >&2
        return 1
      fi
    else
      mount_status=$?
      if [ "$mount_status" -ne 1 ]; then
        echo "Could not determine whether $mount_dir is mounted; leaving temporary resources for recovery." >&2
        return 1
      fi
    fi
  fi
  if [ -n "$loop" ] && "${as_root[@]}" losetup "$loop" >/dev/null 2>&1; then
    if ! "${as_root[@]}" losetup --detach "$loop"; then
      echo "Could not detach $loop; leaving temporary files for recovery." >&2
      return 1
    fi
  fi
  if [ -n "$mount_dir" ]; then rmdir -- "$mount_dir" || return 1; mount_dir=''; fi
  if [ -n "$archive" ]; then rm -f -- "$archive" || return 1; archive=''; fi
  if [ -n "$check_log" ]; then rm -f -- "$check_log" || return 1; check_log=''; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mount_dir=$(mktemp -d)
archive=$(mktemp -- "$output_dir/.puredarwin-root.XXXXXXXX")
check_log=$(mktemp)
# Keep INT/TERM pending until losetup has returned the attached device. That
# lets the EXIT trap detach the exact loop device if a signal arrives here.
loop=$(trap '' INT TERM; "${as_root[@]}" losetup --find --show --read-only --offset "$offset" --sizelimit "$length" -- "$image")

filesystem_features=$("${as_root[@]}" tune2fs -l "$loop" | sed -n 's/^Filesystem features:[[:space:]]*//p')
if [ -z "$filesystem_features" ]; then
  echo 'Could not read ext4 filesystem features from the PureDarwin root partition.' >&2
  exit 1
fi
case " $filesystem_features " in
  *' needs_recovery '*)
    echo 'Refusing to export an ext4 filesystem that needs journal recovery.' >&2
    exit 1
    ;;
esac
if ! "${as_root[@]}" e2fsck -fn "$loop" >"$check_log" 2>&1; then
  cat "$check_log" >&2
  echo 'Refusing to export an ext4 filesystem that is not clean.' >&2
  exit 1
fi
# A plain read-only ext4 mount can replay the journal. Verify it is clean first,
# then mount with noload so the source image remains unchanged.
"${as_root[@]}" mount -t ext4 -o ro,noload,nodev,nosuid,noexec -- "$loop" "$mount_dir"
"${as_root[@]}" tar -C "$mount_dir" --numeric-owner -czf - . > "$archive"
# Finish filesystem cleanup before publishing a successful archive.
"${as_root[@]}" umount -- "$mount_dir"
"${as_root[@]}" losetup --detach "$loop"
loop=''
chmod 0644 -- "$archive"
if [ -e "$output" ] || [ -L "$output" ]; then
  echo "Refusing to replace output path created during export: $output" >&2
  exit 1
fi
if ! link "$archive" "$output"; then
  echo "Cannot publish $output; it may have appeared during export or its filesystem may not support hard links." >&2
  exit 1
fi
rm -- "$archive"
archive=''
printf '%s\n' "$output"
