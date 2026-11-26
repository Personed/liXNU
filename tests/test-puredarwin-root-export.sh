#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

script=$(cd -- "$(dirname -- "$0")/.." && pwd)/scripts/puredarwin-root.sh
if [ "$EUID" -ne 0 ]; then
  echo 'Run this integration test as root (for example, sudo bash tests/test-puredarwin-root-export.sh).' >&2
  exit 1
fi
for tool in sfdisk mkfs.ext4 debugfs losetup mount mountpoint umount e2fsck tune2fs tar python3 mknod; do
  command -v "$tool" >/dev/null || { echo "Missing test dependency: $tool" >&2; exit 1; }
done

tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
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

archive=$tmp/root.tar.gz
"$script" "$image" "$archive" >/dev/null
[ "$(stat -c '%a' "$archive")" = 644 ]
gzip -t "$archive"
tar -tzf "$archive" | grep -Fx './hello.txt' >/dev/null
[ "$(tar -xOzf "$archive" ./hello.txt)" = 'export smoke test' ]
[ "$(sha256sum "$image" | cut -d ' ' -f 1)" = "$original_sha" ]

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
[ -L "$tmp/existing-symlink" ] && [ "$(cat "$tmp/symlink-target")" = 'symlink target' ]
ln -s -- "$tmp/no-such-target" "$tmp/dangling-symlink"
expect_refusal "$tmp/dangling-symlink"
[ -L "$tmp/dangling-symlink" ]
mkfifo -- "$tmp/existing-fifo"
expect_refusal "$tmp/existing-fifo"
[ -p "$tmp/existing-fifo" ]
mknod -- "$tmp/existing-device" c 1 3
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
exec "$REAL_LINK" "$@"
WRAPPER
chmod +x "$race_bin/link"
race_path=$tmp/raced-output
if REAL_LINK=$real_link PATH="$race_bin:$PATH" "$script" "$image" "$race_path" >/dev/null 2>&1; then
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
