#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

script=$(cd -- "$(dirname -- "$0")/.." && pwd)/scripts/puredarwin-root.sh
if [ "$EUID" -ne 0 ] && ! command -v sudo >/dev/null; then
  echo 'Run this integration test as root or install sudo.' >&2
  exit 1
fi
for tool in sfdisk mkfs.ext4 debugfs losetup mount mountpoint umount e2fsck tune2fs tar python3 mknod findmnt unshare setpriv id; do
  command -v "$tool" >/dev/null || { echo "Missing test dependency: $tool" >&2; exit 1; }
done

run_as_root() {
  if [ "$EUID" -eq 0 ]; then "$@"; else sudo "$@"; fi
}

tmp=$(mktemp -d)
trap 'run_as_root rm -rf -- "$tmp"' EXIT
root_dir=$tmp/root
mkdir -- "$root_dir"
printf 'export smoke test\n' >"$root_dir/hello.txt"
partition=$tmp/root.ext4
root_sectors=65536
root_start=2048
truncate -s "$((root_sectors * 512))" "$partition"
mkfs.ext4 -q -F -d "$root_dir" "$partition"
image=$tmp/puredarwin.img
truncate -s "$(((root_start + root_sectors + 4096) * 512))" "$image"
sfdisk --quiet "$image" <<PARTITIONS
label: gpt
unit: sectors

start=$root_start, size=$root_sectors, type=linux, name="Darwin ext4 Root"
PARTITIONS
dd if="$partition" of="$image" bs=512 seek="$root_start" conv=notrunc status=none
original_sha=$(sha256sum "$image" | cut -d ' ' -f 1)

output_dir=$tmp/shared-output
mkdir -- "$output_dir"
chmod 1777 -- "$output_dir"
chmod_bin=$tmp/chmod-bin
mkdir -- "$chmod_bin"
real_chmod=$(command -v chmod)
cat >"$chmod_bin/chmod" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
if [ "${1:-}" = 0644 ] && [ "${2:-}" = -- ]; then
  archive_path=$3
  private_mode=$(stat -c '%a' -- "$(dirname -- "$archive_path")")
  [ "$private_mode" = 700 ] || {
    echo "Archive staging directory must be private (mode 700); got $private_mode." >&2
    exit 1
  }
  : >"@CHMOD_MARKER@"
fi
exec "@REAL_CHMOD@" "$@"
WRAPPER
chmod_marker=$tmp/chmod-called
sed -i "s|@REAL_CHMOD@|$real_chmod|g; s|@CHMOD_MARKER@|$chmod_marker|g" "$chmod_bin/chmod"
chmod +x "$chmod_bin/chmod"
archive=$output_dir/root.tar.gz
PATH="$chmod_bin:$PATH" "$script" "$image" "$archive" >/dev/null
[ "$(stat -c '%a' "$archive")" = 644 ]
[ "$(stat -c '%u' "$archive")" = "$EUID" ]
[ -f "$chmod_marker" ]
gzip -t "$archive"
tar -tzf "$archive" | grep -Fx './hello.txt' >/dev/null
[ "$(tar -xOzf "$archive" ./hello.txt)" = 'export smoke test' ]
[ "$(sha256sum "$image" | cut -d ' ' -f 1)" = "$original_sha" ]
[ -z "$(find "$output_dir" -mindepth 1 -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]

# Resolve the script path when Bash is asked to open it by a bare filename.
simple_output=$tmp/simple-invocation.tar.gz
(cd -- "$(dirname -- "$script")" && bash puredarwin-root.sh "$image" "$simple_output" >/dev/null)
[ -f "$simple_output" ]
[ "$(stat -c '%u' "$simple_output")" = "$EUID" ]

# A symlink plus '..' must resolve the same physical output directory for
# mktemp, cd, publication and cleanup.
mkdir -- "$output_dir/nested"
ln -s -- "$output_dir/nested" "$tmp/output-link"
symlink_output=$tmp/output-link/../via-symlink.tar.gz
PATH="$chmod_bin:$PATH" "$script" "$image" "$symlink_output" >/dev/null
[ -f "$output_dir/via-symlink.tar.gz" ]
[ ! -e "$tmp/via-symlink.tar.gz" ]

# Replace the temporary directory after mktemp creates it, simulating a user
# who can rename entries in the shared output directory. The exporter must
# detect the changed owner before writing through the replacement symlink.
attacker_dir=$tmp/attacker-owned
mkdir -- "$attacker_dir"
printf 'private sentinel\n' >"$attacker_dir/root.tar.gz"
chmod 0700 "$attacker_dir"
run_as_root chown 65534:65534 "$attacker_dir" "$attacker_dir/root.tar.gz"
replace_bin=$tmp/replace-bin
mkdir -- "$replace_bin"
real_mktemp=$(command -v mktemp)
cat >"$replace_bin/mktemp" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
result=$("@REAL_MKTEMP@" "$@")
case "$*" in
  *'.puredarwin-root.'*)
    mv -- "$result" "$result.original"
    ln -s -- "@ATTACKER_DIR@" "$result"
    ;;
