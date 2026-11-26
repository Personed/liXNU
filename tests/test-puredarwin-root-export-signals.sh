#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

script=$(cd -- "$(dirname -- "$0")/.." && pwd)/scripts/puredarwin-root.sh
real_mktemp=$(command -v mktemp)
real_chmod=$(command -v chmod)
real_stat=$(command -v stat)
real_mkdir=$(command -v mkdir)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
bin=$tmp/bin
state=$tmp/state
shared_output=$tmp/shared-output
alternate_tmp=$tmp/alternate-tmp
mkdir -- "$bin" "$state" "$shared_output" "$alternate_tmp"
chmod 2777 -- "$shared_output"
export TEST_STATE=$state
export FAKE_LOOP=/dev/loop-codex-test
export REAL_MKTEMP=$real_mktemp
export REAL_CHMOD=$real_chmod
export REAL_STAT=$real_stat
export REAL_MKDIR=$real_mkdir
export TMPDIR=$alternate_tmp
export PATH="$bin:$PATH"

cat >"$bin/sfdisk" <<'WRAPPER'
#!/bin/sh
printf '%s\n' '{"partitiontable":{"label":"gpt","sectorsize":512,"partitions":[{"start":2048,"size":2048,"name":"Darwin ext4 Root"}]}}'
WRAPPER
cat >"$bin/findmnt" <<'WRAPPER'
#!/bin/sh
printf '%s\n' "${TEST_TMP_FSTYPE:-tmpfs}"
WRAPPER
cat >"$bin/sudo" <<'WRAPPER'
#!/bin/sh
exec "$@"
WRAPPER
cat >"$bin/unshare" <<'WRAPPER'
#!/bin/sh
while [ "$#" -gt 0 ] && [ "$1" != '--' ]; do shift; done
[ "$#" -gt 0 ] || exit 2
shift
if [ "${1:-}" = setpriv ]; then
  shift
  while [ "$#" -gt 0 ] && [ "$1" != '--' ]; do
    case "$1" in
      --reuid|--regid) shift 2 ;;
      --init-groups) shift ;;
      *) echo "Unexpected setpriv argument: $1" >&2; exit 2 ;;
    esac
  done
  [ "$#" -gt 0 ] || exit 2
  shift
fi
exec "$@"
WRAPPER
cat >"$bin/mkdir" <<'WRAPPER'
#!/bin/sh
target=''
for arg in "$@"; do target=$arg; done
case "$target" in
  /tmp/.puredarwin-root-mount.*)
    "$REAL_MKDIR" "$@" || exit
    printf '%s\n' "$target" >"$TEST_STATE/mount-dir"
    if [ "${SIGNAL_DURING_MOUNTPOINT_MKDIR:-0}" = 1 ]; then kill -TERM "$PPID"; fi
    ;;
  *) exec "$REAL_MKDIR" "$@" ;;
esac
WRAPPER
cat >"$bin/losetup" <<'WRAPPER'
#!/bin/sh
case "$1" in
  --find)
    : >"$TEST_STATE/loop-attached"
    printf '%s\n' "$FAKE_LOOP"
    printf '%s\n' attached >>"$TEST_STATE/events"
    ;;
  --detach)
    rm -f -- "$TEST_STATE/loop-attached"
    printf '%s\n' detached >>"$TEST_STATE/events"
    ;;
  "$FAKE_LOOP")
    [ -e "$TEST_STATE/loop-attached" ]
    ;;
  *)
    echo "Unexpected losetup arguments: $*" >&2
    exit 1
    ;;
