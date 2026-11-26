#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -Eeuo pipefail

QEMU_IMG="${QEMU_IMG:-qemu-img}"
QEMU_IO="${QEMU_IO:-qemu-io}"
command -v -- "$QEMU_IMG" >/dev/null 2>&1 || { printf 'qemu-img executable not found: %s\n' "$QEMU_IMG" >&2; exit 1; }
command -v -- "$QEMU_IO" >/dev/null 2>&1 || { printf 'qemu-io executable not found: %s\n' "$QEMU_IO" >&2; exit 1; }

TMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "$TMP_DIR"' EXIT
BASE_IMAGE="$TMP_DIR/base.raw"
STATE_DIR="$TMP_DIR/vm state"
EXTERNAL_DIR="$TMP_DIR/external volume"
OVERLAY="$STATE_DIR/disk-0123456789abcdefabcd.qcow2"
EXTERNAL_OVERLAY="$EXTERNAL_DIR/guest overlay.qcow2"

mkdir -p -- "$STATE_DIR" "$EXTERNAL_DIR"
truncate -s 1M -- "$BASE_IMAGE"
"$QEMU_IMG" create -q -f qcow2 -F raw -b "$BASE_IMAGE" "$EXTERNAL_OVERLAY"
ln -s -- "$EXTERNAL_OVERLAY" "$OVERLAY"
BASE_SHA256_BEFORE="$(sha256sum -- "$BASE_IMAGE" | awk '{print $1}')"

# Exercise qcow2 open, write, close, and reopen through the same symlink the launcher uses.
"$QEMU_IMG" info -f qcow2 "$OVERLAY" >/dev/null
"$QEMU_IO" -f qcow2 -c 'write -P 0x5a 0 512' "$OVERLAY" >/dev/null
"$QEMU_IO" -f qcow2 -c 'read -P 0x5a 0 512' "$OVERLAY" >/dev/null
"$QEMU_IMG" check -f qcow2 "$OVERLAY" >/dev/null

[[ "$(sha256sum -- "$BASE_IMAGE" | awk '{print $1}')" == "$BASE_SHA256_BEFORE" ]] \
  || { printf 'qcow2 writes through the symlink changed the backing image\n' >&2; exit 1; }
printf 'PASS: QEMU opens and persists writes through a symlinked qcow2 overlay\n'
