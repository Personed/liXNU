#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Exercise the proprietary x86_64 stub only inside a disposable QEMU guest.
set -euo pipefail

if [ "$#" -ne 1 ] || [ ! -f "$1/Makefile" ]; then
  echo "usage: $0 KDIR" >&2
  exit 2
fi

KDIR=$(cd "$1" && pwd)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
for tool in apt-get cat chmod cpio dpkg-deb find grep gzip install mkdir mktemp mknod qemu-system-x86_64 rm sort sudo timeout; do
  command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done
KERNEL_RELEASE=$(cat "$KDIR/include/config/kernel.release")
test -n "$KERNEL_RELEASE" || { echo "kernel release metadata is missing from KDIR" >&2; exit 1; }
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

test -s "$REPO/xnu-ko/xnu.ko" || { echo "build xnu-ko/xnu.ko before running this test" >&2; exit 1; }

(
  cd "$STAGE"
  apt-get download "linux-image-unsigned-$KERNEL_RELEASE" busybox-static >/dev/null
)

IMAGE_DEB=$(find "$STAGE" -maxdepth 1 -name "linux-image-unsigned-${KERNEL_RELEASE}_*.deb" -print -quit)
BUSYBOX_DEB=$(find "$STAGE" -maxdepth 1 -name 'busybox-static_*.deb' -print -quit)
test -n "$IMAGE_DEB" || { echo "matching unsigned kernel image package not found" >&2; exit 1; }
test -n "$BUSYBOX_DEB" || { echo "busybox-static package not found" >&2; exit 1; }

mkdir -p "$STAGE/kernel" "$STAGE/busybox"
dpkg-deb -x "$IMAGE_DEB" "$STAGE/kernel"
dpkg-deb -x "$BUSYBOX_DEB" "$STAGE/busybox"
KERNEL="$STAGE/kernel/boot/vmlinuz-$KERNEL_RELEASE"
BUSYBOX=$(find "$STAGE/busybox" -type f -name busybox -print -quit)
test -s "$KERNEL" || { echo "matching kernel image not found: $KERNEL" >&2; exit 1; }
test -s "$BUSYBOX" || { echo "busybox-static binary not found" >&2; exit 1; }

ROOTFS="$STAGE/rootfs"
mkdir -p "$ROOTFS/bin" "$ROOTFS/dev" "$ROOTFS/proc" "$ROOTFS/sys"
install -m 0755 "$BUSYBOX" "$ROOTFS/bin/busybox"
install -m 0644 "$REPO/xnu-ko/xnu.ko" "$ROOTFS/xnu.ko"
sudo mknod "$ROOTFS/dev/console" c 5 1
sudo mknod "$ROOTFS/dev/null" c 1 3
sudo chmod 0600 "$ROOTFS/dev/console"
sudo chmod 0666 "$ROOTFS/dev/null"

cat > "$ROOTFS/init" <<'INIT'
#!/bin/busybox sh
set -eu
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys

set +e
insmod_output=$(/bin/busybox insmod /xnu.ko 2>&1)
insmod_rc=$?
set -e
echo "$insmod_output"
if [ "$insmod_rc" -eq 0 ]; then
	/bin/busybox echo "FAIL: xnu.ko unexpectedly loaded"
	exit 1
fi
case "$insmod_output" in
	*"Operation not supported"*) ;;
	*) /bin/busybox echo "FAIL: insmod did not report EOPNOTSUPP"; exit 1 ;;
esac
if [ -e /sys/module/xnu ]; then
	/bin/busybox echo "FAIL: xnu.ko remains present after rejected initialization"
	exit 1
fi
tainted=$(/bin/busybox cat /proc/sys/kernel/tainted)
if [ $((tainted & 1)) -eq 0 ]; then
	/bin/busybox echo "FAIL: proprietary-module taint bit was not set"
	exit 1
fi

/bin/busybox echo XNU_STUB_VM_TEST_PASS
/bin/busybox poweroff -f
INIT
chmod 0755 "$ROOTFS/init"

INITRD="$STAGE/xnu-stub-initramfs.cpio.gz"
(
  cd "$ROOTFS"
  find . -print0 | sort -z | cpio --null --create --format=newc --owner=0:0 2>/dev/null
) | gzip -n -9 > "$INITRD"

LOG="$STAGE/qemu-serial.log"
set +e
timeout 240 qemu-system-x86_64 -machine q35,accel=tcg -cpu max -smp 2 -m 1024 -kernel "$KERNEL" -initrd "$INITRD" -append 'console=ttyS0,115200 rdinit=/init panic=1 loglevel=7' -display none -serial stdio -monitor none -no-reboot > "$LOG" 2>&1
QEMU_RC=$?
set -e
cat "$LOG"
if [ "$QEMU_RC" -ne 0 ]; then
  echo "QEMU disposable-VM test failed with status $QEMU_RC" >&2
  exit "$QEMU_RC"
fi
grep -F 'xnu: native x86_64 world switch is not implemented; refusing to load' "$LOG" >/dev/null || {
  echo "disposable VM did not log that the xnu stub initializer ran" >&2
  exit 1
}
grep -F 'XNU_STUB_VM_TEST_PASS' "$LOG" >/dev/null || {
  echo "disposable VM did not report the xnu stub test marker" >&2
  exit 1
}
