#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

script_dir=$(cd "$(dirname "$0")/.." && pwd)
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

test "$(cat "$regular")" = 'keep this image'
test -L "$symlink"
test -L "$dangling"
test -p "$fifo"
printf 'existing regular, symlink, dangling symlink and FIFO destinations preserved\n'
