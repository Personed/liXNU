#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
SMOKE="$ROOT_DIR/tests/x86_64/tcg-smoke.sh"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "$TMP_DIR"' EXIT

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

KERNEL="$TMP_DIR/vmlinuz"
INITRD="$TMP_DIR/initrd.cpio.gz"
FAKE_QEMU="$TMP_DIR/fake-qemu"
ARGS_LOG="$TMP_DIR/qemu-args.bin"
printf 'kernel fixture\n' >"$KERNEL"
printf 'initramfs fixture\n' >"$INITRD"
cat >"$FAKE_QEMU" <<'FAKE_QEMU'
#!/usr/bin/env bash
set -Eeuo pipefail
printf '%s\0' "$@" >"$QEMU_ARGS_LOG"
case "${FAKE_QEMU_MODE:-marker}" in
  marker) printf 'XNU_LINUX_TCG_BOOTED\nreboot: Power down\n' ;;
  no-marker) printf 'guest started without the completion marker\n' ;;
  reboot) printf 'XNU_LINUX_TCG_BOOTED\nRestarting system\n' ;;
  hang) sleep 10 ;;
esac
FAKE_QEMU
chmod +x -- "$FAKE_QEMU"

[[ -x "$SMOKE" ]] || fail "TCG smoke script is missing or not executable: $SMOKE"

MISSING_OUTPUT="$TMP_DIR/missing.out"
if QEMU="$FAKE_QEMU" "$SMOKE" >"$MISSING_OUTPUT" 2>&1; then
  fail 'missing kernel/initrd unexpectedly passed'
fi
grep -Fq 'Usage:' "$MISSING_OUTPUT" || fail 'missing paths did not print usage'

if QEMU="$FAKE_QEMU" "$SMOKE" "$TMP_DIR/missing-kernel" "$INITRD" >"$MISSING_OUTPUT" 2>&1; then
  fail 'missing kernel path unexpectedly passed'
fi
grep -Fq 'kernel is not a readable file' "$MISSING_OUTPUT" || fail 'missing kernel path error is unclear'

if QEMU="$FAKE_QEMU" "$SMOKE" "$KERNEL" "$TMP_DIR/missing-initrd" >"$MISSING_OUTPUT" 2>&1; then
  fail 'missing initrd path unexpectedly passed'
fi
grep -Fq 'initrd is not a readable file' "$MISSING_OUTPUT" || fail 'missing initrd path error is unclear'

SUCCESS_OUTPUT="$TMP_DIR/success.out"
QEMU="$FAKE_QEMU" QEMU_ARGS_LOG="$ARGS_LOG" "$SMOKE" "$KERNEL" "$INITRD" >"$SUCCESS_OUTPUT" 2>&1 || {
  cat "$SUCCESS_OUTPUT" >&2
  fail 'marker-producing guest was not accepted'
}
grep -Fq 'XNU_LINUX_TCG_BOOTED' "$SUCCESS_OUTPUT" || fail 'success output lost the guest marker'
grep -Fq 'reboot: Power down' "$SUCCESS_OUTPUT" || fail 'success output lost the shutdown marker'

python3 - "$ARGS_LOG" "$KERNEL" "$INITRD" <<'PY'
from pathlib import Path
import sys

args = [arg.decode() for arg in Path(sys.argv[1]).read_bytes().split(b"\0") if arg]
assert args[args.index("-accel") + 1] == "tcg"
assert args[args.index("-cpu") + 1] == "max"
assert args[args.index("-kernel") + 1] == sys.argv[2]
assert args[args.index("-initrd") + 1] == sys.argv[3]
assert "-serial" in args and args[args.index("-serial") + 1] == "stdio"
PY

NO_MARKER_OUTPUT="$TMP_DIR/no-marker.out"
if QEMU="$FAKE_QEMU" QEMU_ARGS_LOG="$ARGS_LOG" FAKE_QEMU_MODE=no-marker "$SMOKE" "$KERNEL" "$INITRD" >"$NO_MARKER_OUTPUT" 2>&1; then
  fail 'guest without completion marker unexpectedly passed'
fi
grep -Fq 'did not print XNU_LINUX_TCG_BOOTED' "$NO_MARKER_OUTPUT" || fail 'missing marker failure is unclear'

REBOOT_OUTPUT="$TMP_DIR/reboot.out"
if QEMU="$FAKE_QEMU" QEMU_ARGS_LOG="$ARGS_LOG" FAKE_QEMU_MODE=reboot "$SMOKE" "$KERNEL" "$INITRD" >"$REBOOT_OUTPUT" 2>&1; then
  fail 'rebooting guest unexpectedly passed the shutdown check'
fi
grep -Fq 'attempted to reboot instead of powering down' "$REBOOT_OUTPUT" || fail 'guest reboot failure is unclear'

TIMEOUT_OUTPUT="$TMP_DIR/timeout.out"
if QEMU="$FAKE_QEMU" QEMU_ARGS_LOG="$ARGS_LOG" FAKE_QEMU_MODE=hang TIMEOUT_SECONDS=1 "$SMOKE" "$KERNEL" "$INITRD" >"$TIMEOUT_OUTPUT" 2>&1; then
  fail 'hanging guest unexpectedly passed the bounded smoke test'
fi
grep -Fq 'timed out after 1s' "$TIMEOUT_OUTPUT" || fail 'timeout failure is unclear'

printf 'PASS: TCG smoke validation and bounded execution\n'
