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
for tool in apt-cache apt-get cat chmod cpio dpkg-deb find grep gzip install mkdir mktemp mknod qemu-system-x86_64 rm sort sudo timeout; do
  command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done
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
if (( QEMU_MAJOR < 8 || (QEMU_MAJOR == 8 && QEMU_MINOR < 2) )); then
  echo "QEMU >= 8.2 is required for the Haswell TCG AVX/FMA preflight; older TCG versions (including 6.2) may omit these flags (detected $QEMU_VERSION)" >&2
  exit 1
fi
echo "Using QEMU $QEMU_VERSION for the x86 fail-closed VM preflight" >&2
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
trap 'rm -rf "$STAGE"' EXIT

test -s "$REPO/xnu-ko/xnu.ko" || { echo "build xnu-ko/xnu.ko before running this test" >&2; exit 1; }

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

run_stub_vm() {
  local accel="$1"
  local cpu="$2"
  local log="$3"
  local extra_kernel_args="${4:-}"
  local kernel_args='console=ttyS0,115200 rdinit=/init panic=1 loglevel=7'

  if [[ -n "$extra_kernel_args" ]]; then
    kernel_args+=" $extra_kernel_args"
  fi

  set +e
  timeout 240 qemu-system-x86_64 -machine q35 -accel "$accel" -cpu "$cpu" -smp 2 -m 1024 -kernel "$KERNEL" -initrd "$INITRD" -append "$kernel_args" -display none -serial stdio -monitor none -no-reboot > "$log" 2>&1
  QEMU_RC=$?
  set -e
  cat "$log"
  if [ "$QEMU_RC" -ne 0 ]; then
    echo "QEMU disposable-VM test failed with status $QEMU_RC for CPU $cpu" >&2
    exit "$QEMU_RC"
  fi
  grep -F 'XNU_STUB_VM_TEST_PASS' "$log" >/dev/null || {
    echo "disposable VM did not report the xnu stub test marker for CPU $cpu" >&2
    exit 1
  }
}

host_has_required_cpu_features() {
  local flags feature

  flags=$(grep -m1 '^flags' /proc/cpuinfo)
  for feature in sse4_2 avx fma movbe popcnt bmi1 avx2 bmi2; do
    [[ " $flags " == *" $feature "* ]] || return 1
  done
  [[ " $flags " == *" lzcnt "* || " $flags " == *" abm "* ]]
}

if [[ -w /dev/kvm ]] && host_has_required_cpu_features; then
  HOST_LOG="$STAGE/qemu-serial-host-features.log"
  run_stub_vm kvm host "$HOST_LOG"
  grep -E 'xnu: (native x86_64 world switch is not implemented; refusing to load|pinned XNU source requires GenuineIntel|pinned XNU source does not recognize Intel family)' "$HOST_LOG" >/dev/null || {
    echo "feature-complete VM did not report either the fail-closed entry stub or source-pinned CPU rejection" >&2
    exit 1
  }
else
  echo 'skipping feature-complete CPU validation: KVM or a required host CPU feature is unavailable' >&2
fi

MISSING_LOG="$STAGE/qemu-serial-missing-features.log"
run_stub_vm tcg qemu64 "$MISSING_LOG"
grep -E 'xnu: (CPU is missing required x86_64 features:|pinned XNU source requires GenuineIntel|pinned XNU source does not recognize Intel family)' "$MISSING_LOG" >/dev/null || {
  echo "feature-limited disposable VM did not report a source-pinned CPU preflight rejection" >&2
  exit 1
}
if grep -F 'xnu: native x86_64 world switch is not implemented; refusing to load' "$MISSING_LOG" >/dev/null; then
  echo "feature-limited disposable VM reached the world-switch refusal before CPU validation" >&2
  exit 1
fi

AMD_LOG="$STAGE/qemu-serial-amd-vendor.log"
run_stub_vm tcg 'max,vendor=AuthenticAMD' "$AMD_LOG"
grep -F 'xnu: pinned XNU source requires GenuineIntel; this CPU vendor is unsupported' "$AMD_LOG" >/dev/null || {
  echo "AMD-vendor disposable VM did not report the pinned-source vendor refusal" >&2
  exit 1
}
if grep -E 'xnu: (CPU is missing required x86_64 features:|pinned XNU source does not recognize Intel family|native x86_64 world switch is not implemented; refusing to load)' "$AMD_LOG" >/dev/null; then
  echo "AMD-vendor disposable VM did not stop at the vendor preflight" >&2
  exit 1
fi

HASWELL_LOG="$STAGE/qemu-serial-haswell.log"
run_stub_vm tcg Haswell "$HASWELL_LOG"
grep -F 'xnu: native x86_64 world switch is not implemented; refusing to load' "$HASWELL_LOG" >/dev/null || {
  echo "feature-complete Haswell VM did not reach the world-switch refusal" >&2
  exit 1
}

AVX2_LOG="$STAGE/qemu-serial-avx2-disabled.log"
run_stub_vm tcg 'Haswell,-avx2' "$AVX2_LOG"
grep -F 'xnu: CPU is missing required x86_64 features: AVX2' "$AVX2_LOG" >/dev/null || {
  echo "Intel Haswell-profile VM did not identify the disabled AVX2 feature" >&2
  exit 1
}
if grep -F 'xnu: native x86_64 world switch is not implemented; refusing to load' "$AVX2_LOG" >/dev/null; then
  echo "Haswell profile reached the world-switch refusal with AVX2 disabled" >&2
  exit 1
fi

LINUX_DISABLED_LOG="$STAGE/qemu-serial-linux-disabled-avx2.log"
run_stub_vm tcg Haswell "$LINUX_DISABLED_LOG" 'clearcpuid=avx2'
grep -F 'xnu: CPU is missing required x86_64 features: AVX2' "$LINUX_DISABLED_LOG" >/dev/null || {
  echo "Linux clearcpuid=avx2 was not reflected by the module preflight" >&2
  exit 1
}
if grep -F 'xnu: native x86_64 world switch is not implemented; refusing to load' "$LINUX_DISABLED_LOG" >/dev/null; then
  echo "Haswell profile reached the world-switch refusal after Linux disabled AVX2" >&2
  exit 1
fi
