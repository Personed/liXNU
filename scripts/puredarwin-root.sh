#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# On Linux, archive the "Darwin ext4 Root" partition from a PureDarwin arm64 image.
#   puredarwin-root.sh IMAGE OUT.tar.gz
set -euo pipefail
private_mount_namespace=0
if [ "$#" -eq 3 ] && [ "${1:-}" = '--private-mount-namespace' ]; then
  private_mount_namespace=1
  shift
fi
if [ "$private_mount_namespace" -eq 1 ] && [ -n "${XNULINUX_PRIVATE_MOUNT_PATH:-}" ]; then
  PATH=$XNULINUX_PRIVATE_MOUNT_PATH
  export PATH
  unset XNULINUX_PRIVATE_MOUNT_PATH
fi
if [ "$private_mount_namespace" -eq 1 ] && [ "${XNULINUX_PRIVATE_MOUNT_TMPDIR+x}" = x ]; then
  if [ -n "$XNULINUX_PRIVATE_MOUNT_TMPDIR" ]; then
    TMPDIR=$XNULINUX_PRIVATE_MOUNT_TMPDIR
    export TMPDIR
  else
    unset TMPDIR
  fi
  unset XNULINUX_PRIVATE_MOUNT_TMPDIR
fi
case "$0" in
  */*)
    case "$0" in /*) script_path=$0 ;; *) script_path=$PWD/$0 ;; esac
    ;;
  *)
    if [ -f "$PWD/$0" ]; then
      script_path=$PWD/$0
    else
      script_path=$(command -v -- "$0" && printf '.')
      script_path=${script_path%.}
      script_path=${script_path%$'\n'}
    fi
    ;;
esac
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
output_name=${output##*/}
if [ -z "$output_name" ]; then
  echo 'Output path must name a file.' >&2
  exit 1
