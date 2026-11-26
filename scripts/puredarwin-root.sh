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
if [ ! -f "$image" ] || [ -d "$output" ] || [ "$image" -ef "$output" ]; then
  echo 'Expected an image file and a separate output archive path.' >&2
  exit 2
fi

as_root=()
if [ "$EUID" -ne 0 ]; then as_root=(sudo); fi
export PATH="$PATH:/usr/sbin:/sbin"
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

mount_dir= archive= loop= mounted=
cleanup() {
  if [ -n "$mounted" ]; then "${as_root[@]}" umount -- "$mount_dir" || return; fi
  if [ -n "$loop" ]; then "${as_root[@]}" losetup --detach "$loop" || return; fi
  if [ -n "$mount_dir" ]; then rmdir -- "$mount_dir"; fi
  if [ -n "$archive" ]; then rm -f -- "$archive"; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mount_dir=$(mktemp -d)
archive=$(mktemp -- "$(dirname -- "$output")/.puredarwin-root.XXXXXXXX")
loop=$("${as_root[@]}" losetup --read-only --find --show --offset "$offset" --sizelimit "$length" -- "$image")
# A plain read-only ext4 mount can replay the journal. Keep the source image untouched.
"${as_root[@]}" mount -t ext4 -o ro,noload,nodev,nosuid,noexec -- "$loop" "$mount_dir"
mounted=1
"${as_root[@]}" tar -C "$mount_dir" --numeric-owner -czf - . > "$archive"
"${as_root[@]}" umount -- "$mount_dir"
mounted=
"${as_root[@]}" losetup --detach "$loop"
loop=
mv -fT -- "$archive" "$output"
archive=
printf '%s\n' "$output"
