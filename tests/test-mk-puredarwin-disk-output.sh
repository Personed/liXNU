#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

script_dir=$(cd "$(dirname "$0")/.." && pwd)
. "$script_dir/scripts/lib/publish-file-no-replace.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

regular="$work/existing.img"
symlink="$work/link.img"
dangling="$work/dangling.img"
fifo="$work/pipe.img"
printf 'keep this image\n' > "$regular"
ln -s "$(basename "$regular")" "$symlink"
ln -s missing.img "$dangling"
mkfifo "$fifo"

for output in "$regular" "$symlink" "$dangling" "$fifo"; do
  if message=$("$script_dir/scripts/mk-puredarwin-disk.sh" "$work/missing-root.tar.gz" "$output" 2>&1); then
    echo "expected existing output rejection: $output" >&2
    exit 1
  fi
  case "$message" in
    *'Refusing to replace existing output path:'*) ;;
    *) echo "unexpected error for $output: $message" >&2; exit 1 ;;
  esac
done

if message=$(cd "$script_dir" && CDPATH=. ./scripts/mk-puredarwin-disk.sh \
  "$work/missing-root.tar.gz" "$regular" 2>&1); then
  echo 'expected existing output rejection with CDPATH set' >&2
  exit 1
fi
case "$message" in
  *'Refusing to replace existing output path:'*) ;;
  *) echo "CDPATH broke script path resolution: $message" >&2; exit 1 ;;
esac

test "$(cat "$regular")" = 'keep this image'
test -L "$symlink"
test -L "$dangling"
test -p "$fifo"
printf 'existing regular, symlink, dangling symlink and FIFO destinations preserved\n'

successful_stage="$work/success-staging.img"
successful_output="$work/success-output.img"
printf 'completed image bytes\n' > "$successful_stage"
publish_file_no_replace "$successful_stage" "$successful_output"
test ! -e "$successful_stage"
test "$(cat "$successful_output")" = 'completed image bytes'
printf 'new image published and staging name removed\n'

# Simulate a directory appearing after validation but immediately before the
# POSIX link syscall; it must not receive the staging image.
race_stage="$work/race-staging.img"
race_destination="$work/raced-output.img"
printf 'staged image bytes\n' > "$race_stage"
real_link=$(command -v link)
mkdir "$work/bin"
cat > "$work/bin/link" <<'LINK'
#!/bin/bash
mkdir "$LINK_RACE_DESTINATION"
exec "$REAL_LINK" "$@"
LINK
chmod +x "$work/bin/link"
export REAL_LINK="$real_link" LINK_RACE_DESTINATION="$race_destination"
PATH="$work/bin:$PATH"
hash -r
if message=$(publish_file_no_replace "$race_stage" "$race_destination" 2>&1); then
  echo 'expected publication failure when a directory appears during generation' >&2
  exit 1
fi
case "$message" in
  *'Cannot publish'*) ;;
  *) echo "unexpected publication error: $message" >&2; exit 1 ;;
esac
test -f "$race_stage"
test -d "$race_destination"
test -z "$(find "$race_destination" -mindepth 1 -maxdepth 1 -print -quit)"
printf 'destination created at publish time preserved without a false success\n'

# A failed macOS image creation must clean its staging directory without
# trying to detach a volume that was never attached.
macos="$work/macos"
mkdir -p "$macos/bin" "$macos/output"
cat > "$macos/bin/uname" <<'UNAME'
#!/bin/bash
if [ "${1:-}" = -s ]; then printf 'Darwin\n'; else /usr/bin/uname "$@"; fi
UNAME
cat > "$macos/bin/hdiutil" <<'HDIUTIL'
#!/bin/bash
printf '%s\n' "$*" >> "$HDIUTIL_LOG"
[ "$1" != create ] || exit 1
exit 1
HDIUTIL
chmod +x "$macos/bin/uname" "$macos/bin/hdiutil"
export HDIUTIL_LOG="$macos/hdiutil.log"
if PATH="$macos/bin:$PATH" "$script_dir/scripts/mk-puredarwin-disk.sh" \
  "$macos/missing-root.tar.gz" "$macos/output/pd.img" >"$macos/stdout" 2>"$macos/stderr"; then
  echo 'expected hdiutil create failure' >&2
  exit 1
fi
grep -F 'create -quiet -size 1g -fs Journaled HFS+ -volname PureDarwin -layout GPTSPUD' "$HDIUTIL_LOG" >/dev/null
if grep -q '^detach ' "$HDIUTIL_LOG"; then
  echo 'cleanup attempted to detach a volume that was never attached' >&2
  exit 1
fi
test ! -e "$macos/output/pd.img"
test -z "$(find "$macos/output" -mindepth 1 -maxdepth 1 -name '.puredarwin-disk.*' -print -quit)"
printf 'failed macOS image creation cleaned staging without an invalid detach\n'

# If hdiutil reports an attach error after a mount appeared, cleanup must find
# the mountpoint when TMPDIR is a symlink to its canonical directory.
macos_attach="$work/macos-attach"
mkdir -p "$macos_attach/bin" "$macos_attach/output" "$macos_attach/private-var"
ln -s private-var "$macos_attach/var-alias"
cat > "$macos_attach/bin/uname" <<'UNAME'
#!/bin/bash
if [ "${1:-}" = -s ]; then printf 'Darwin\n'; else /usr/bin/uname "$@"; fi
UNAME
cat > "$macos_attach/bin/hdiutil" <<'HDIUTIL'
#!/bin/bash
printf '%s\n' "$*" >> "$HDIUTIL_LOG"
case "$1" in
  create)
    for arg in "$@"; do image=$arg; done
    : > "$image"
    ;;
  attach)
    shift
    while [ "$#" -gt 0 ]; do
      if [ "$1" = -mountpoint ]; then mountpoint=$2; shift 2; continue; fi
      image=$1
      shift
    done
    canonical=$(CDPATH= cd -P -- "$mountpoint" && pwd -P)
    printf '%s on %s (hfs, local, read-only)\n' "$image" "$canonical" > "$MOUNT_TABLE"
    exit 1
    ;;
  detach) exit 0 ;;
esac
HDIUTIL
cat > "$macos_attach/bin/mount" <<'MOUNT'
#!/bin/bash
cat "$MOUNT_TABLE"
MOUNT
chmod +x "$macos_attach/bin/uname" "$macos_attach/bin/hdiutil" "$macos_attach/bin/mount"
export HDIUTIL_LOG="$macos_attach/hdiutil.log" MOUNT_TABLE="$macos_attach/mount-table"
if TMPDIR="$macos_attach/var-alias" PATH="$macos_attach/bin:$PATH" \
  "$script_dir/scripts/mk-puredarwin-disk.sh" "$macos_attach/missing-root.tar.gz" \
  "$macos_attach/output/pd.img" >"$macos_attach/stdout" 2>"$macos_attach/stderr"; then
  echo 'expected hdiutil attach failure' >&2
  exit 1
fi
grep -F 'attach -quiet -owners on -nobrowse -mountpoint' "$HDIUTIL_LOG" >/dev/null
grep -F 'detach -quiet' "$HDIUTIL_LOG" >/dev/null
test ! -e "$macos_attach/output/pd.img"
test -z "$(find "$macos_attach/private-var" -mindepth 1 -maxdepth 1 -print -quit)"
test -z "$(find "$macos_attach/output" -mindepth 1 -maxdepth 1 -name '.puredarwin-disk.*' -print -quit)"
printf 'failed macOS attach cleaned the canonical staged mount and image\n'
