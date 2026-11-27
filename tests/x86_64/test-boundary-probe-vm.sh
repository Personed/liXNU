#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Run the x86_64 register/host-state probe only in a disposable Linux VM.
set -euo pipefail

if { [ "$#" -ne 1 ] && [ "$#" -ne 3 ]; } || [ ! -f "$1/Makefile" ]; then
  echo "usage: $0 KDIR [KERNEL_IMAGE BUSYBOX_STATIC]" >&2
  exit 2
fi

KDIR=$(cd -- "$1" && pwd)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
tools=(cat chmod cp cpio find gcc grep gzip id install make mkdir mktemp mknod qemu-system-x86_64 rm sort timeout)
if [ "$#" -eq 1 ]; then
  tools+=(apt-cache apt-get dpkg-deb)
else
  KERNEL=$2
  BUSYBOX=$3
  if [ ! -f "$KERNEL" ] || [ ! -r "$KERNEL" ] || [ ! -s "$KERNEL" ]; then
    echo "kernel image must be a readable, nonempty file: $KERNEL" >&2
    exit 2
  fi
  if [ ! -f "$BUSYBOX" ] || [ ! -r "$BUSYBOX" ] || [ ! -s "$BUSYBOX" ] || [ ! -x "$BUSYBOX" ]; then
    echo "static BusyBox must be a readable, nonempty executable file: $BUSYBOX" >&2
    exit 2
  fi
fi
for tool in "${tools[@]}"; do
  command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done
if [ "$(id -u)" -ne 0 ]; then
  command -v sudo >/dev/null || { echo "missing required tool: sudo" >&2; exit 1; }
fi
if ! QEMU_VERSION_OUTPUT=$(qemu-system-x86_64 --version 2>&1); then
  echo "unable to determine qemu-system-x86_64 version" >&2
  exit 1
fi
if [[ ! $QEMU_VERSION_OUTPUT =~ [Vv]ersion[[:space:]]+([0-9]+)\.([0-9]+) ]]; then
  echo "unable to parse qemu-system-x86_64 version: $QEMU_VERSION_OUTPUT" >&2
  exit 1
fi
QEMU_MAJOR=${BASH_REMATCH[1]}
QEMU_MINOR=${BASH_REMATCH[2]}
QEMU_VERSION="$QEMU_MAJOR.$QEMU_MINOR"
host_flags=$(grep -m1 '^flags' /proc/cpuinfo || true)
KVM_USABLE=0
if [ -c /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ] && \
   [[ " $host_flags " == *' xsave '* ]] && [[ " $host_flags " == *' avx '* ]]; then
  KVM_USABLE=1
fi
QEMU_TCG_XSAVE_OK=1
if (( QEMU_MAJOR < 8 || (QEMU_MAJOR == 8 && QEMU_MINOR < 2) )); then
  if [ "${X86_BOUNDARY_ALLOW_KVM_ONLY:-0}" = "1" ] && [ "$KVM_USABLE" -eq 1 ]; then
    echo "QEMU $QEMU_VERSION is too old for the TCG XSAVE/AVX profile; running the explicitly requested KVM-only validation" >&2
    QEMU_TCG_XSAVE_OK=0
  else
    echo "QEMU >= 8.2 is required for the TCG boundary-probe XSAVE/AVX profile; older TCG versions (including 6.2) may omit this state (detected $QEMU_VERSION)" >&2
    if [ "$KVM_USABLE" -eq 1 ]; then
      echo "for complementary local validation only, X86_BOUNDARY_ALLOW_KVM_ONLY=1 runs the KVM/host profile without the TCG leg" >&2
    fi
    exit 1
  fi
fi
echo "Using QEMU $QEMU_VERSION for the x86 boundary-probe VM" >&2

KERNEL_RELEASE=$(cat "$KDIR/include/config/kernel.release")
test -n "$KERNEL_RELEASE" || { echo "kernel release metadata is missing from KDIR" >&2; exit 1; }
STAGE=$(mktemp -d)
trap 'rm -rf -- "$STAGE"' EXIT

if [ "$#" -eq 1 ]; then
  KERNEL_IMAGE_PACKAGE=
  for candidate in "linux-image-unsigned-$KERNEL_RELEASE" "linux-image-$KERNEL_RELEASE-unsigned"; do
    if apt-cache show "$candidate" 2>/dev/null | grep -F "Package: $candidate" >/dev/null; then
      KERNEL_IMAGE_PACKAGE=$candidate
      break
    fi
  done
  test -n "$KERNEL_IMAGE_PACKAGE" || {
    echo "no Debian/Ubuntu unsigned kernel image package found for $KERNEL_RELEASE; pass KERNEL_IMAGE and BUSYBOX_STATIC explicitly" >&2
    exit 1
  }

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
fi

