#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build on the CI host; load every test module only inside a disposable QEMU VM.
set -euo pipefail

if { [ "$#" -ne 1 ] && [ "$#" -ne 3 ]; } || [ ! -f "$1/Makefile" ]; then
  echo "usage: $0 KDIR [KERNEL_IMAGE BUSYBOX_STATIC]" >&2
  exit 2
fi

KDIR=$(cd -- "$1" && pwd)
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
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
if (( QEMU_MAJOR < 8 || (QEMU_MAJOR == 8 && QEMU_MINOR < 2) )); then
  echo "QEMU >= 8.2 is required for the TCG XSAVE/AVX profile (detected $QEMU_MAJOR.$QEMU_MINOR)" >&2
  exit 1
fi

KERNEL_RELEASE=$(cat "$KDIR/include/config/kernel.release")
test -n "$KERNEL_RELEASE" || { echo "kernel release metadata is missing from KDIR" >&2; exit 1; }
STAGE=$(mktemp -d)
trap 'rm -rf -- "$STAGE"' EXIT

if [ "$#" -eq 1 ]; then
  KERNEL_IMAGE_PACKAGE=
  for candidate in "linux-image-unsigned-$KERNEL_RELEASE" "linux-image-$KERNEL_RELEASE-unsigned"; do
    if apt-cache show "$candidate" 2>/dev/null | grep -Fx "Package: $candidate" >/dev/null; then
      KERNEL_IMAGE_PACKAGE=$candidate
      break
    fi
  done
  test -n "$KERNEL_IMAGE_PACKAGE" || {
    echo "no unsigned Debian/Ubuntu kernel image package matches $KERNEL_RELEASE; pass KERNEL_IMAGE and BUSYBOX_STATIC explicitly" >&2
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

mkdir -p "$STAGE/build/fki-ko" "$STAGE/build/tests/x86_64/boundary-probe" \
  "$STAGE/build/tests/x86_64/exception-recovery"
cp -a "$REPO/fki-ko/." "$STAGE/build/fki-ko/"
cp -a "$REPO/tests/x86_64/boundary-probe/." \
  "$STAGE/build/tests/x86_64/boundary-probe/"
cp -a "$REPO/tests/x86_64/exception-recovery/." \
  "$STAGE/build/tests/x86_64/exception-recovery/"
make -C "$KDIR" ARCH=x86_64 M="$STAGE/build/fki-ko" CONFIG_DEBUG_INFO_BTF_MODULES= modules
make -C "$KDIR" ARCH=x86_64 M="$STAGE/build/tests/x86_64/boundary-probe" \
  KBUILD_EXTRA_SYMBOLS="$STAGE/build/fki-ko/Module.symvers" \
  CONFIG_DEBUG_INFO_BTF_MODULES= modules
make -C "$KDIR" ARCH=x86_64 M="$STAGE/build/tests/x86_64/exception-recovery" \
  KBUILD_EXTRA_SYMBOLS="$STAGE/build/fki-ko/Module.symvers" \
  CONFIG_DEBUG_INFO_BTF_MODULES= modules
gcc -O2 -Wall -Wextra -Werror -mavx -mxsave -static \
  -I"$REPO/tests/x86_64/boundary-probe" \
  -o "$STAGE/build/boundary-probe-test" \
  "$REPO/tests/x86_64/boundary-probe/user_test.c"
gcc -O2 -Wall -Wextra -Werror -static \
  -I"$REPO/tests/x86_64/exception-recovery" \
  -o "$STAGE/build/exception-recovery-test" \
  "$REPO/tests/x86_64/exception-recovery/user_test.c"

ROOTFS="$STAGE/rootfs"
mkdir -p "$ROOTFS/bin" "$ROOTFS/dev" "$ROOTFS/proc" "$ROOTFS/sys"
install -m 0755 -- "$BUSYBOX" "$ROOTFS/bin/busybox"
printf '%s\n' "$KERNEL_RELEASE" > "$ROOTFS/expected-kernel-release"
install -m 0755 "$STAGE/build/boundary-probe-test" "$ROOTFS/boundary-probe-test"
install -m 0755 "$STAGE/build/exception-recovery-test" "$ROOTFS/exception-recovery-test"
install -m 0644 "$STAGE/build/fki-ko/fki.ko" "$ROOTFS/fki.ko"
install -m 0644 "$STAGE/build/tests/x86_64/boundary-probe/x86-boundary-probe.ko" \
  "$ROOTFS/x86-boundary-probe.ko"
install -m 0644 "$STAGE/build/tests/x86_64/exception-recovery/x86-exception-recovery.ko" \
  "$ROOTFS/x86-exception-recovery.ko"
cat > "$ROOTFS/init" <<'INIT'
#!/bin/busybox sh
set -eu
export PATH=/bin
/bin/busybox --install -s /bin
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mount -t proc proc /proc
mount -t sysfs sysfs /sys
original_console_loglevel=
restore_console_loglevel() {
	if [ -n "$original_console_loglevel" ]; then
		dmesg -n "$original_console_loglevel"
	fi
}
failure_cleanup() {
	status=$?
	trap - EXIT
	if ! restore_console_loglevel; then
		echo "x86 exception recovery VM could not restore console log level" >&2
	fi
	echo "x86 exception recovery VM failed (status $status)" >&2
	if ! dmesg; then
		echo "x86 exception recovery VM could not dump the kernel ring buffer" >&2
	fi
	poweroff -f
	exit "$status"
}
trap failure_cleanup EXIT
expected_release=$(cat /expected-kernel-release)
actual_release=$(uname -r)
if [ "$actual_release" != "$expected_release" ]; then
	printf 'kernel release mismatch: headers %s, booted kernel %s\n' "$expected_release" "$actual_release" >&2
	exit 1
fi
# BusyBox read reads one byte at a time, which procfs sysctls may truncate.
original_console_loglevel=$(cut -f1 /proc/sys/kernel/printk)
test -n "$original_console_loglevel"

insmod /fki.ko
insmod /x86-boundary-probe.ko
/boundary-probe-test
rmmod x86_boundary_probe
test ! -e /sys/module/x86_boundary_probe
test ! -e /dev/x86-boundary-probe

insmod /x86-exception-recovery.ko
# Keep kernel info lines in the ring buffer so they cannot interrupt
# userspace marker lines on the shared serial console. Keep warnings visible.
dmesg -n 5
/exception-recovery-test 0
/exception-recovery-test 1
# Require every controlled recovery to have a successful kernel record before
# replaying the records to the serial console. This makes the diagnostics part
# of the disposable-VM pass condition rather than best-effort output.
recovery_diagnostics=/x86-exception-recovery-dmesg.log
probe_log=$(dmesg)
printf '%s\n' "$probe_log" > "$recovery_diagnostics"
if ! grep -Fq 'x86-exception-recovery:' "$recovery_diagnostics"; then
	echo "x86 exception recovery VM has no recovery diagnostics to replay" >&2
	exit 1
fi
for cpu in 0 1; do
	for vector in 14 13; do
		diagnostic_pattern="x86-exception-recovery: cpu=${cpu} vector=${vector} rip=0x[[:xdigit:]]+ error=0x[[:xdigit:]]+ cr2=0x[[:xdigit:]]+ recovery_count=1 idtr_restored=1 gprs=ok ds=0x[[:xdigit:]]+ stacks=ok"
		if ! grep -Eq "$diagnostic_pattern" "$recovery_diagnostics"; then
			echo "x86 exception recovery VM missed successful diagnostic for cpu=$cpu vector=$vector" >&2
			exit 1
		fi
	done
done
if grep -Eq 'x86-exception-recovery: (cpu=[0-9]+ vector=[0-9]+ failed status=|not-run)' \
	"$recovery_diagnostics"; then
	echo "x86 exception recovery VM ring buffer contains a failed or skipped probe" >&2
	exit 1
fi
grep -F 'x86-exception-recovery:' "$recovery_diagnostics"
restore_console_loglevel
rmmod x86_exception_recovery
test ! -e /sys/module/x86_exception_recovery
test ! -e /dev/x86-exception-recovery
test ! -e /sys/module/xnu
rmmod fki
test ! -e /sys/module/fki
trap - EXIT
echo X86_EXCEPTION_RECOVERY_VM_PASS
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
INITRD="$STAGE/exception-recovery-initramfs.cpio.gz"
(
  cd "$ROOTFS"
  find . -print0 | sort -z | cpio --null --create --format=newc --owner=0:0 2>/dev/null
) | gzip -n -9 > "$INITRD"

LOG="$STAGE/qemu.log"
set +e
timeout --kill-after=30 360 qemu-system-x86_64 -machine q35 -accel tcg -cpu max,+hypervisor \
  -smp 2 -m 1024 -kernel "$KERNEL" -initrd "$INITRD" \
  -append 'console=ttyS0,115200 rdinit=/init panic=1 loglevel=7' \
  -display none -serial stdio -monitor none -no-reboot \
  -device isa-debug-exit,iobase=0xf4,iosize=0x04 > "$LOG" 2>&1
QEMU_STATUS=$?
set -e
cat "$LOG"
if [ "$QEMU_STATUS" -ne 0 ]; then
  echo "disposable QEMU VM failed with status $QEMU_STATUS (terminal debug-exit and timeout are failures)" >&2
  exit "$QEMU_STATUS"
fi
if grep -Eiq 'Kernel panic|Oops:|BUG:|not syncing|Call Trace:' "$LOG"; then
  echo "disposable QEMU VM reported a kernel panic, oops, or bug" >&2
  exit 1
fi
for marker in \
  'X86_EXCEPTION_RECOVERY_CASE cpu=0 vector=14 rip=0x' \
  'X86_EXCEPTION_RECOVERY_CASE cpu=0 vector=13 rip=0x' \
  'X86_EXCEPTION_RECOVERY_PASS cpu=0 pf=passed gp=passed syscalls=passed' \
  'X86_EXCEPTION_RECOVERY_CASE cpu=1 vector=14 rip=0x' \
  'X86_EXCEPTION_RECOVERY_CASE cpu=1 vector=13 rip=0x' \
  'X86_EXCEPTION_RECOVERY_PASS cpu=1 pf=passed gp=passed syscalls=passed' \
  'X86_EXCEPTION_RECOVERY_VM_PASS' \
  'reboot: Power down'; do
  grep -Fq "$marker" "$LOG" || {
    echo "disposable VM missed required marker: $marker" >&2
    exit 1
  }
done
for cpu in 0 1; do
  for vector in 14 13; do
    diagnostic_pattern="x86-exception-recovery: cpu=${cpu} vector=${vector} rip=0x[[:xdigit:]]+ error=0x[[:xdigit:]]+ cr2=0x[[:xdigit:]]+ recovery_count=1 idtr_restored=1 gprs=ok ds=0x[[:xdigit:]]+ stacks=ok"
    if ! grep -Eq "$diagnostic_pattern" "$LOG"; then
      echo "disposable VM serial log missed successful diagnostic for cpu=$cpu vector=$vector" >&2
      exit 1
    fi
  done
done