fi
output_dir=${output%/*}
if [ -z "$output_dir" ]; then output_dir=/; fi
if [ ! -d "$output_dir" ]; then
  echo "Output directory does not exist: $output_dir" >&2
  exit 1
fi
resolved_output_dir=''
if ! IFS= read -r -d '' resolved_output_dir < <(CDPATH= cd -P -- "$output_dir" && printf '%s\0' "$PWD"); then
  echo "Could not resolve output directory: $output_dir" >&2
  exit 1
fi
output_dir=$resolved_output_dir
if [ "$output_dir" = / ]; then output=/$output_name; else output=$output_dir/$output_name; fi
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
for tool in sfdisk python3 losetup mount mountpoint umount e2fsck tune2fs tar mktemp link stat chmod mkdir unshare setpriv id env; do
  if ! command -v "$tool" >/dev/null; then
    echo "PureDarwin root export requires $tool." >&2
    exit 1
  fi
done

# Isolate the temporary ext4 mount from the host mount table. The child keeps
# the original caller's permissions so output files on root-squashed NFS remain
# owned by that user; sudo is used only for loop and mount operations.
if [ "$private_mount_namespace" -eq 0 ]; then
  if [ "$EUID" -eq 0 ]; then
    exec unshare --mount --propagation private -- "$script_path" --private-mount-namespace "$image" "$output"
  fi
  caller_uid=$EUID
  caller_gid=$(id -g)
  exec sudo env XNULINUX_PRIVATE_MOUNT_PATH="$PATH" XNULINUX_PRIVATE_MOUNT_TMPDIR="${TMPDIR-}" \
    unshare --mount --propagation private -- setpriv \
    --reuid "$caller_uid" --regid "$caller_gid" --init-groups -- \
    "$script_path" --private-mount-namespace "$image" "$output"
fi

# Keep export temporary files out of paths another local account can change.
# Sticky shared directories such as /tmp remain safe for entries owned by this
# process; writable non-sticky parents and foreign-owned directories do not.
export_path_is_safe() {
  local path=$1 owner_mode owner mode mode_bits trusted_uid parent
  trusted_uid=$EUID
  if [ "$EUID" -eq 0 ] && [[ "${SUDO_UID:-}" =~ ^[0-9]+$ ]]; then
    trusted_uid=$((10#$SUDO_UID))
  fi
  while :; do
    owner_mode=$(stat -c '%u:%a' -- "$path" 2>/dev/null) || return 1
    owner=${owner_mode%%:*}
    mode=${owner_mode#*:}
    mode_bits=$((8#$mode))
    if (( (mode_bits & 0022) != 0 && (mode_bits & 01000) == 0 )); then
      return 1
    fi
    if [ "$owner" != "$EUID" ] && [ "$owner" != "$trusted_uid" ] && [ "$owner" != 0 ]; then
      return 1
    fi
    [ "$path" = / ] && return 0
    parent=${path%/*}
    if [ -z "$parent" ]; then path=/; else path=$parent; fi
  done
}
if ! export_path_is_safe "$output_dir"; then
  echo "Refusing an output directory that another local account can change: $output_dir" >&2
  exit 1
fi

# Resolve a possibly relative TMPDIR before entering the archive staging
# directory, matching the caller's original working directory.
mount_tmp_candidates=()
add_mount_tmp_candidate() {
  local candidate=$1 resolved=''
  if [ -d "$candidate" ] &&
     IFS= read -r -d '' resolved < <(CDPATH= cd -P -- "$candidate" && printf '%s\0' "$PWD"); then
    mount_tmp_candidates+=("$resolved")
  fi
}
if [ -n "${TMPDIR:-}" ]; then add_mount_tmp_candidate "$TMPDIR"; fi
add_mount_tmp_candidate /tmp

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

mount_dir='' mount_identity='' mount_parent='' mount_parent_identity=''
archive_dir='' archive_identity='' archive='' loop='' check_log=''
cleanup() {
  local cleanup_mount_identity cleanup_mount_owner_mode cleanup_parent_identity cleanup_parent_owner_mode
  local cleanup_archive_identity cleanup_archive_owner_mode
  trap '' INT TERM
  if [ -n "$mount_dir" ] && { [ -e "$mount_dir" ] || [ -L "$mount_dir" ]; }; then
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
  if [ -n "$archive_dir" ] && [ -n "$archive_identity" ]; then
    if [ "$(stat -c '%d:%i' -- . 2>/dev/null || true)" != "$archive_identity" ]; then
      cleanup_archive_identity=$(stat -c '%d:%i' -- "$archive_dir" 2>/dev/null || true)
      cleanup_archive_owner_mode=$(stat -c '%u:%a' -- "$archive_dir" 2>/dev/null || true)
      if [ ! -L "$archive_dir" ] &&
         [ "$cleanup_archive_identity" = "$archive_identity" ] &&
         { [ "$cleanup_archive_owner_mode" = "$EUID:700" ] ||
           [ "$cleanup_archive_owner_mode" = "$EUID:2700" ]; } &&
         export_path_is_safe "$output_dir"; then
        CDPATH= cd -P -- "$archive_dir" || return 1
      else
        echo "Staging path changed before cleanup; leaving it untouched: $archive_dir" >&2
        return 1
      fi
    fi
    if [ "$(stat -c '%d:%i' -- . 2>/dev/null || true)" = "$archive_identity" ]; then
      if [ -n "$mount_dir" ] && { [ -e "$mount_dir" ] || [ -L "$mount_dir" ]; }; then
        cleanup_mount_identity=$(stat -c '%d:%i' -- "$mount_dir" 2>/dev/null || true)
        cleanup_mount_owner_mode=$(stat -c '%u:%a' -- "$mount_dir" 2>/dev/null || true)
        if [ ! -L "$mount_dir" ] &&
           [ -n "$cleanup_mount_identity" ] &&
           { [ "$cleanup_mount_owner_mode" = "$EUID:700" ] ||
             [ "$cleanup_mount_owner_mode" = "$EUID:2700" ]; } &&
           { [ -z "$mount_identity" ] || [ "$cleanup_mount_identity" = "$mount_identity" ]; }; then
          rmdir -- "$mount_dir" || return 1
        else
          echo "Mount staging path changed during export; leaving it untouched: $mount_dir" >&2
          return 1
        fi
      fi
      mount_dir=''
      if [ -n "$mount_parent" ] && { [ -e "$mount_parent" ] || [ -L "$mount_parent" ]; }; then
        cleanup_parent_identity=$(stat -c '%d:%i' -- "$mount_parent" 2>/dev/null || true)
        cleanup_parent_owner_mode=$(stat -c '%u:%a' -- "$mount_parent" 2>/dev/null || true)
        if [ ! -L "$mount_parent" ] &&
           [ -n "$cleanup_parent_identity" ] &&
           { [ -z "$mount_parent_identity" ] || [ "$cleanup_parent_identity" = "$mount_parent_identity" ]; } &&
           { [ "$cleanup_parent_owner_mode" = "$EUID:700" ] ||
             [ "$cleanup_parent_owner_mode" = "$EUID:2700" ]; }; then
          rmdir -- "$mount_parent" || return 1
        else
          echo "Mount staging parent changed during export; leaving it untouched: $mount_parent" >&2
          return 1
        fi
        mount_parent=''
      fi
      if [ -n "$archive" ]; then rm -f -- "$archive" || return 1; archive=''; fi
      if [ -n "$check_log" ]; then rm -f -- "$check_log" || return 1; check_log=''; fi
      CDPATH= cd -P -- "$output_dir" || return 1
      if [ ! -L "$archive_dir" ] &&
         [ "$(stat -c '%d:%i' -- "$archive_dir" 2>/dev/null || true)" = "$archive_identity" ] &&
         [ "$(stat -c '%u' -- "$archive_dir" 2>/dev/null || true)" = "$EUID" ] &&
         { [ "$(stat -c '%a' -- "$archive_dir" 2>/dev/null || true)" = 700 ] ||
           [ "$(stat -c '%a' -- "$archive_dir" 2>/dev/null || true)" = 2700 ]; } &&
         export_path_is_safe "$output_dir"; then
        rmdir -- "$archive_dir" || return 1
      else
        echo "Staging path changed during export; leaving it untouched: $archive_dir" >&2
      fi
    else
      echo 'Exporter is no longer in its private staging directory; leaving temporary files for recovery.' >&2
      return 1
    fi
    archive_dir=''
  fi
}
trap cleanup EXIT
staging_signal=0
trap 'staging_signal=130' INT
trap 'staging_signal=143' TERM
archive_dir=$(mktemp -d -- "$output_dir/.puredarwin-root.XXXXXXXX")
if [ -n "$archive_dir" ]; then
  expected_archive_identity=$(stat -c '%d:%i' -- "$archive_dir")
  archive_identity=$expected_archive_identity
fi
trap 'exit 130' INT
trap 'exit 143' TERM
if [ "$staging_signal" -ne 0 ]; then exit "$staging_signal"; fi
if [ -L "$archive_dir" ] || ! export_path_is_safe "$archive_dir"; then
  echo "Refusing an unsafe private staging directory: $archive_dir" >&2
  exit 1
fi
CDPATH= cd -P -- "$archive_dir"
staging_identity=$(stat -c '%d:%i' -- .)
staging_owner=$(stat -c '%u' .)
staging_mode=$(stat -c '%a' .)
if [ "$staging_identity" != "$expected_archive_identity" ] ||
   [ "$(stat -c '%d:%i' -- "$archive_dir" 2>/dev/null || true)" != "$expected_archive_identity" ] ||
   [ -L "$archive_dir" ] || [ "$staging_owner" != "$EUID" ] ||
   { [ "$staging_mode" != 700 ] && [ "$staging_mode" != 2700 ]; }; then
  echo "Staging directory changed before it could be opened securely: $archive_dir" >&2
  exit 1
fi
chmod 0700 -- .
chmod g-s -- .
staging_identity=$(stat -c '%d:%i' -- .)
if [ "$staging_identity" != "$expected_archive_identity" ] ||
   [ "$(stat -c '%d:%i' -- "$archive_dir" 2>/dev/null || true)" != "$expected_archive_identity" ] ||
   [ "$(stat -c '%a' .)" != 700 ] || [ -L "$archive_dir" ]; then
  echo "Could not make the export staging directory private: $archive_dir" >&2
  exit 1
fi
archive=root.tar.gz
check_log=check.log

# The output staging directory may be on root-squashed NFS. Keep the mount
# point in a separate private temp directory that the privileged mount helper
# can access; the archive itself remains opened by this unprivileged shell.
create_mount_stage() {
  local candidate last_candidate='' owner_mode owner attempt
  local -a candidates=("${mount_tmp_candidates[@]}")
  for candidate in "${candidates[@]}"; do
    [ -n "$candidate" ] || continue
    [ "$candidate" != "$last_candidate" ] || continue
    last_candidate=$candidate
    [ -d "$candidate" ] || continue
    export_path_is_safe "$candidate" || continue
    mount_parent=''
    for attempt in {1..10}; do
      mount_parent=$candidate/.puredarwin-root-mount.$$.${RANDOM}${RANDOM}
      if mkdir -m 0700 -- "$mount_parent" 2>/dev/null; then
        break
      fi
      mount_parent=''
    done
    [ -n "$mount_parent" ] || continue
    if [ -L "$mount_parent" ] || ! export_path_is_safe "$mount_parent"; then
      rmdir -- "$mount_parent" 2>/dev/null || true
      mount_parent=''
      continue
    fi
    mount_parent_identity=$(stat -c '%d:%i' -- "$mount_parent")
    chmod 0700 -- "$mount_parent"
    chmod g-s -- "$mount_parent"
    owner_mode=$(stat -c '%u:%a' -- "$mount_parent")
    owner=${owner_mode%%:*}
    if [ "$owner" != "$EUID" ] || [ "${owner_mode#*:}" != 700 ] ||
       [ "$(stat -c '%d:%i' -- "$mount_parent")" != "$mount_parent_identity" ] ||
       [ -L "$mount_parent" ]; then
      rmdir -- "$mount_parent" 2>/dev/null || true
      mount_parent=''
      mount_parent_identity=''
      continue
    fi
    mount_dir=$mount_parent/mount
    mkdir -m 0700 -- "$mount_dir"
    mount_identity=$(stat -c '%d:%i' -- "$mount_dir")
    if [ "$(stat -c '%a' -- "$mount_dir")" != 700 ] || ! "${as_root[@]}" test -w "$mount_dir" 2>/dev/null; then
      rmdir -- "$mount_dir"
      rmdir -- "$mount_parent"
      mount_dir=''
      mount_identity=''
      mount_parent=''
      mount_parent_identity=''
      continue
    fi
    return 0
  done
  echo 'Could not create a private mount staging directory accessible to the mount helper. Set TMPDIR to a local filesystem, or use a local /tmp (root-squashed NFS cannot host the mount point).' >&2
  return 1
}
create_mount_stage
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
