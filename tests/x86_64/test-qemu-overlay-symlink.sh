#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
LAUNCHER="$ROOT_DIR/scripts/x86_64/xnulinux-boot.sh"
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

# Exercise managed-overlay rejection with qemu-img's real backing-file report,
# once through the state-directory link and once through its external target.
OVERLAY_META="${OVERLAY%.qcow2}.base"
printf 'version=1\nformat=raw\nstat=%s\nsha256=%s\n' \
  "$(stat -Lc '%d:%i:%s:%y:%z' -- "$BASE_IMAGE")" "$BASE_SHA256_BEFORE" >"$OVERLAY_META"
chmod 600 -- "$OVERLAY_META"
OVMF_CODE="$TMP_DIR/OVMF_CODE.fd"
OVMF_VARS="$TMP_DIR/OVMF_VARS.fd"
FAKE_QEMU="$TMP_DIR/qemu-system-x86_64"
REAL_LAUNCH_QEMU_LOG="$TMP_DIR/launcher-started-qemu"
SMC_KEY='0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'
printf 'firmware fixture\n' >"$OVMF_CODE"
printf 'firmware variables fixture\n' >"$OVMF_VARS"
cat >"$FAKE_QEMU" <<'FAKE_QEMU'
#!/usr/bin/env bash
set -Eeuo pipefail
if [[ "${1-}" == --version ]]; then
  printf 'QEMU emulator version 10.0.13\n'
  exit 0
fi
touch -- "$REAL_LAUNCH_QEMU_LOG"
FAKE_QEMU
chmod +x -- "$FAKE_QEMU"
for overlay_base in "$OVERLAY" "$EXTERNAL_OVERLAY"; do
  LAUNCH_OUTPUT="$TMP_DIR/launcher-$(basename -- "$overlay_base").out"
  if env \
    "QEMU=$FAKE_QEMU" \
    "QEMU_IMG=$QEMU_IMG" \
    "DISK=$overlay_base" \
    "DISK_FORMAT=qcow2" \
    "OVMF_CODE=$OVMF_CODE" \
    "OVMF_VARS_TEMPLATE=$OVMF_VARS" \
    "STATE_DIR=$STATE_DIR" \
    "APPLE_SMC_OSK=$SMC_KEY" \
    "ACCEL=tcg" \
    "REAL_LAUNCH_QEMU_LOG=$REAL_LAUNCH_QEMU_LOG" \
    "$LAUNCHER" >"$LAUNCH_OUTPUT" 2>&1; then
    cat "$LAUNCH_OUTPUT" >&2
    printf 'launcher accepted a managed overlay selected through %s\n' "$overlay_base" >&2
    exit 1
  fi
  grep -Fq 'DISK points to an xnulinux-managed overlay in STATE_DIR' "$LAUNCH_OUTPUT" || {
    cat "$LAUNCH_OUTPUT" >&2
    printf 'launcher did not recognize the real qcow2 overlay selected through %s\n' "$overlay_base" >&2
    exit 1
  }
  [[ ! -e "$REAL_LAUNCH_QEMU_LOG" ]] || { printf 'launcher started QEMU for a managed overlay\n' >&2; exit 1; }
done

# A user-prepared qcow2 base may use the reserved filename shape by coincidence.
# Without xnulinux's .base ownership sidecar, it remains a valid source image.
USER_STATE_DIR="$TMP_DIR/user source state"
USER_BASE="$USER_STATE_DIR/disk-aaaaaaaaaaaaaaaaaaaa.qcow2"
USER_QEMU_LOG="$TMP_DIR/user-source-started-qemu"
mkdir -p -- "$USER_STATE_DIR"
"$QEMU_IMG" create -q -f qcow2 "$USER_BASE" 1M
[[ ! -e "${USER_BASE%.qcow2}.base" ]] || { printf 'user qcow2 fixture unexpectedly has an ownership sidecar\n' >&2; exit 1; }
USER_BASE_SHA256_BEFORE="$(sha256sum -- "$USER_BASE" | awk '{print $1}')"
USER_LAUNCH_OUTPUT="$TMP_DIR/user-source-launch.out"
env \
  "QEMU=$FAKE_QEMU" \
  "QEMU_IMG=$QEMU_IMG" \
  "DISK=$USER_BASE" \
  "DISK_FORMAT=qcow2" \
  "OVMF_CODE=$OVMF_CODE" \
  "OVMF_VARS_TEMPLATE=$OVMF_VARS" \
  "STATE_DIR=$USER_STATE_DIR" \
  "APPLE_SMC_OSK=$SMC_KEY" \
  "ACCEL=tcg" \
  "REAL_LAUNCH_QEMU_LOG=$USER_QEMU_LOG" \
  "$LAUNCHER" >"$USER_LAUNCH_OUTPUT" 2>&1 || {
    cat "$USER_LAUNCH_OUTPUT" >&2
    printf 'launcher rejected an unowned user qcow2 base with the reserved filename shape\n' >&2
    exit 1
  }
[[ -e "$USER_QEMU_LOG" ]] || { cat "$USER_LAUNCH_OUTPUT" >&2; printf 'launcher did not start QEMU for the user base\n' >&2; exit 1; }
[[ "$(sha256sum -- "$USER_BASE" | awk '{print $1}')" == "$USER_BASE_SHA256_BEFORE" ]] \
  || { printf 'launcher changed the user qcow2 base\n' >&2; exit 1; }
USER_DISK_ID="$(printf '%s\0' "$USER_BASE" | sha256sum | awk '{print substr($1, 1, 20)}')"
[[ -f "$USER_STATE_DIR/disk-$USER_DISK_ID.base" ]] \
  || { printf 'launcher did not write an ownership sidecar for its new overlay\n' >&2; exit 1; }
printf 'PASS: symlinked managed overlays are rejected; an unowned user qcow2 base with the reserved filename shape is accepted and remains unchanged\n'