esac
printf '%s\n' "$result"
WRAPPER
attack_output=$output_dir/attack-output.tar.gz
sed -i "s|@REAL_MKTEMP@|$real_mktemp|g; s|@ATTACKER_DIR@|$attacker_dir|g" "$replace_bin/mktemp"
chmod +x "$replace_bin/mktemp"
if PATH="$replace_bin:$PATH" \
  "$script" "$image" "$attack_output" >/dev/null 2>&1; then
  echo 'Exporter accepted a replaced staging directory.' >&2
  exit 1
fi
[ "$(run_as_root cat "$attacker_dir/root.tar.gz")" = 'private sentinel' ]
[ ! -e "$attack_output" ]
replaced_dir=$(find "$output_dir" -maxdepth 1 -type d -name '.puredarwin-root.*.original' -print -quit)
[ -n "$replaced_dir" ]
run_as_root rm -rf -- "$replaced_dir"
rm -- "$output_dir"/.puredarwin-root.*

expect_refusal() {
  local path=$1
  if "$script" "$image" "$path" >/dev/null 2>&1; then
    echo "Exporter unexpectedly replaced or accepted $path" >&2
    exit 1
  fi
}

printf 'keep me\n' >"$tmp/existing-file"
expect_refusal "$tmp/existing-file"
[ "$(cat "$tmp/existing-file")" = 'keep me' ]
printf 'symlink target\n' >"$tmp/symlink-target"
ln -s -- "$tmp/symlink-target" "$tmp/existing-symlink"
expect_refusal "$tmp/existing-symlink"
[ -L "$tmp/existing-symlink" ]
[ "$(cat "$tmp/symlink-target")" = 'symlink target' ]
ln -s -- "$tmp/no-such-target" "$tmp/dangling-symlink"
expect_refusal "$tmp/dangling-symlink"
[ -L "$tmp/dangling-symlink" ]
mkfifo -- "$tmp/existing-fifo"
expect_refusal "$tmp/existing-fifo"
[ -p "$tmp/existing-fifo" ]
run_as_root mknod -- "$tmp/existing-device" c 1 3
expect_refusal "$tmp/existing-device"
[ -c "$tmp/existing-device" ]
mkdir -- "$tmp/existing-directory"
printf 'keep directory\n' >"$tmp/existing-directory/sentinel"
expect_refusal "$tmp/existing-directory"
[ "$(cat "$tmp/existing-directory/sentinel")" = 'keep directory' ]

# Force the final destination to appear between the preflight check and link(2).
race_bin=$tmp/race-bin
mkdir -- "$race_bin"
real_link=$(command -v link)
cat >"$race_bin/link" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
mkdir -- "$2"
exec "@REAL_LINK@" "$@"
WRAPPER
race_path=$tmp/raced-output
sed -i "s|@REAL_LINK@|$real_link|g" "$race_bin/link"
chmod +x "$race_bin/link"
if PATH="$race_bin:$PATH" "$script" "$image" "$race_path" >/dev/null 2>&1; then
  echo 'Exporter unexpectedly published over a destination created during export.' >&2
  exit 1
fi
[ -d "$race_path" ]
[ -z "$(find "$tmp" -maxdepth 1 -name '.puredarwin-root.*' -print -quit)" ]

# Mark an ext4 fixture as needing journal recovery without mounting or replaying it.
dirty_partition=$tmp/dirty.ext4
cp -- "$partition" "$dirty_partition"
debugfs -w -R 'feature needs_recovery' "$dirty_partition" >/dev/null 2>&1
tune2fs -l "$dirty_partition" | grep -F 'needs_recovery' >/dev/null
dirty_image=$tmp/puredarwin-dirty.img
cp -- "$image" "$dirty_image"
dd if="$dirty_partition" of="$dirty_image" bs=512 seek="$root_start" conv=notrunc status=none
dirty_sha=$(sha256sum "$dirty_image" | cut -d ' ' -f 1)
dirty_output=$tmp/dirty-output.tar.gz
if "$script" "$dirty_image" "$dirty_output" >/dev/null 2>&1; then
  echo 'Exporter unexpectedly accepted a filesystem with pending journal recovery.' >&2
  exit 1
fi
[ ! -e "$dirty_output" ]
[ "$(sha256sum "$dirty_image" | cut -d ' ' -f 1)" = "$dirty_sha" ]
[ "$(sha256sum "$image" | cut -d ' ' -f 1)" = "$original_sha" ]

echo 'PureDarwin root export integration tests passed.'
