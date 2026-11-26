#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

script=$(cd -- "$(dirname -- "$0")/.." && pwd)/scripts/puredarwin-root.sh
real_mktemp=$(command -v mktemp)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
bin=$tmp/bin
state=$tmp/state
mkdir -- "$bin" "$state"
export TEST_STATE=$state
export FAKE_LOOP=/dev/loop-codex-test
export REAL_MKTEMP=$real_mktemp
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
if [ "${SIGNAL_BEFORE_MOUNT:-0}" = 1 ]; then kill -TERM "$PPID"; fi
exit 0
WRAPPER
cat >"$bin/mktemp" <<'WRAPPER'
#!/bin/sh
result=$("$REAL_MKTEMP" "$@") || exit
if [ "$1" = '-d' ]; then
  case "$*" in
    *'.puredarwin-root.'*) printf '%s\n' "$result" >"$TEST_STATE/archive-dir" ;;
    *) printf '%s\n' "$result" >"$TEST_STATE/mount-dir" ;;
  esac
elif [ "$#" -eq 0 ]; then
  printf '%s\n' "$result" >"$TEST_STATE/check-log"
fi
printf '%s\n' "$result"
WRAPPER
chmod +x "$bin"/*

image=$tmp/fake.img
truncate -s 4194304 "$image"
output=$tmp/must-not-exist.tar.gz
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
[ -z "$(find "$tmp" -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]
[ ! -e "$(cat "$state/check-log")" ]

echo 'PureDarwin root exporter signal cleanup test passed.'

# Also interrupt after loop setup and filesystem validation but before mount.
# util-linux mountpoint returns 32 for the existing, unmounted directory;
# cleanup must accept that state and still detach the loop and remove temps.
rm -f -- "$state/events" "$state/loop-attached" "$state/mounted"
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
[ -z "$(find "$tmp" -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]
[ ! -e "$(cat "$state/check-log")" ]

echo 'PureDarwin root exporter pre-mount signal cleanup test passed.'
