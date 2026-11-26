#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

if [ "$#" -ne 8 ]; then
  echo "Usage: $0 ARM64_VMLINUZ STATIC_BUSYBOX FKI_KO XNU_KO KC_FREE PD_CONF KERNEL_MODULES_DIR OUTPUT_DIR" >&2
  exit 2
fi
for input in "$1" "$2" "$3" "$4" "$5" "$6"; do
  if [ ! -r "$input" ]; then
    printf 'Cannot read input: %s\n' "$input" >&2
    exit 2
  fi
done

kernel=$1 busybox=$2 fki=$3 xnu=$4 kc=$5 conf=$6 modules=$7 out=$8
if [ ! -d "$modules" ]; then
  printf 'Cannot read kernel modules directory: %s\n' "$modules" >&2
  exit 2
fi
mkdir -p -- "$out"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

# Ubuntu's ARM64 vmlinuz can be an EFI zboot image compressed with zstd.
python3 - "$kernel" "$out/Image" <<'PYTHON'
import shutil
import struct
import subprocess
import sys

kernel, output = sys.argv[1:]
with open(kernel, "rb") as source:
    header = source.read(64)
    if header[4:8] == b"zimg" and header[24:32].rstrip(b"\0") == b"zstd":
        offset, size = struct.unpack_from("<II", header, 8)
        source.seek(offset)
        payload = source.read(size)
        if offset < len(header) or not size or len(payload) != size:
            sys.exit(f"{kernel}: invalid EFI zboot payload")
        with open(output, "wb") as image:
            subprocess.run(["zstd", "--decompress", "--stdout"], input=payload, stdout=image, check=True)
    else:
        shutil.copyfile(kernel, output)
PYTHON

root=$work/root
mkdir -p "$root/bin" "$root/lib/modules" "$root/lib/firmware/xnu"
busybox_applets=$("$busybox" --list)
if ! grep -qx modprobe <<<"$busybox_applets"; then
  echo 'Static BusyBox must include the modprobe applet for virtio disk modules.' >&2
  exit 1
fi
cp -- "$busybox" "$root/bin/busybox"
cp -- "$fki" "$root/fki.ko"
cp -- "$xnu" "$root/xnu.ko"
cp -- "$kc" "$root/lib/firmware/xnu/kc-free.bin"
cp -- "$conf" "$root/pd.conf"
cp -a -- "$modules" "$root/lib/modules/"
cp -- "$(dirname -- "$0")/init" "$root/init"
chmod 0755 "$root/bin/busybox" "$root/init"
find "$root/lib/modules" -type f -name '*.ko.zst' -print0 |
  while IFS= read -r -d '' module; do zstd --decompress --rm "$module"; done
find "$root/lib/modules" -type f -name '*.ko.xz' -print0 |
  while IFS= read -r -d '' module; do xz --decompress -- "$module"; done
depmod -b "$root" "$(basename -- "$modules")"

(cd "$root" && find . -print0 | cpio --quiet --null -o -H newc --owner=0:0) |
  gzip -1 > "$out/initramfs.cpio.gz"
printf 'Prepared ARM64 PureDarwin guest inputs in %s\n' "$out"
