#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Exercise CR3 writes exclusively inside a disposable QEMU TCG guest.
set -euo pipefail

if [ "$#" -ne 3 ] || [ ! -f "$1/Makefile" ] || [ ! -r "$2" ] || [ ! -x "$3" ]; then
	echo "usage: $0 KDIR KERNEL_IMAGE BUSYBOX_STATIC" >&2
	exit 2
fi

KDIR=$(cd -- "$1" && pwd)
KERNEL=$2
BUSYBOX=$3
REPO=$(cd "$(dirname "$0")/../.." && pwd)
release=$(cat "$KDIR/include/config/kernel.release")
test -n "$release"
stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
build="$stage/build/tests/x86_64/cr3-switch"
mkdir -p "$build" "$stage/build/fki-ko"
cp -a "$REPO/tests/x86_64/cr3-switch/." "$build/"
cp "$REPO/fki-ko/fki.h" "$REPO/fki-ko/fki_x86_64_cr3.h" "$stage/build/fki-ko/"
make -C "$KDIR" ARCH=x86_64 M="$build" CONFIG_DEBUG_INFO_BTF_MODULES= modules

cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
set -eu
export PATH=/bin
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
cleanup() {
	echo X86_CR3_SWITCH_VM_FAIL
	dmesg
	poweroff -f
}
trap cleanup EXIT
test "$(uname -r)" = "$(cat /expected-release)"
if grep -qw cr3_probe_reject /proc/cmdline; then
	if insmod /x86-cr3-switch.ko; then
		exit 1
	fi
	dmesg | grep -F 'x86-cr3-switch: requires exactly one possible CPU'
	marker=X86_CR3_SWITCH_VM_REJECT_PASS
else
	insmod /x86-cr3-switch.ko
	cat /proc/uptime >/dev/null
	rmmod x86_cr3_switch
	marker=X86_CR3_SWITCH_VM_PASS
fi
test ! -e /sys/module/x86_cr3_switch
trap - EXIT
echo "$marker"
poweroff -f
INIT
python3 "$build/make_initramfs.py" "$stage/initramfs.cpio.gz" "$stage/init" \
	"$BUSYBOX" "$build/x86-cr3-switch.ko" "$release"

run_vm() {
	local cpus=$1 extra_args=$2 marker=$3 log="$stage/qemu-$1.log"
	if ! timeout 120 qemu-system-x86_64 -machine q35 -accel tcg \
	    -cpu max,+hypervisor,-pcid,-la57,tcg-cpuid=on -smp "$cpus" -m 512 \
	    -nodefaults -kernel "$KERNEL" -initrd "$stage/initramfs.cpio.gz" \
	    -append "console=ttyS0,115200 rdinit=/init panic=1 pti=off nmi_watchdog=0 loglevel=7 $extra_args" \
	    -display none -serial stdio -monitor none -no-reboot >"$log" 2>&1; then
		cat "$log" >&2
		return 1
	fi
	cat "$log"
	grep -Fq "$marker" "$log"
	grep -Fq 'reboot: Power down' "$log"
	if grep -Eq 'Kernel panic|Oops:|BUG:|X86_CR3_SWITCH_VM_FAIL' "$log"; then
		return 1
	fi
}

run_vm 1 "" X86_CR3_SWITCH_VM_PASS
grep -Fq 'x86-cr3-switch: return verified' "$stage/qemu-1.log"
grep -Fq 'x86-cr3-switch: abort verified' "$stage/qemu-1.log"
run_vm 2 cr3_probe_reject X86_CR3_SWITCH_VM_REJECT_PASS
if grep -Eq 'x86-cr3-switch: (return|abort) verified' "$stage/qemu-2.log"; then
	exit 1
fi
