#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -Eeuo pipefail

fail() {
  printf 'TCG smoke: %s\n' "$*" >&2
  exit 2
}

usage() {
  printf 'Usage: %s KERNEL INITRD\n' "$(basename -- "$0")" >&2
  printf 'Boot a disposable x86_64 Linux guest with QEMU TCG and check its serial marker.\n' >&2
}

[[ "$#" == 2 ]] || { usage; fail 'exactly a kernel and initrd path are required'; }
KERNEL="$1"
INITRD="$2"
[[ -f "$KERNEL" && -r "$KERNEL" ]] || fail "kernel is not a readable file: $KERNEL"
[[ -f "$INITRD" && -r "$INITRD" ]] || fail "initrd is not a readable file: $INITRD"

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-90}"
[[ "$TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]] || fail 'TIMEOUT_SECONDS must be a positive integer'
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_PATH="$(command -v -- "$QEMU" 2>/dev/null || true)"
[[ -n "$QEMU_PATH" && -x "$QEMU_PATH" ]] || fail "QEMU executable not found: $QEMU"
command -v timeout >/dev/null 2>&1 || fail 'timeout command not found'

TMP_DIR="$(mktemp -d)" || fail 'could not create temporary log directory'
trap 'rm -rf -- "$TMP_DIR"' EXIT
SERIAL_LOG="$TMP_DIR/serial.log"

QEMU_ARGS=(
  -name xnulinux-tcg-smoke
  -machine q35
  -accel tcg
  -cpu max
  -smp 2
  -m 1024
  -kernel "$KERNEL"
  -initrd "$INITRD"
  -append 'console=ttyS0 rdinit=/init'
  -display none
  -serial stdio
  -monitor none
  -no-reboot
)

set +e
timeout --foreground --signal=TERM --kill-after=5s "${TIMEOUT_SECONDS}s" "$QEMU_PATH" "${QEMU_ARGS[@]}" >"$SERIAL_LOG" 2>&1
QEMU_STATUS=$?
set -e
cat -- "$SERIAL_LOG"

if [[ "$QEMU_STATUS" == 124 || "$QEMU_STATUS" == 137 ]]; then
  fail "QEMU timed out after ${TIMEOUT_SECONDS}s"
fi
[[ "$QEMU_STATUS" == 0 ]] || fail "QEMU exited with status $QEMU_STATUS"
grep -Fq 'XNU_LINUX_TCG_BOOTED' "$SERIAL_LOG" || fail 'guest did not print XNU_LINUX_TCG_BOOTED'

printf 'TCG smoke: disposable Linux guest printed XNU_LINUX_TCG_BOOTED. This is not a macOS, KVM, Intel, AMD, or performance result.\n'
