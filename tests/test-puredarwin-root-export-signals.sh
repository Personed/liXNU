#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

script=$(cd -- "$(dirname -- "$0")/.." && pwd)/scripts/puredarwin-root.sh
real_mktemp=$(command -v mktemp)
real_chmod=$(command -v chmod)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
bin=$tmp/bin
state=$tmp/state
shared_output=$tmp/shared-output
mkdir -- "$bin" "$state" "$shared_output"
chmod 2777 -- "$shared_output"
export TEST_STATE=$state
export FAKE_LOOP=/dev/loop-codex-test
export REAL_MKTEMP=$real_mktemp
export REAL_CHMOD=$real_chmod
export PATH="$bin:$PATH"

cat >"$bin/sfdisk" <<'WRAPPER'
#!/bin/sh
printf '%s\n' '{"partitiontable":{"label":"gpt","sectorsize":512,"partitions":[{"start":2048,"size":2048,"name":"Darwin ext4 Root"}]}}'
WRAPPER
cat >"$bin/sudo" <<'WRAPPER'
#!/bin/sh
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
[ ! -e "$mount_dir" ]
[ -z "$(find "$shared_output" -mindepth 1 -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]
first_check_log=$(cat "$state/check-log")
[ -n "$first_check_log" ]
[ ! -e "$first_check_log" ]

echo 'PureDarwin root exporter signal cleanup test passed.'

# Also interrupt after loop setup and filesystem validation but before mount.
# util-linux mountpoint returns 32 for the existing, unmounted directory;
# cleanup must accept that state and still detach the loop and remove temps.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted"
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
[ ! -e "$mount_dir" ]
[ -z "$(find "$shared_output" -mindepth 1 -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]
second_check_log=$(cat "$state/check-log")
[ -n "$second_check_log" ]
[ "$second_check_log" != "$first_check_log" ]
[ ! -e "$second_check_log" ]

echo 'PureDarwin root exporter pre-mount signal cleanup test passed.'

# A signal after mktemp but before clearing setgid must still remove the empty
# 2700 staging directory using its recorded inode identity.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted" "$state/check-log"
set +e
SIGNAL_BEFORE_STAGING_CHMOD=1 "$script" "$image" "$output" >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq 143 ] || { echo "Expected early SIGTERM exit 143; got $status" >&2; exit 1; }
staging_dir=$(cat "$state/archive-dir")
[ ! -e "$staging_dir" ]
[ -z "$(find "$shared_output" -mindepth 1 -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]

echo 'PureDarwin root exporter early staging signal cleanup test passed.'

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