mkdir -p "$STAGE/build/fki-ko" "$STAGE/build/tests/x86_64/boundary-probe"
cp -a "$REPO/fki-ko/." "$STAGE/build/fki-ko/"
cp -a "$REPO/tests/x86_64/boundary-probe/." "$STAGE/build/tests/x86_64/boundary-probe/"
make -C "$KDIR" ARCH=x86_64 M="$STAGE/build/fki-ko" CONFIG_DEBUG_INFO_BTF_MODULES= modules
make -C "$KDIR" ARCH=x86_64 M="$STAGE/build/tests/x86_64/boundary-probe" \
	KBUILD_EXTRA_SYMBOLS="$STAGE/build/fki-ko/Module.symvers" \
	CONFIG_DEBUG_INFO_BTF_MODULES= modules
gcc -O2 -Wall -Wextra -Werror -mavx -mxsave -static \
  -I"$REPO/tests/x86_64/boundary-probe" \
  -o "$STAGE/build/boundary-probe-test" "$REPO/tests/x86_64/boundary-probe/user_test.c"

ROOTFS="$STAGE/rootfs"
mkdir -p "$ROOTFS/bin" "$ROOTFS/dev" "$ROOTFS/proc" "$ROOTFS/sys"
install -m 0755 -- "$BUSYBOX" "$ROOTFS/bin/busybox"
printf '%s\n' "$KERNEL_RELEASE" > "$ROOTFS/expected-kernel-release"
install -m 0755 "$STAGE/build/boundary-probe-test" "$ROOTFS/boundary-probe-test"
install -m 0644 "$STAGE/build/fki-ko/fki.ko" "$ROOTFS/fki.ko"
install -m 0644 "$STAGE/build/tests/x86_64/boundary-probe/x86-boundary-probe.ko" \
  "$ROOTFS/x86-boundary-probe.ko"
cat > "$ROOTFS/init" <<'INIT'
#!/bin/busybox sh
set -eu
export PATH=/bin
/bin/busybox --install -s /bin
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mount -t proc proc /proc
mount -t sysfs sysfs /sys
console_level=
restore_console_level() {
  if [ -n "$console_level" ]; then
    dmesg -n "$console_level"
  fi
}
trap 'restore_console_level || true; echo "x86 boundary probe VM failed"; dmesg; poweroff -f' EXIT

expected_release=$(cat /expected-kernel-release)
actual_release=$(uname -r)
if [ "$actual_release" != "$expected_release" ]; then
  printf 'kernel release mismatch: headers %s, booted kernel %s\n' "$expected_release" "$actual_release" >&2
  exit 1
fi
insmod /fki.ko
insmod /x86-boundary-probe.ko
# Keep kernel info lines in the ring buffer while userspace prints its markers.
console_level=$(cut -f1 /proc/sys/kernel/printk)
case "$console_level" in
  ''|*[!0-9]*) echo "invalid print level in /proc/sys/kernel/printk" >&2; exit 1 ;;
esac
dmesg -n 5
/boundary-probe-test
dmesg | grep -F 'x86-boundary-probe:' || true
test ! -e /sys/module/xnu
rmmod x86_boundary_probe
test ! -e /sys/module/x86_boundary_probe
test ! -e /dev/x86-boundary-probe
rmmod fki
test ! -e /sys/module/fki
restore_console_level
trap - EXIT
echo X86_BOUNDARY_PROBE_VM_PASS
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
INITRD="$STAGE/boundary-probe-initramfs.cpio.gz"
(
  cd "$ROOTFS"
  find . -print0 | sort -z | cpio --null --create --format=newc --owner=0:0 2>/dev/null
) | gzip -n -9 > "$INITRD"

run_probe_vm() {
  local accel="$1"
  local cpu="$2"
  local log="$3"
  local qemu_status

  set +e
  timeout 300 qemu-system-x86_64 -machine q35 -accel "$accel" -cpu "$cpu" -smp 2 -m 1024 \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -append 'console=ttyS0,115200 rdinit=/init panic=1 loglevel=7' \
    -display none -serial stdio -monitor none -no-reboot > "$log" 2>&1
  qemu_status=$?
  set -e
  cat "$log"
  if [ "$qemu_status" -ne 0 ]; then
    echo "QEMU boundary-probe VM failed with status $qemu_status ($accel, cpu=$cpu)" >&2
    exit "$qemu_status"
  fi
  grep -Fq 'X86_BOUNDARY_PROBE_VM_PASS' "$log" || {
    echo "disposable VM did not report the boundary-probe success marker ($accel)" >&2
    exit 1
  }
  grep -Fq 'X86_FKI_TASK_CONTINUATION_PASS' "$log" || {
    echo "disposable VM did not report the task-continuation success marker ($accel)" >&2
    exit 1
  }
  grep -Fq 'reboot: Power down' "$log" || {
    echo "disposable VM did not power down cleanly ($accel)" >&2
    exit 1
  }
}

if [ "$QEMU_TCG_XSAVE_OK" -eq 1 ]; then
  run_probe_vm tcg max "$STAGE/tcg.log"
fi
if [ "$KVM_USABLE" -eq 1 ]; then
  run_probe_vm kvm host "$STAGE/kvm.log"
else
  echo 'skipping optional KVM boundary-probe run: KVM/XSAVE/AVX is unavailable'
fi
