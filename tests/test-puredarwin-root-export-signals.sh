#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

script=$(cd -- "$(dirname -- "$0")/.." && pwd)/scripts/puredarwin-root.sh
real_mktemp=$(command -v mktemp)
real_chmod=$(command -v chmod)
real_stat=$(command -v stat)
real_mkdir=$(command -v mkdir)
real_sudo=$(command -v sudo || true)
unset TMPDIR
tmp=$(mktemp -d)
case "$tmp" in /*) ;; *) tmp=$PWD/$tmp ;; esac
trap 'rm -rf -- "$tmp"' EXIT
bin=$tmp/bin
state=$tmp/state
mkdir -- "$bin" "$state"
export SECURE_PATH=/usr/sbin:/usr/bin:/sbin:/bin
export TEST_STATE=$state
export FAKE_LOOP=/dev/loop-codex-test
export REAL_MKTEMP=$real_mktemp
export REAL_CHMOD=$real_chmod
export REAL_STAT=$real_stat
export REAL_MKDIR=$real_mkdir
export PATH="$bin:$PATH"

cat >"$bin/sfdisk" <<'WRAPPER'
#!/bin/sh
printf '%s\n' '{"partitiontable":{"label":"gpt","sectorsize":512,"partitions":[{"start":2048,"size":2048,"name":"Darwin ext4 Root"}]}}'
WRAPPER
cat >"$bin/sudo" <<'WRAPPER'
#!/bin/sh
if [ "${NFS_ROOT_SQUASH_TEST:-0}" = 1 ] && [ "$1" = test ] && [ "$2" = -w ]; then
  case "$3" in
    "$NFS_SIM_DIR"/*) exit 1 ;;
  esac
fi
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
# Simulate sudo's secure_path before the second script invocation. The child
# must restore the original PATH from XNULINUX_PRIVATE_MOUNT_PATH.
if [ -n "${XNULINUX_PRIVATE_MOUNT_PATH:-}" ]; then
  PATH=$SECURE_PATH
  export PATH
fi
exec "$@"
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
if [ ! -s "$TEST_STATE/mount-dir" ]; then printf '%s\n' "$mount_dir" >"$TEST_STATE/mount-dir"; fi
if [ -e "$TEST_STATE/mounted" ]; then exit 0; fi
if [ -d "$3" ]; then exit 32; fi
exit 1
WRAPPER
cat >"$bin/mount" <<'WRAPPER'
#!/bin/sh
: >"$TEST_STATE/mounted"
mount_dir=''
for arg do mount_dir=$arg; done
case "$mount_dir" in /*) ;; *) mount_dir=$PWD/$mount_dir ;; esac
mount_parent=${mount_dir%/*}
stat -c '%a' -- "$mount_parent" >"$TEST_STATE/mount-parent-mode"
printf '%s\n' "$mount_dir" >"$TEST_STATE/mount-dir"
printf 'signal test root\n' >"$mount_dir/hello.txt"
printf '%s\n' mounted >>"$TEST_STATE/events"
case "${RACE_STAGING:-}" in
  symlink)
    stage_dir=$(cat "$TEST_STATE/archive-dir")
    mv -- "$stage_dir" "$stage_dir.moved"
    ln -s -- "$ATTACKER_DIR" "$stage_dir"
    ;;
  same-owner-directory)
    stage_dir=$(cat "$TEST_STATE/archive-dir")
    mv -- "$stage_dir" "$stage_dir.moved"
    mkdir -m 0700 -- "$stage_dir"
    ;;
esac
if [ "${SIGNAL_AFTER_MOUNT:-0}" = 1 ]; then kill -TERM "$PPID"; fi
WRAPPER
cat >"$bin/umount" <<'WRAPPER'
#!/bin/sh
rm -f -- "$TEST_STATE/mounted"
mount_dir=$(cat "$TEST_STATE/mount-dir")
rm -f -- "$mount_dir/hello.txt"
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
  stage_dir=$(cat "$TEST_STATE/archive-dir")
  mv -- "$stage_dir" "$stage_dir.original"
  "$REAL_MKDIR" -m 0700 -- "$stage_dir"
  "$REAL_CHMOD" 0700 -- "$stage_dir"
  "$REAL_CHMOD" g-s -- "$stage_dir"
  printf '%s\n' "$stage_dir" >"$TEST_STATE/cleanup-replacement"
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
  printf 'replacement archive sentinel\n' >"$4/root.tar.gz"
  printf 'replacement check sentinel\n' >"$4/check.log"
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
cat >"$bin/mkdir" <<'WRAPPER'
#!/bin/sh
"$REAL_MKDIR" "$@" || exit
last=''
for arg do last=$arg; done
case "$last" in
  */mount)
    printf '%s\n' "${last%/mount}" >"$TEST_STATE/mount-parent"
    ;;
  *'.puredarwin-root-mount.'*)
    printf '%s\n' "$last" >"$TEST_STATE/mount-parent"
    if [ "${SIGNAL_DURING_MOUNT_PARENT_MKDIR:-0}" = 1 ]; then kill -TERM "$PPID"; fi
    ;;
