#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build and run the Linux NMI boundary probe only in a disposable TCG VM.
set -euo pipefail

if [ "$#" -ne 1 ] || [ ! -f "$1/Makefile" ]; then
	echo "usage: $0 KDIR" >&2
	exit 2
fi

KDIR=$(cd "$1" && pwd)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
for tool in apt-cache apt-get cat chmod cp cpio dpkg-deb find gcc grep gzip id install make mkdir mktemp mknod python3 qemu-system-x86_64 rm sort; do
	command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done
if [ "$(id -u)" -ne 0 ]; then
	command -v sudo >/dev/null || { echo "missing required tool: sudo" >&2; exit 1; }
fi

KERNEL_RELEASE=$(cat "$KDIR/include/config/kernel.release")
test -n "$KERNEL_RELEASE" || { echo "kernel release metadata is missing from KDIR" >&2; exit 1; }
KERNEL_IMAGE_PACKAGE=
for candidate in "linux-image-unsigned-$KERNEL_RELEASE" "linux-image-$KERNEL_RELEASE-unsigned"; do
	if apt-cache show "$candidate" 2>/dev/null | grep -F "Package: $candidate" >/dev/null; then
		KERNEL_IMAGE_PACKAGE=$candidate
		break
	fi
done
test -n "$KERNEL_IMAGE_PACKAGE" || {
	echo "no Debian/Ubuntu unsigned kernel image package found for $KERNEL_RELEASE" >&2
	exit 1
}

STAGE=$(mktemp -d)
trap 'rm -rf -- "$STAGE"' EXIT
BUILD="$STAGE/build/nmi-boundary"
mkdir -p "$BUILD"
cp -a "$REPO/tests/x86_64/nmi-boundary/." "$BUILD/"
make -C "$KDIR" ARCH=x86_64 M="$BUILD" CONFIG_DEBUG_INFO_BTF_MODULES= modules
gcc -O2 -Wall -Wextra -Werror -static \
	-I"$REPO/tests/x86_64/nmi-boundary" \
	-o "$STAGE/build/nmi-boundary-test" \
	"$REPO/tests/x86_64/nmi-boundary/user_test.c"

(
	cd "$STAGE"
	apt-get download "$KERNEL_IMAGE_PACKAGE" busybox-static >/dev/null
)
IMAGE_DEB=$(find "$STAGE" -maxdepth 1 -name "${KERNEL_IMAGE_PACKAGE}_*.deb" -print -quit)
BUSYBOX_DEB=$(find "$STAGE" -maxdepth 1 -name 'busybox-static_*.deb' -print -quit)
test -n "$IMAGE_DEB" || { echo "matching unsigned kernel image package not found" >&2; exit 1; }
test -n "$BUSYBOX_DEB" || { echo "busybox-static package not found" >&2; exit 1; }

mkdir -p "$STAGE/kernel" "$STAGE/busybox"
dpkg-deb -x "$IMAGE_DEB" "$STAGE/kernel"
dpkg-deb -x "$BUSYBOX_DEB" "$STAGE/busybox"
KERNEL="$STAGE/kernel/boot/vmlinuz-$KERNEL_RELEASE"
BUSYBOX=$(find "$STAGE/busybox" -type f -name busybox -perm /111 -print -quit)
test -s "$KERNEL" || { echo "matching kernel image not found: $KERNEL" >&2; exit 1; }
test -s "$BUSYBOX" || { echo "executable busybox-static binary not found" >&2; exit 1; }

ROOTFS="$STAGE/rootfs"
mkdir -p "$ROOTFS/bin" "$ROOTFS/dev" "$ROOTFS/proc" "$ROOTFS/sys"
install -m 0755 "$BUSYBOX" "$ROOTFS/bin/busybox"
install -m 0755 "$STAGE/build/nmi-boundary-test" "$ROOTFS/nmi-boundary-test"
install -m 0644 "$BUILD/x86-nmi-boundary.ko" "$ROOTFS/x86-nmi-boundary.ko"
cat > "$ROOTFS/init" <<'INIT'
#!/bin/busybox sh
set -eu
export PATH=/bin
/bin/busybox --install -s /bin
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mount -t proc proc /proc
mount -t sysfs sysfs /sys
failure_cleanup() {
	echo X86_NMI_BOUNDARY_VM_FAIL
	dmesg
	rmmod x86_nmi_boundary 2>/dev/null || true
	poweroff -f
}
trap failure_cleanup EXIT

insmod /x86-nmi-boundary.ko
/nmi-boundary-test
rmmod x86_nmi_boundary
test ! -e /sys/module/x86_nmi_boundary
trap - EXIT
echo X86_NMI_BOUNDARY_VM_PASS
sync
poweroff -f
INIT
chmod 0755 "$ROOTFS/init"
if [ "$(id -u)" -eq 0 ]; then
	mknod "$ROOTFS/dev/console" c 5 1
	mknod "$ROOTFS/dev/null" c 1 3
	chmod 0600 "$ROOTFS/dev/console"
	chmod 0666 "$ROOTFS/dev/null"
else
	sudo mknod "$ROOTFS/dev/console" c 5 1
	sudo mknod "$ROOTFS/dev/null" c 1 3
	sudo chmod 0600 "$ROOTFS/dev/console"
	sudo chmod 0666 "$ROOTFS/dev/null"
fi
INITRD="$STAGE/nmi-boundary-initramfs.cpio.gz"
(
	cd "$ROOTFS"
	find . -print0 | sort -z | cpio --null --create --format=newc --owner=0:0 2>/dev/null
) | gzip -n -9 > "$INITRD"

python3 "$REPO/tests/x86_64/nmi-boundary/qmp_inject_nmi.py" \
	--qemu "$(command -v qemu-system-x86_64)" \
	--kernel "$KERNEL" \
	--initrd "$INITRD" \
	--qmp-socket "$STAGE/qmp.sock" \
	--serial-log "$STAGE/serial.log" \
	--debug-log "$STAGE/debugcon.log"
