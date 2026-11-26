#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# On Linux, archive the "Darwin ext4 Root" partition from a PureDarwin arm64 image.
#   puredarwin-root.sh IMAGE OUT.tar.gz
set -euo pipefail
private_mount_namespace=0
if [ "${1:-}" = '--private-mount-namespace' ]; then
  private_mount_namespace=1
  shift
fi
if [ "$#" -ne 2 ]; then
  echo "Usage: ${0##*/} IMAGE OUT.tar.gz" >&2
  exit 2
fi
image=$1 output=$2
if [ ! -f "$image" ]; then
  echo 'Expected an image file.' >&2
  exit 2
fi
case "$image" in
  /*) ;;
  *) image=$PWD/$image ;;
esac
case "$output" in
  /*) ;;
  *) output=$PWD/$output ;;
esac
output_dir=$(dirname -- "$output" && printf '.')
output_dir=${output_dir%.}
output_dir=${output_dir%$'\n'}
if [ ! -d "$output_dir" ]; then
  echo "Output directory does not exist: $output_dir" >&2
  exit 1
fi
output_dir=$(CDPATH= cd -P -- "$output_dir" && printf '%s.' "$PWD")
output_dir=${output_dir%.}
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
for tool in sfdisk python3 losetup mount mountpoint umount e2fsck tune2fs tar mktemp link stat chmod mkdir findmnt unshare setpriv id; do
  if ! command -v "$tool" >/dev/null; then
    echo "PureDarwin root export requires $tool." >&2
    exit 1
  fi
done

# Keep the privileged read-only mount private to this export process. Mounting
# directly in the host namespace would expose any world-readable guest files
# through /tmp while the ext4 filesystem is mounted.
if [ "$private_mount_namespace" -eq 0 ]; then
  if [ "$EUID" -eq 0 ]; then
    exec unshare --mount --propagation private -- "$0" --private-mount-namespace "$image" "$output"
  fi
  if ! command -v sudo >/dev/null; then
    echo 'Run as root or install sudo to create a private mount namespace.' >&2
    exit 1
  fi
  caller_uid=$EUID
  caller_gid=$(id -g)
  exec sudo unshare --mount --propagation private -- setpriv \
    --reuid "$caller_uid" --regid "$caller_gid" --init-groups -- \
    "$0" --private-mount-namespace "$image" "$output"
fi

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

mount_dir='' archive_dir='' archive_identity='' archive='' loop='' check_log=''
cleanup() {
  trap '' INT TERM
  if [ -n "$mount_dir" ] && [ -d "$mount_dir" ]; then
    if mountpoint -q -- "$mount_dir"; then
      if ! "${as_root[@]}" umount -- "$mount_dir"; then
        echo "Could not unmount $mount_dir; leaving the loop device and temporary archive for recovery." >&2
        return 1
      fi
    else
      mount_status=$?
      # util-linux mountpoint returns 32 when an existing path is not mounted.
      if [ "$mount_status" -ne 32 ]; then
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
  if [ -n "$mount_dir" ] && [ -d "$mount_dir" ]; then rmdir -- "$mount_dir" || return 1; fi
  mount_dir=''
  if [ -n "$archive" ]; then rm -f -- "$archive" || return 1; archive=''; fi
  if [ -n "$check_log" ]; then rm -f -- "$check_log" || return 1; check_log=''; fi
  if [ -n "$archive_dir" ]; then
    CDPATH= cd -P -- "$output_dir" || return 1
    staging_owner=$(stat -c '%u' -- "$archive_dir" 2>/dev/null || true)
    staging_mode=$(stat -c '%a' -- "$archive_dir" 2>/dev/null || true)
    staging_identity=$(stat -c '%d:%i' -- "$archive_dir" 2>/dev/null || true)
    if [ -d "$archive_dir" ] && [ ! -L "$archive_dir" ] &&
       [ "$staging_owner" = "$EUID" ] &&
       { [ "$staging_mode" = 700 ] || [ "$staging_mode" = 2700 ]; } &&
       { [ -z "$archive_identity" ] || [ "$staging_identity" = "$archive_identity" ]; }; then
      rmdir -- "$archive_dir" || return 1
    else
      echo "Staging path changed during export; leaving it untouched: $archive_dir" >&2
    fi
    archive_dir='' archive_identity=''
  fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if [ ! -d /tmp ] || [ ! -w /tmp ] || [ ! -k /tmp ]; then
  echo 'PureDarwin root export requires a writable sticky /tmp for its local mount point.' >&2
  exit 1
fi
tmp_fstype=$(findmnt --target /tmp --noheadings --output FSTYPE)
case "$tmp_fstype" in
  nfs|nfs4|cifs|smb3|sshfs|fuse.sshfs|davfs|9p)
    echo "PureDarwin root export requires /tmp on a local filesystem; found $tmp_fstype." >&2
    exit 1
    ;;
esac
mount_dir=''
for attempt in {1..10}; do
  mount_dir=/tmp/.puredarwin-root-mount.$$.${RANDOM}${RANDOM}
  if mkdir -m 0700 -- "$mount_dir" 2>/dev/null; then
    break
  fi
  mount_dir=''
done
if [ -z "$mount_dir" ]; then
  echo 'Could not create a unique mount point under /tmp.' >&2
  exit 1
fi
archive_dir=$(mktemp -d -- "$output_dir/.puredarwin-root.XXXXXXXX")
archive_identity=$(stat -c '%d:%i' -- "$archive_dir")
CDPATH= cd -P -- "$archive_dir"
staging_owner=$(stat -c '%u' .)
staging_mode=$(stat -c '%a' .)
staging_identity=$(stat -c '%d:%i' .)
if [ "$staging_owner" != "$EUID" ] || [ "$staging_identity" != "$archive_identity" ] ||
   { [ "$staging_mode" != 700 ] && [ "$staging_mode" != 2700 ]; }; then
  echo "Staging directory changed before it could be opened securely: $archive_dir" >&2
  exit 1
fi
chmod 0700 -- .
chmod g-s -- .
if [ "$(stat -c '%a' .)" != 700 ]; then
  echo "Could not make the export staging directory private: $archive_dir" >&2
  exit 1
fi
archive=root.tar.gz
check_log=check.log
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
