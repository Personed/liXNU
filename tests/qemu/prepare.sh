#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Prepare an ARM64 Linux kernel and initramfs for the QEMU module and carve-out smoke test.
set -euo pipefail

if [ "$#" -ne 6 ]; then
  echo "Usage: $0 ARM64_KERNEL ARM64_STATIC_BUSYBOX FKI_KO XNU_KO FKI_CARVE_PROBE_KO OUTPUT_DIR" >&2
  exit 2
fi
for file in "$1" "$2" "$3" "$4" "$5"; do
  if [ ! -f "$file" ] || [ ! -r "$file" ]; then
    printf 'Cannot read input file: %s\n' "$file" >&2
    exit 2
  fi
done

out=$6
mkdir -p -- "$out"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
# Ubuntu's ARM64 vmlinuz can be an EFI zboot image compressed with zstd,
# which QEMU's direct kernel loader cannot unpack. Use its embedded Image.
python3 - "$1" "$out/Image" <<'PYTHON'
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

mkdir -p "$work/root/bin"
cp -- "$2" "$work/root/bin/busybox"
cp -- "$3" "$work/root/fki.ko"
cp -- "$4" "$work/root/xnu.ko"
cp -- "$5" "$work/root/fki-carve-probe.ko"
cat > "$work/root/init" <<'INIT'
#!/bin/busybox sh
set -eu
export PATH=/bin
/bin/busybox --install -s /bin
mkdir -p /dev /proc /sys /tmp
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
trap 'echo "xnulinux QEMU smoke test failed"; dmesg; poweroff -f' EXIT

test "$(uname -m)" = aarch64
grep -qw paca /proc/cpuinfo
insmod /fki.ko carve_base=0x70000000 carve_mb=256
test -c /dev/xnu-hostcall
test -d /sys/class/net/xnu0
insmod /fki-carve-probe.ko
dmesg | grep -Fq 'fki-carve-probe: allocation 1 got 16777216 bytes at physical 0x70000000' || {
  echo "FKI did not allocate from the requested physical carve-out on pass 1"
  exit 1
}
dmesg | grep -Fq 'fki-carve-probe: allocation 2 got 16777216 bytes at physical 0x70000000' || {
  echo "FKI did not reuse the freed physical carve-out on pass 2"
  exit 1
}
rmmod fki-carve-probe

# xnu.ko should resolve its FKI symbols and get as far as requesting firmware.
# This image is deliberately absent so the test needs no kernel collection.
if insmod /xnu.ko image=xnulinux-smoke-missing.bin 2>/tmp/insmod-error; then
  echo "xnu.ko unexpectedly loaded without firmware"
  exit 1
fi
dmesg | grep -Fq 'xnu: cannot read image' || { cat /tmp/insmod-error >&2; exit 1; }
test ! -d /sys/module/xnu
rmmod fki
test ! -e /dev/xnu-hostcall
test ! -e /sys/class/net/xnu0

trap - EXIT
echo "xnulinux QEMU smoke test passed"
poweroff -f
INIT
chmod +x "$work/root/init" "$work/root/bin/busybox"
(cd "$work/root" && find . -print0 | cpio --quiet --null -o -H newc --owner=0:0) > "$out/initramfs"