esac
if [ "${SIGNAL_AFTER_MOUNT_DIR_MKDIR:-0}" = 1 ] && [ "$last" = mount ]; then
  kill -TERM "$PPID"
fi
WRAPPER
chmod +x "$bin/mkdir"

image=$tmp/fake.img
truncate -s 4194304 "$image"
output=$tmp/must-not-exist.tar.gz
rm -f -- "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode"
export SIGNAL_AFTER_MOUNT=1
set +e
TMPDIR=. "$script" "$image" "$output" >/dev/null 2>&1
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
if [ ! -s "$state/archive-dir" ]; then
  echo 'Exporter did not record the new signal-test staging directory.' >&2
  exit 1
fi
archive_dir=$(cat "$state/archive-dir")
mount_dir=$(cat "$state/mount-dir")
mount_parent=$(cat "$state/mount-parent")
[ "$mount_parent" = "${mount_dir%/*}" ]
[ "$mount_dir" != "$archive_dir/mount" ]
[ "$(cat "$state/mount-parent-mode")" = 700 ]
[ ! -e "$mount_dir" ]
[ ! -e "$mount_parent" ]
if [ -z "$archive_dir" ] || [ -e "$archive_dir" ] || [ -L "$archive_dir" ]; then
  echo "Exporter did not remove its signal-test staging directory: $archive_dir" >&2
  exit 1