esac
WRAPPER
cat >"$bin/mountpoint" <<'WRAPPER'
#!/bin/sh
case "$3" in /*) mount_dir=$3 ;; *) mount_dir=$PWD/$3 ;; esac
printf '%s\n' "$mount_dir" >"$TEST_STATE/mount-dir"
if [ -e "$TEST_STATE/mounted" ]; then exit 0; fi
if [ -d "$3" ]; then exit 32; fi
exit 1
WRAPPER
cat >"$bin/mount" <<'WRAPPER'
#!/bin/sh
: >"$TEST_STATE/mounted"
printf '%s\n' "$7" >"$TEST_STATE/mount-dir"
printf '%s\n' mounted >>"$TEST_STATE/events"
kill -TERM "$PPID"
WRAPPER
cat >"$bin/umount" <<'WRAPPER'
#!/bin/sh
rm -f -- "$TEST_STATE/mounted"
printf '%s\n' unmounted >>"$TEST_STATE/events"
WRAPPER
cat >"$bin/tune2fs" <<'WRAPPER'
#!/bin/sh
printf '%s\n' 'Filesystem features: has_journal ext_attr'
WRAPPER
cat >"$bin/e2fsck" <<'WRAPPER'
#!/bin/sh
printf '%s\n' "$PWD/check.log" >"$TEST_STATE/check-log"
if [ "${SIGNAL_BEFORE_MOUNT:-0}" = 1 ]; then kill -TERM "$PPID"; fi
if [ "${REPLACE_DURING_E2FSCK:-0}" = 1 ]; then
  staging=$(cat "$TEST_STATE/archive-dir")
  mv -- "$staging" "$staging.original"
  "$REAL_MKDIR" -m 0700 -- "$staging"
  "$REAL_CHMOD" 0700 -- "$staging"
  "$REAL_CHMOD" g-s -- "$staging"
  printf '%s\n' "$staging" >"$TEST_STATE/cleanup-replacement"
  exit 1
fi
exit 0
WRAPPER
cat >"$bin/mktemp" <<'WRAPPER'
#!/bin/sh
result=$("$REAL_MKTEMP" "$@") || exit
if [ "$1" = '-d' ]; then
  case "$*" in
    *'.puredarwin-root.'*)
      printf '%s\n' "$result" >"$TEST_STATE/archive-dir"
      if [ "${REPLACE_ARCHIVE_DIR:-0}" = 1 ]; then
        mv -- "$result" "$result.original"
        ln -s -- "$ATTACKER_DIR" "$result"
        printf '%s\n' "$result.original" >"$TEST_STATE/replaced-stage"
      fi
      ;;
  esac
fi
printf '%s\n' "$result"
WRAPPER
cat >"$bin/stat" <<'WRAPPER'
#!/bin/sh
result=$("$REAL_STAT" "$@") || exit
if [ "${REPLACE_AFTER_IDENTITY:-0}" = 1 ] &&
   [ "$#" -eq 4 ] && [ "$1" = '-c' ] && [ "$2" = '%d:%i' ] && [ "$3" = '--' ] &&
   [ "$4" = "$(cat "$TEST_STATE/archive-dir")" ] &&
   [ ! -e "$TEST_STATE/identity-replaced" ]; then
  mv -- "$4" "$4.original"
  "$REAL_MKDIR" -m 0700 -- "$4"
  "$REAL_CHMOD" 0700 -- "$4"
  "$REAL_CHMOD" g-s -- "$4"
  printf '%s\n' "$4.original" >"$TEST_STATE/replaced-stage"
  : >"$TEST_STATE/identity-replaced"
fi
printf '%s\n' "$result"
WRAPPER
chmod +x "$bin"/*
cat >"$bin/chmod" <<'WRAPPER'
#!/bin/sh
if [ "${SIGNAL_BEFORE_STAGING_CHMOD:-0}" = 1 ] && [ "${1:-}" = 0700 ]; then
  kill -TERM "$PPID"
  exit 0
fi
exec "$REAL_CHMOD" "$@"
WRAPPER
chmod +x "$bin/chmod"

image=$tmp/fake.img
truncate -s 4194304 "$image"
output=$shared_output/must-not-exist.tar.gz
set +e
TEST_TMP_FSTYPE=nfs4 "$script" "$image" "$output" >"$tmp/nfs-tmp.log" 2>&1
status=$?
set -e
[ "$status" -ne 0 ] || { echo 'Exporter accepted NFS /tmp for its privileged mountpoint' >&2; exit 1; }
grep -q 'requires /tmp on a local filesystem' "$tmp/nfs-tmp.log"
[ ! -e "$state/mount-dir" ]

set +e
"$script" "$image" "$output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq 143 ] || { echo "Expected SIGTERM exit 143; got $status" >&2; exit 1; }
[ ! -e "$output" ]
[ ! -e "$state/mounted" ]
[ ! -e "$state/loop-attached" ]
[ "$(grep -c '^unmounted$' "$state/events")" -eq 1 ]
[ "$(grep -c '^detached$' "$state/events")" -eq 1 ]
unmount_line=$(grep -n '^unmounted$' "$state/events" | cut -d: -f1)
detach_line=$(grep -n '^detached$' "$state/events" | cut -d: -f1)
[ "$unmount_line" -lt "$detach_line" ]
mount_dir=$(cat "$state/mount-dir")
[ "${mount_dir#/tmp/.puredarwin-root-mount.}" != "$mount_dir" ]
[ ! -e "$mount_dir" ]
[ -z "$(find "$shared_output" -mindepth 1 -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]
first_check_log=$(cat "$state/check-log")
[ -n "$first_check_log" ]
[ ! -e "$first_check_log" ]

echo 'PureDarwin root exporter signal cleanup test passed.'

# Also interrupt after loop setup and filesystem validation but before mount.
# util-linux mountpoint returns 32 for the existing, unmounted directory;
# cleanup must accept that state and still detach the loop and remove temps.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/mount-dir"
rm -f -- "$state/check-log"
export SIGNAL_BEFORE_MOUNT=1
set +e
"$script" "$image" "$output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq 143 ] || { echo "Expected pre-mount SIGTERM exit 143; got $status" >&2; exit 1; }
[ ! -e "$output" ]
[ ! -e "$state/mounted" ]
[ ! -e "$state/loop-attached" ]
[ "$(grep -c '^detached$' "$state/events")" -eq 1 ]
if grep -q '^unmounted$' "$state/events"; then
  echo 'pre-mount cleanup attempted to unmount an unmounted directory' >&2
  exit 1
fi
mount_dir=$(cat "$state/mount-dir")
[ "${mount_dir#/tmp/.puredarwin-root-mount.}" != "$mount_dir" ]
[ ! -e "$mount_dir" ]
[ -z "$(find "$shared_output" -mindepth 1 -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]
second_check_log=$(cat "$state/check-log")
[ -n "$second_check_log" ]
[ "$second_check_log" != "$first_check_log" ]
[ ! -e "$second_check_log" ]

echo 'PureDarwin root exporter pre-mount signal cleanup test passed.'

# Interrupt while the mountpoint directory is being created, before mkdir
# returns. The path is already known to the shell, so cleanup must remove it.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/mount-dir" "$state/check-log"
set +e
SIGNAL_DURING_MOUNTPOINT_MKDIR=1 "$script" "$image" "$output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq 143 ] || { echo "Expected mountpoint-creation SIGTERM exit 143; got $status" >&2; exit 1; }
mount_dir=$(cat "$state/mount-dir")
[ "${mount_dir#/tmp/.puredarwin-root-mount.}" != "$mount_dir" ]
[ ! -e "$mount_dir" ]
[ ! -e "$state/loop-attached" ]
[ ! -e "$state/check-log" ]
[ ! -e "$output" ]

echo 'PureDarwin root exporter mountpoint creation signal cleanup test passed.'

# A signal after mktemp but before clearing setgid must still remove the empty
# 2700 staging directory using its recorded inode identity.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/mount-dir" "$state/check-log"
unset SIGNAL_BEFORE_MOUNT
set +e
SIGNAL_BEFORE_STAGING_CHMOD=1 "$script" "$image" "$output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq 143 ] || { echo "Expected early SIGTERM exit 143; got $status" >&2; exit 1; }
staging_dir=$(cat "$state/archive-dir")
[ ! -e "$staging_dir" ]
mount_dir=$(cat "$state/mount-dir")
[ "${mount_dir#/tmp/.puredarwin-root-mount.}" != "$mount_dir" ]
[ ! -e "$mount_dir" ]
[ ! -e "$state/check-log" ]
[ -z "$(find "$shared_output" -mindepth 1 -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]

echo 'PureDarwin root exporter early staging signal cleanup test passed.'

# Replace the recorded inode with a same-owner private directory after its
# identity has been captured. The exporter must detect the mismatch before
# invoking e2fsck or writing into the replacement.
private_attacker_dir=$tmp/attacker-private
mkdir -- "$private_attacker_dir"
chmod 0700 "$private_attacker_dir"
printf 'private sentinel\n' >"$private_attacker_dir/root.tar.gz"
printf 'check sentinel\n' >"$private_attacker_dir/check.log"
rm -f -- "$state/check-log" "$state/identity-replaced" "$state/replaced-stage"
set +e
REPLACE_AFTER_IDENTITY=1 ATTACKER_DIR=$private_attacker_dir \
  "$script" "$image" "$output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -ne 0 ] || { echo 'Exporter accepted a same-owner staging inode replacement' >&2; exit 1; }
[ "$(cat "$private_attacker_dir/root.tar.gz")" = 'private sentinel' ]
[ "$(cat "$private_attacker_dir/check.log")" = 'check sentinel' ]
[ ! -e "$output" ]
[ ! -e "$state/check-log" ] || { echo 'Exporter started filesystem validation through a replaced staging inode' >&2; exit 1; }
replaced_stage=$(cat "$state/replaced-stage")
[ -d "$replaced_stage" ]
[ -d "$(cat "$state/archive-dir")" ]
[ ! -L "$(cat "$state/archive-dir")" ]
rm -rf -- "$replaced_stage"
rm -rf -- "$(cat "$state/archive-dir")"

echo 'PureDarwin root exporter same-owner inode replacement test passed.'

# Replace the directory after validation has started. Cleanup must compare the
# recorded staging identity and leave the new empty 0700 directory untouched.
rm -f -- "$state/check-log" "$state/archive-dir" "$state/cleanup-replacement" "$state/loop-attached" "$state/mounted"
set +e
REPLACE_DURING_E2FSCK=1 "$script" "$image" "$output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -ne 0 ] || { echo 'Exporter unexpectedly completed after staging replacement during cleanup' >&2; exit 1; }
cleanup_replacement=$(cat "$state/cleanup-replacement")
[ -d "$cleanup_replacement" ]
[ ! -L "$cleanup_replacement" ]
[ "$(stat -c '%a' -- "$cleanup_replacement")" = 700 ]
[ -z "$(find "$cleanup_replacement" -mindepth 1 -print -quit)" ]
cleanup_original=$(cat "$state/archive-dir").original
[ -d "$cleanup_original" ]
rm -rf -- "$cleanup_original"
rmdir -- "$cleanup_replacement"

echo 'PureDarwin root exporter cleanup inode replacement test passed.'

# An attacker-controlled replacement directory in the shared output parent
# must be rejected before the exporter writes its archive or e2fsck log.
attacker_dir=$tmp/attacker-owned
mkdir -- "$attacker_dir"
chmod 0755 "$attacker_dir"
printf 'private sentinel\n' >"$attacker_dir/root.tar.gz"
rm -f -- "$state/check-log"
set +e
REPLACE_ARCHIVE_DIR=1 ATTACKER_DIR=$attacker_dir \
  "$script" "$image" "$output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -ne 0 ] || { echo 'Exporter accepted a replaced staging directory' >&2; exit 1; }
[ "$(cat "$attacker_dir/root.tar.gz")" = 'private sentinel' ]
[ ! -e "$output" ]
check_log=$(cat "$state/check-log" 2>/dev/null || true)
[ -z "$check_log" ] || { echo 'Exporter started filesystem validation through a replaced staging path' >&2; exit 1; }
replaced_stage=$(cat "$state/replaced-stage")
[ -d "$replaced_stage" ]
rm -rf -- "$replaced_stage" "$(cat "$state/archive-dir")"

echo 'PureDarwin root exporter staging replacement test passed.'

# A directory name ending in a newline must survive output-directory
# canonicalization and keep the temporary staging path in that directory.
newline_dir=$tmp/$'newline-tail\n'
mkdir -- "$newline_dir"
rm -f -- "$state/check-log"
set +e
REPLACE_ARCHIVE_DIR=1 ATTACKER_DIR=$attacker_dir \
  "$script" "$image" "$newline_dir/must-not-exist.tar.gz" >/dev/null 2>&1
status=$?
set -e
[ "$status" -ne 0 ] || { echo 'Exporter accepted a replaced staging directory in a newline path' >&2; exit 1; }
newline_stage=$(cat "$state/archive-dir")
[[ "$newline_stage" == "$newline_dir"/* ]]
[ "$(cat "$attacker_dir/root.tar.gz")" = 'private sentinel' ]
replaced_stage=$(cat "$state/replaced-stage")
rm -rf -- "$replaced_stage" "$newline_stage"

echo 'PureDarwin root exporter newline path check passed.'