fi
[ -z "$(find "$tmp" -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]
first_check_log=$(cat "$state/check-log")
[ -n "$first_check_log" ]
[ ! -e "$first_check_log" ]

echo 'PureDarwin root exporter signal cleanup test passed.'

# Also interrupt after loop setup and filesystem validation but before mount.
# util-linux mountpoint returns 32 for the existing, unmounted directory;
# cleanup must accept that state and still detach the loop and remove temps.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted"
rm -f -- "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode" "$state/check-log"
export SIGNAL_BEFORE_MOUNT=1
set +e
TMPDIR=. "$script" "$image" "$output" >/dev/null 2>&1
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
if [ ! -s "$state/archive-dir" ]; then
  echo 'Exporter did not record the new pre-mount staging directory.' >&2
  exit 1
fi
archive_dir=$(cat "$state/archive-dir")
mount_dir=$(cat "$state/mount-dir")
mount_parent=$(cat "$state/mount-parent")
[ "$mount_parent" = "${mount_dir%/*}" ]
[ "$mount_dir" != "$archive_dir/mount" ]
[ ! -e "$mount_dir" ]
[ ! -e "$mount_parent" ]
if [ -z "$archive_dir" ] || [ -e "$archive_dir" ] || [ -L "$archive_dir" ]; then
  echo "Exporter did not remove its pre-mount staging directory: $archive_dir" >&2
  exit 1
fi
[ -z "$(find "$tmp" -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]
second_check_log=$(cat "$state/check-log")
[ -n "$second_check_log" ]
[ "$second_check_log" != "$first_check_log" ]
[ ! -e "$second_check_log" ]

echo 'PureDarwin root exporter pre-mount signal cleanup test passed.'

# A signal after mktemp but before clearing setgid must still remove the empty
# 2700 staging directory using its recorded inode identity.
early_setgid_dir=$tmp/early-setgid
mkdir -- "$early_setgid_dir"
chmod 2700 "$early_setgid_dir"
early_output=$early_setgid_dir/root.tar.gz
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode" "$state/check-log"
set +e
SIGNAL_BEFORE_STAGING_CHMOD=1 "$script" "$image" "$early_output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq 143 ] || { echo "Expected early SIGTERM exit 143; got $status" >&2; exit 1; }
staging_dir=$(cat "$state/archive-dir")
[ ! -e "$staging_dir" ]
[ ! -e "$state/mount-dir" ]
[ -z "$(find "$early_setgid_dir" -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]

echo 'PureDarwin root exporter early staging signal cleanup test passed.'

# A signal while the private mount parent is created must not lose its path
# before the exit trap can remove it.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/check-log"
set +e
SIGNAL_DURING_MOUNT_PARENT_MKDIR=1 "$script" "$image" "$output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq 143 ] || { echo "Expected mount-parent SIGTERM exit 143; got $status" >&2; exit 1; }
mount_parent=$(cat "$state/mount-parent")
[ ! -e "$mount_parent" ]
if [ -s "$state/mount-dir" ]; then [ ! -e "$(cat "$state/mount-dir")" ]; fi
[ ! -e "$state/loop-attached" ]
if [ -s "$state/check-log" ]; then
  check_log=$(cat "$state/check-log")
  [ ! -e "$check_log" ]
fi
[ ! -e "$output" ]

echo 'PureDarwin root exporter mount-parent signal cleanup test passed.'

# A signal between mount-directory creation and inode recording must not leave
# the empty mount directory or its private staging parent behind.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode" "$state/check-log"
set +e
SIGNAL_AFTER_MOUNT_DIR_MKDIR=1 "$script" "$image" "$output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq 143 ] || { echo "Expected mount-dir SIGTERM exit 143; got $status" >&2; exit 1; }
archive_dir=$(cat "$state/archive-dir")
mount_dir=$(cat "$state/mount-dir")
mount_parent=$(cat "$state/mount-parent")
[ "$mount_parent" = "${mount_dir%/*}" ]
[ "$mount_dir" != "$archive_dir/mount" ]
[ ! -e "$mount_dir" ]
[ ! -e "$archive_dir" ]
[ ! -e "$mount_parent" ]
[ ! -e "$state/loop-attached" ]
if [ -e "$state/check-log" ]; then
  check_log=$(cat "$state/check-log")
  [ ! -e "$check_log" ]
fi

echo 'PureDarwin root exporter mount-directory signal cleanup test passed.'

# A same-owner directory replacement after the original identity was recorded
# must not receive archive or e2fsck writes.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode" "$state/check-log" "$state/identity-replaced" "$state/replaced-stage"
set +e
REPLACE_AFTER_IDENTITY=1 TMPDIR=. "$script" "$image" "$output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -ne 0 ] || { echo 'Exporter accepted a same-owner staging inode replacement' >&2; exit 1; }
[ ! -e "$output" ]
[ ! -e "$state/check-log" ]
replaced_stage=$(cat "$state/replaced-stage")
[ -d "$replaced_stage" ]
replacement_dir=$(cat "$state/archive-dir")
[ -d "$replacement_dir" ]
[ ! -L "$replacement_dir" ]
[ "$(cat "$replacement_dir/root.tar.gz")" = 'replacement archive sentinel' ]
[ "$(cat "$replacement_dir/check.log")" = 'replacement check sentinel' ]
rm -rf -- "$replaced_stage" "$replacement_dir"

echo 'PureDarwin root exporter same-owner staging identity test passed.'

# Replacing the stage during filesystem validation must leave the replacement
# directory alone while cleanup removes files from the original inode.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode" "$state/check-log" "$state/cleanup-replacement"
set +e
REPLACE_DURING_E2FSCK=1 TMPDIR=. "$script" "$image" "$output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -ne 0 ] || { echo 'Exporter unexpectedly succeeded after staging replacement during cleanup' >&2; exit 1; }
cleanup_replacement=$(cat "$state/cleanup-replacement")
[ -d "$cleanup_replacement" ]
[ ! -L "$cleanup_replacement" ]
[ "$(stat -c '%a' -- "$cleanup_replacement")" = 700 ]
[ -z "$(find "$cleanup_replacement" -mindepth 1 -print -quit)" ]
cleanup_original=$(cat "$state/archive-dir").original
[ -d "$cleanup_original" ]
rm -rf -- "$cleanup_original"
rmdir -- "$cleanup_replacement"

echo 'PureDarwin root exporter cleanup identity replacement test passed.'

# A replacement symlink created immediately after mktemp must be rejected
# before the exporter enters the directory or starts e2fsck.
attacker_dir=$tmp/attacker
mkdir -- "$attacker_dir"
printf 'private sentinel\n' >"$attacker_dir/root.tar.gz"
sentinel_sha=$(sha256sum "$attacker_dir/root.tar.gz" | cut -d ' ' -f 1)
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode" "$state/replaced-stage" "$state/check-log"
if TMPDIR=. REPLACE_ARCHIVE_DIR=1 ATTACKER_DIR="$attacker_dir" "$script" "$image" "$output" >/dev/null 2>"$tmp/pre-cd-race.log"; then
  echo 'Exporter accepted a staging path replaced before cd.' >&2
  exit 1
fi
archive_dir=$(cat "$state/archive-dir")
[ -L "$archive_dir" ]
[ "$(readlink -- "$archive_dir")" = "$attacker_dir" ]
[ "$(sha256sum "$attacker_dir/root.tar.gz" | cut -d ' ' -f 1)" = "$sentinel_sha" ]
[ ! -e "$output" ]
[ ! -e "$state/check-log" ]
[ ! -e "$state/mount-dir" ]
replaced_stage=$(cat "$state/replaced-stage")
[ -d "$replaced_stage" ]
rm -rf -- "$replaced_stage" "$archive_dir"
echo 'PureDarwin root exporter pre-cd staging replacement test passed.'

# Rename the private staging directory after the exporter has entered it, then
# replace its old pathname with a symlink to an attacker-controlled directory.
# Relative archive and log writes must stay pinned to the original directory.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode" "$state/check-log"
printf 'do not overwrite\n' >"$attacker_dir/root.tar.gz"
sentinel_sha=$(sha256sum "$attacker_dir/root.tar.gz" | cut -d ' ' -f 1)
export ATTACKER_DIR=$attacker_dir
export RACE_STAGING=symlink
unset SIGNAL_AFTER_MOUNT SIGNAL_BEFORE_MOUNT
race_output=$tmp/race-output.tar.gz
TMPDIR=. "$script" "$image" "$race_output" >/dev/null
if [ ! -s "$state/archive-dir" ]; then
  echo 'Exporter did not record the new race-test staging directory.' >&2
  exit 1
fi
archive_dir=$(cat "$state/archive-dir")
[ -n "$archive_dir" ]
[ -L "$archive_dir" ]
[ "$(readlink -- "$archive_dir")" = "$attacker_dir" ]
[ "$(sha256sum "$attacker_dir/root.tar.gz" | cut -d ' ' -f 1)" = "$sentinel_sha" ]
[ -d "$archive_dir.moved" ]
[ -z "$(find "$archive_dir.moved" -mindepth 1 -print -quit)" ]
[ "$(tar -xOzf "$race_output" ./hello.txt)" = 'signal test root' ]
[ ! -e "$(cat "$state/mount-dir")" ]
[ ! -e "$(cat "$state/mount-parent")" ]
rm -- "$archive_dir"
[ -n "$(find "$tmp" -maxdepth 1 -name '.puredarwin-root.*.moved' -print -quit)" ]
rmdir -- "$archive_dir.moved"
[ -z "$(find "$tmp" -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]

echo 'PureDarwin root exporter staging race test passed.'

# Replacing the staging path with a different empty directory that has the
# same owner and mode must still be detected by the inode comparison.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode" "$state/check-log"
same_owner_output=$tmp/same-owner-race.tar.gz
RACE_STAGING=same-owner-directory TMPDIR=. "$script" "$image" "$same_owner_output" >/dev/null 2>"$tmp/same-owner-race.log"
same_owner_stage=$(cat "$state/archive-dir")
[ -d "$same_owner_stage" ]
[ ! -L "$same_owner_stage" ]
[ "$(stat -c '%u' -- "$same_owner_stage")" = "$EUID" ]
[ "$(stat -c '%a' -- "$same_owner_stage")" = 700 ]
[ -z "$(find "$same_owner_stage" -mindepth 1 -print -quit)" ]
[ -d "$same_owner_stage.moved" ]
[ -z "$(find "$same_owner_stage.moved" -mindepth 1 -print -quit)" ]
grep -F 'Staging path changed during export; leaving it untouched' "$tmp/same-owner-race.log" >/dev/null
[ "$(tar -xOzf "$same_owner_output" ./hello.txt)" = 'signal test root' ]
[ ! -e "$(cat "$state/mount-dir")" ]
[ ! -e "$(cat "$state/mount-parent")" ]
[ ! -e "$state/loop-attached" ]
rm -rf -- "$same_owner_stage" "$same_owner_stage.moved"
echo 'PureDarwin root exporter same-owner inode check test passed.'

# Refuse a group/world-writable non-sticky output directory before creating any
# root-owned temporary paths there.
shared_dir=$tmp/shared
mkdir -- "$shared_dir"
chmod 0777 "$shared_dir"
shared_output=$shared_dir/root.tar.gz
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode"
if TMPDIR=. "$script" "$image" "$shared_output" >/dev/null 2>"$tmp/shared-export.log"; then
  echo 'Exporter unexpectedly accepted a shared writable output directory.' >&2
  exit 1
fi
grep -F 'Refusing an output directory that another local account can change' "$tmp/shared-export.log" >/dev/null
[ ! -e "$shared_output" ]
[ ! -e "$state/loop-attached" ]
[ ! -e "$state/archive-dir" ]
[ ! -e "$state/mount-dir" ]
[ "$(sha256sum "$attacker_dir/root.tar.gz" | cut -d ' ' -f 1)" = "$sentinel_sha" ]

echo 'PureDarwin root exporter shared-directory rejection test passed.'

# A setgid destination gives its private child a 2700 mode; that remains safe
# and must not make the exporter reject it.
unset RACE_STAGING
setgid_dir=$tmp/setgid-output
mkdir -- "$setgid_dir"
chmod 2700 "$setgid_dir"
setgid_output=$setgid_dir/root.tar.gz
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode"
TMPDIR=. "$script" "$image" "$setgid_output" >/dev/null
gzip -t "$setgid_output"
[ ! -e "$(cat "$state/mount-dir")" ]
if [ ! -s "$state/archive-dir" ]; then
  echo 'Exporter did not record the new setgid staging directory.' >&2
  exit 1
fi
archive_dir=$(cat "$state/archive-dir")
if [ -z "$archive_dir" ] || [ -e "$archive_dir" ] || [ -L "$archive_dir" ]; then
  echo "Exporter did not remove its setgid staging directory: $archive_dir" >&2
  exit 1
fi
[ -z "$(find "$setgid_dir" -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]

echo 'PureDarwin root exporter setgid staging test passed.'

# A root exporter must reject a destination controlled by a different account
# even when the directory mode is not group/world writable: its owner can
# change the mode while export is in progress.
root_runner=()
if [ "$EUID" -ne 0 ] && [ -n "$real_sudo" ] && "$real_sudo" -n true 2>/dev/null; then
  root_runner=("$real_sudo" -n)
fi
if [ "$EUID" -eq 0 ] || [ "${#root_runner[@]}" -gt 0 ]; then
  foreign_dir=$tmp/foreign-owned
  mkdir -- "$foreign_dir"
  chmod 0755 "$foreign_dir"
  if [ "$EUID" -eq 0 ]; then
    chown 65534:65534 "$foreign_dir"
  else
    "$real_sudo" -n chown 65534:65534 "$foreign_dir"
  fi
  foreign_output=$foreign_dir/root.tar.gz
  rm -f -- "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode" "$state/loop-attached"
  if "${root_runner[@]}" env PATH="$PATH" TEST_STATE="$state" FAKE_LOOP="$FAKE_LOOP" REAL_MKTEMP="$REAL_MKTEMP" TMPDIR=. "$script" "$image" "$foreign_output" >/dev/null 2>"$tmp/foreign-export.log"; then
    echo 'Exporter unexpectedly accepted a directory controlled by another account.' >&2
    exit 1
  fi
  grep -F 'Refusing an output directory that another local account can change' "$tmp/foreign-export.log" >/dev/null
  [ ! -e "$foreign_output" ]
  [ ! -e "$state/loop-attached" ]
  [ ! -e "$state/archive-dir" ]
  [ ! -e "$state/mount-dir" ]
  echo 'PureDarwin root exporter foreign-owner rejection test passed.'
else
  echo 'PureDarwin root exporter foreign-owner rejection test skipped (root or passwordless sudo unavailable).'
fi

# Paths that traverse a symlink and then `..` are canonicalized before staging,
# so the temporary archive and published output use the same physical parent.
physical_parent=$tmp/physical
mkdir -p -- "$physical_parent/branch" "$physical_parent/branch2"
ln -s -- "$physical_parent/branch" "$tmp/parent-link"
physical_output_dir=$tmp/parent-link/../branch2
physical_output=$physical_output_dir/path-resolution.tar.gz
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode"
TMPDIR=. "$script" "$image" "$physical_output" >/dev/null
[ -f "$physical_parent/branch2/path-resolution.tar.gz" ]
[ ! -e "$state/loop-attached" ]
[ ! -e "$(cat "$state/mount-dir")" ]
[ ! -e "$(cat "$state/mount-parent")" ]
[ -z "$(find "$physical_parent/branch2" -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]
echo 'PureDarwin root exporter physical-path resolution test passed.'

# pwd output ending in a newline must not be truncated while resolving a
# symlink to a directory whose physical name ends in a newline.
newline_dir=$tmp/$'physical-name\n'
mkdir -- "$newline_dir"
ln -s -- "$newline_dir" "$tmp/newline-link"
newline_output=$tmp/newline-link/newline-name.tar.gz
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode" "$state/check-log"
TMPDIR=. "$script" "$image" "$newline_output" >/dev/null
[ -f "$newline_dir/newline-name.tar.gz" ]
[ ! -e "$state/loop-attached" ]
[ ! -e "$(cat "$state/mount-dir")" ]
[ ! -e "$(cat "$state/mount-parent")" ]
[ -z "$(find "$newline_dir" -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]
echo 'PureDarwin root exporter newline-path resolution test passed.'

# Simulate a root-squashed NFS TMPDIR: the user's archive staging remains on
# that filesystem, but the privileged mount helper must fall back to local /tmp.
nfs_sim=$tmp/nfs-sim
mkdir -p -- "$nfs_sim/tmp" "$nfs_sim/output"
nfs_output=$nfs_sim/output/root.tar.gz
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/archive-dir" "$state/mount-dir" "$state/mount-parent" "$state/mount-parent-mode" "$state/check-log"
NFS_ROOT_SQUASH_TEST=1 NFS_SIM_DIR="$nfs_sim" TMPDIR="$nfs_sim/tmp" \
  "$script" "$image" "$nfs_output" >/dev/null
gzip -t "$nfs_output"
mount_parent=$(cat "$state/mount-parent")
case "$mount_parent" in
  "$nfs_sim"/*)
    echo "Mount staging remained under simulated root-squashed NFS: $mount_parent" >&2
    exit 1
    ;;
esac
[ ! -e "$mount_parent" ]
[ ! -e "$(cat "$state/mount-dir")" ]
[ ! -e "$state/loop-attached" ]
[ -f "$nfs_output" ]
echo 'PureDarwin root exporter root-squashed NFS staging test passed.'
