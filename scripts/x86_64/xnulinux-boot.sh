#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -Eeuo pipefail

fail() {
  printf 'xnulinux x86_64: %s\n' "$*" >&2
  exit 2
}

usage() {
  cat <<'USAGE'
Usage: xnulinux-boot.sh [--check]

Boot a user-prepared Tahoe 26 x86_64 disk in QEMU. KVM is the default;
ACCEL=tcg is available for development smoke tests only.

Required environment:
  DISK                 Bootable raw or qcow2 disk with EFI/OpenCore
  APPLE_SMC_OSK        Apple SMC OSK value required by QEMU

Common overrides:
  DISK_FORMAT=qcow2    Disk source format (raw or qcow2)
  OVMF_CODE            OVMF code firmware path (auto-detected when possible)
  OVMF_VARS_TEMPLATE   OVMF variables template path (auto-detected when possible)
  OVMF_DIR             Extra firmware directory searched before system paths
  STATE_DIR            Writable per-VM state directory
  CPU_MODEL            QEMU CPU model (default: host for KVM, max for TCG)
  CPUS=4 RAM_MB=8192
  QEMU_DISPLAY=gtk

--check validates dependencies and host capabilities without creating VM state.
Guest disk changes and OVMF variables are persisted separately from source files.
USAGE
}

CHECK_ONLY=0
case "${1-}" in
  "") ;;
  --check) CHECK_ONLY=1 ;;
  --help|-h) usage; exit 0 ;;
  *) usage >&2; fail "unknown argument: $1" ;;
esac
[[ "$#" -le 1 ]] || { usage >&2; fail 'too many arguments'; }

HOST_ARCH="${HOST_ARCH:-$(uname -m)}"
HOST_OS="$(uname -s)"
[[ "$HOST_OS" == Linux ]] || fail 'requires a Linux host'
[[ "$HOST_ARCH" == x86_64 ]] || fail "requires an x86_64 Linux host (found $HOST_ARCH)"

ACCEL="${ACCEL:-kvm}"
case "$ACCEL" in
  kvm) KVM_DEVICE="${KVM_DEVICE:-/dev/kvm}"; ACCEL_ARG="kvm,device=${KVM_DEVICE//,/,,}"; DEFAULT_CPU_MODEL=host ;;
  tcg) ACCEL_ARG=tcg; DEFAULT_CPU_MODEL=max ;;
  *) fail 'ACCEL must be kvm or tcg' ;;
esac
CPU_MODEL="${CPU_MODEL:-$DEFAULT_CPU_MODEL}"
CPUS="${CPUS:-4}"
RAM_MB="${RAM_MB:-8192}"
[[ "$CPUS" =~ ^[1-9][0-9]*$ ]] || fail 'CPUS must be a positive integer'
[[ "$RAM_MB" =~ ^[1-9][0-9]*$ ]] || fail 'RAM_MB must be a positive integer'

DISK="${DISK:-}"
DISK_FORMAT="${DISK_FORMAT:-qcow2}"
[[ -n "$DISK" && -f "$DISK" && -r "$DISK" ]] || fail 'DISK is not a readable file'
case "$DISK_FORMAT" in
  raw|qcow2) ;;
  *) fail 'DISK_FORMAT must be raw or qcow2' ;;
esac
DISK_PATH="$(realpath -e -- "$DISK")" || fail 'could not resolve DISK path'

OVMF_PAIR_SPECS=(
  'OVMF_CODE_4M.fd|OVMF_VARS_4M.fd'
  'OVMF_CODE_4M.qcow2|OVMF_VARS_4M.qcow2'
  'OVMF_CODE.4m.fd|OVMF_VARS.4m.fd'
  'OVMF_CODE.fd|OVMF_VARS.fd'
  'ovmf-x86_64-4m-code.bin|ovmf-x86_64-4m-vars.bin'
)
OVMF_SYSTEM_PAIRS=(
  '/usr/share/OVMF/OVMF_CODE_4M.fd|/usr/share/OVMF/OVMF_VARS_4M.fd'
  '/usr/share/edk2/x64/OVMF_CODE.4m.fd|/usr/share/edk2/x64/OVMF_VARS.4m.fd'
  '/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2|/usr/share/edk2/ovmf/OVMF_VARS_4M.qcow2'
  '/usr/share/edk2/ovmf-4m/OVMF_CODE.fd|/usr/share/edk2/ovmf-4m/OVMF_VARS.fd'
  '/usr/share/qemu/ovmf-x86_64-4m-code.bin|/usr/share/qemu/ovmf-x86_64-4m-vars.bin'
)

find_ovmf_pair_in_dir() {
  local dir="$1" spec code_name vars_name code vars
  [[ -d "$dir" ]] || return 1
  for spec in "${OVMF_PAIR_SPECS[@]}"; do
    code_name="${spec%%|*}"
    vars_name="${spec##*|}"
    code="$dir/$code_name"
    vars="$dir/$vars_name"
    if [[ -f "$code" && -r "$code" && -f "$vars" && -r "$vars" ]]; then
      printf '%s\n%s\n' "$code" "$vars"
      return 0
    fi
  done
  return 1
}

find_ovmf_pair() {
  local spec code vars
  if [[ -n "${OVMF_DIR:-}" ]] && find_ovmf_pair_in_dir "$OVMF_DIR"; then
    return 0
  fi
  for spec in "${OVMF_SYSTEM_PAIRS[@]}"; do
    code="${spec%%|*}"
    vars="${spec##*|}"
    if [[ -f "$code" && -r "$code" && -f "$vars" && -r "$vars" ]]; then
      printf '%s\n%s\n' "$code" "$vars"
      return 0
    fi
  done
  return 1
}

find_ovmf_counterpart() {
  local explicit="$1" wanted="$2" dir base spec code_name vars_name counterpart
  dir="$(dirname -- "$explicit")"
  base="${explicit##*/}"
  for spec in "${OVMF_PAIR_SPECS[@]}"; do
    code_name="${spec%%|*}"
    vars_name="${spec##*|}"
    if [[ "$wanted" == vars && "$base" == "$code_name" ]]; then
      counterpart="$dir/$vars_name"
    elif [[ "$wanted" == code && "$base" == "$vars_name" ]]; then
      counterpart="$dir/$code_name"
    else
      continue
    fi
    if [[ -f "$counterpart" && -r "$counterpart" ]]; then
      printf '%s\n' "$counterpart"
      return 0
    fi
    return 1
  done
  return 1
}

ovmf_firmware_format() {
  local magic
  magic="$(od -An -N4 -tx1 -- "$1" 2>/dev/null | tr -d ' \n')"
  if [[ "$magic" == 514649fb ]]; then
    printf 'qcow2\n'
  else
    printf 'raw\n'
  fi
}

OVMF_CODE="${OVMF_CODE:-}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-}"
if [[ -z "$OVMF_CODE" && -z "$OVMF_VARS_TEMPLATE" ]]; then
  OVMF_PAIR_OUTPUT="$(find_ovmf_pair)" \
    || fail 'could not find a readable 4 MiB OVMF firmware pair; install the distribution OVMF/edk2 package or set OVMF_CODE and OVMF_VARS_TEMPLATE'
  mapfile -t OVMF_PAIR <<<"$OVMF_PAIR_OUTPUT"
  OVMF_CODE="${OVMF_PAIR[0]}"
  OVMF_VARS_TEMPLATE="${OVMF_PAIR[1]}"
elif [[ -z "$OVMF_CODE" ]]; then
  OVMF_CODE="$(find_ovmf_counterpart "$OVMF_VARS_TEMPLATE" code)" \
    || fail 'OVMF_CODE must be set when OVMF_VARS_TEMPLATE does not use a recognized OVMF filename'
elif [[ -z "$OVMF_VARS_TEMPLATE" ]]; then
  OVMF_VARS_TEMPLATE="$(find_ovmf_counterpart "$OVMF_CODE" vars)" \
    || fail 'OVMF_VARS_TEMPLATE must be set when OVMF_CODE does not use a recognized OVMF filename'
fi
[[ -f "$OVMF_CODE" && -r "$OVMF_CODE" ]] || fail 'OVMF_CODE is not a readable file'
[[ -f "$OVMF_VARS_TEMPLATE" && -r "$OVMF_VARS_TEMPLATE" ]] || fail 'OVMF_VARS_TEMPLATE is not a readable file'
OVMF_CODE_FORMAT="$(ovmf_firmware_format "$OVMF_CODE")"

APPLE_SMC_OSK="${APPLE_SMC_OSK:-}"
[[ -n "$APPLE_SMC_OSK" ]] || fail 'APPLE_SMC_OSK must be set'
APPLE_SMC_OSK_BYTES="$(printf '%s' "$APPLE_SMC_OSK" | wc -c | tr -d '[:space:]')"
[[ "$APPLE_SMC_OSK_BYTES" == 64 ]] || fail 'APPLE_SMC_OSK must contain exactly 64 characters'

QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_PATH="$(command -v -- "$QEMU" 2>/dev/null || true)"
[[ -n "$QEMU_PATH" && -x "$QEMU_PATH" ]] || fail "QEMU executable not found: $QEMU"
if ! QEMU_VERSION="$("$QEMU_PATH" --version 2>&1 | sed -nE '1s/^QEMU emulator version ([0-9]+)(\.|$).*/\1/p')"; then
  fail 'could not run QEMU --version'
fi
[[ -n "$QEMU_VERSION" ]] || fail 'could not determine QEMU version'
(( QEMU_VERSION >= 10 )) || fail "QEMU 10 or newer is required (found major version $QEMU_VERSION)"

QEMU_IMG="${QEMU_IMG:-qemu-img}"
QEMU_IMG_PATH="$(command -v -- "$QEMU_IMG" 2>/dev/null || true)"
[[ -n "$QEMU_IMG_PATH" && -x "$QEMU_IMG_PATH" ]] || fail "qemu-img executable not found: $QEMU_IMG"
if ! QEMU_IMG_INFO="$("$QEMU_IMG_PATH" info -f "$DISK_FORMAT" "$DISK_PATH" 2>&1)"; then
  fail "qemu-img could not read DISK as $DISK_FORMAT"
fi
FLOCK="${FLOCK:-flock}"
FLOCK_PATH="$(command -v -- "$FLOCK" 2>/dev/null || true)"
[[ -n "$FLOCK_PATH" && -x "$FLOCK_PATH" ]] || fail "flock executable not found: $FLOCK (install util-linux)"

KVM_DEVICE="${KVM_DEVICE:-/dev/kvm}"
CPUINFO_FILE="${CPUINFO_FILE:-/proc/cpuinfo}"
if [[ "$ACCEL" == kvm ]]; then
  [[ -c "$KVM_DEVICE" && -r "$KVM_DEVICE" && -w "$KVM_DEVICE" ]] || fail "KVM device is missing or inaccessible: $KVM_DEVICE (enable VT-x/AMD-V and grant access to /dev/kvm)"
  [[ -r "$CPUINFO_FILE" ]] || fail "CPU information file is not readable: $CPUINFO_FILE"

  CPU_FEATURES="$(awk '
    /^[[:space:]]*flags[[:space:]]*:/ {
      line = $0
      sub(/^[^:]*:[[:space:]]*/, "", line)
      count = split(line, values, /[[:space:]]+/)
      delete present
      for (i = 1; i <= count; i++) present[values[i]] = 1
      if (cpus == 0) {
        for (feature in present) common[feature] = 1
      } else {
        for (feature in common) if (!(feature in present)) delete common[feature]
      }
      cpus++
    }
    END {
      if (cpus == 0) exit 2
      names[1] = "sse4_2:SSE4.2"
      names[2] = "avx:AVX"
      names[3] = "fma:FMA"
      names[4] = "movbe:MOVBE"
      names[5] = "popcnt:POPCNT"
      names[6] = "bmi1:BMI1"
      names[7] = "avx2:AVX2"
      names[8] = "bmi2:BMI2"
      for (i = 1; i <= 8; i++) {
        split(names[i], pair, ":")
        if (!(pair[1] in common)) missing = missing (missing ? " " : "") pair[2]
      }
      if (!("lzcnt" in common) && !("abm" in common)) missing = missing (missing ? " " : "") "LZCNT"
      if (!("vmx" in common) && !("svm" in common)) missing = missing (missing ? " " : "") "VT-x/AMD-V"
      print missing
    }
  ' "$CPUINFO_FILE")" || fail "could not read CPU flags from $CPUINFO_FILE"
  [[ -z "$CPU_FEATURES" ]] || fail "Missing required CPU features: $CPU_FEATURES"
fi

if [[ -n "${XDG_STATE_HOME:-}" ]]; then
  XDG_STATE_HOME="$XDG_STATE_HOME"
elif [[ -n "${HOME:-}" ]]; then
  XDG_STATE_HOME="$HOME/.local/state"
else
  fail 'HOME or XDG_STATE_HOME must be set'
fi
STATE_DIR="${STATE_DIR:-$XDG_STATE_HOME/xnulinux/x86_64}"
if [[ -e "$STATE_DIR" ]]; then
  [[ -d "$STATE_DIR" && -r "$STATE_DIR" && -w "$STATE_DIR" && -x "$STATE_DIR" ]] \
    || fail "STATE_DIR must be a readable, writable, and searchable directory: $STATE_DIR"
else
  STATE_PARENT="$STATE_DIR"
  while [[ ! -e "$STATE_PARENT" ]]; do
    STATE_PARENT="$(dirname -- "$STATE_PARENT")"
  done
  [[ -d "$STATE_PARENT" && -w "$STATE_PARENT" && -x "$STATE_PARENT" ]] || fail "cannot create STATE_DIR below: $STATE_PARENT"
fi

QEMU_DISPLAY="${QEMU_DISPLAY:-gtk}"
VARS_FILE="$STATE_DIR/OVMF_VARS.fd"
DISK_STAT="$(stat -Lc '%d:%i:%s:%y:%z' -- "$DISK_PATH")" || fail 'could not inspect DISK metadata'
DISK_ID="$(printf '%s\0' "$DISK_PATH" | sha256sum | awk '{print substr($1, 1, 20)}')"
DISK_OVERLAY="$STATE_DIR/disk-$DISK_ID.qcow2"
DISK_BASE_META="$STATE_DIR/disk-$DISK_ID.base"

printf 'xnulinux x86_64: Preflight OK (QEMU %s, accel=%s, cpu=%s).\n' "$QEMU_VERSION" "$ACCEL" "$CPU_MODEL"
if [[ "$ACCEL" == tcg ]]; then
  printf 'xnulinux x86_64: TCG is for development smoke tests; it is not a KVM support or performance result.\n'
fi
if [[ "$CHECK_ONLY" == 1 ]]; then
  exit 0
fi

umask 077
mkdir -p -- "$STATE_DIR" || fail "could not create STATE_DIR: $STATE_DIR"
if ! exec 9>"$STATE_DIR/.lock"; then
  fail "could not open STATE_DIR lock file: $STATE_DIR/.lock"
fi
"$FLOCK_PATH" -n 9 || fail "STATE_DIR is already in use by another QEMU process: $STATE_DIR"

STATE_DIR_REAL="$(realpath -e -- "$STATE_DIR")" || fail "could not resolve STATE_DIR: $STATE_DIR"
DISK_FILENAME="${DISK_PATH##*/}"
if [[ "$DISK_FORMAT" == qcow2 && "$(dirname -- "$DISK_PATH")" == "$STATE_DIR_REAL" \
    && "$DISK_FILENAME" =~ ^disk-[a-f0-9]{20}\.qcow2$ ]]; then
  SOURCE_OVERLAY_META="${DISK_PATH%.qcow2}.base"
  if [[ -f "$SOURCE_OVERLAY_META" && ! -L "$SOURCE_OVERLAY_META" ]] \
      && grep -Eq '^backing file:[[:space:]]+' <<<"$QEMU_IMG_INFO"; then
    fail "DISK points to an xnulinux-managed overlay in STATE_DIR: $DISK_PATH (use the original base image with this state directory, or choose a separate STATE_DIR to fork the image)"
  fi
fi

# This directory owns one VM's disk overlay. Catch legacy overlays from prior
# launcher versions even if their inode-based filename can no longer be
# reconstructed after the base image was restored or replaced.
for existing_overlay in "$STATE_DIR"/disk-*.qcow2; do
  [[ -e "$existing_overlay" || -L "$existing_overlay" ]] || continue
  [[ "${existing_overlay##*/}" =~ ^disk-[a-f0-9]{20}\.qcow2$ ]] || continue
  EXISTING_OVERLAY_REALPATH="$(realpath -e -- "$existing_overlay")" \
    || fail "could not resolve existing disk overlay: $existing_overlay"
  if [[ "$EXISTING_OVERLAY_REALPATH" == "$DISK_PATH" ]]; then
    EXISTING_OVERLAY_META="${existing_overlay%.qcow2}.base"
    if [[ "$DISK_FORMAT" == qcow2 && -f "$EXISTING_OVERLAY_META" && ! -L "$EXISTING_OVERLAY_META" ]] \
        && grep -Eq '^backing file:[[:space:]]+' <<<"$QEMU_IMG_INFO"; then
      fail "DISK points to an xnulinux-managed overlay in STATE_DIR: $existing_overlay (use the original base image with this state directory, or choose a separate STATE_DIR to fork the image)"
    fi
    [[ "$existing_overlay" != "$DISK_OVERLAY" ]] \
      || fail "DISK path collides with its overlay path: $DISK_PATH"
    continue
  fi
  if [[ "$existing_overlay" != "$DISK_OVERLAY" ]]; then
    fail "unassociated disk overlay exists in STATE_DIR: $existing_overlay (back it up and migrate deliberately, or use a new STATE_DIR to start fresh)"
  fi
done

if [[ -L "$DISK_OVERLAY" ]]; then
  [[ -f "$DISK_OVERLAY" ]] || fail "disk overlay symlink does not resolve to a regular file: $DISK_OVERLAY"
  DISK_OVERLAY_REALPATH="$(realpath -e -- "$DISK_OVERLAY")" \
    || fail "could not resolve disk overlay symlink: $DISK_OVERLAY"
  [[ "$DISK_OVERLAY_REALPATH" != "$DISK_PATH" ]] \
    || fail "disk overlay symlink resolves to DISK: $DISK_OVERLAY"
elif [[ -e "$DISK_OVERLAY" && ! -f "$DISK_OVERLAY" ]]; then
  fail "disk overlay is not a regular file: $DISK_OVERLAY"
fi

write_disk_base_metadata() {
  local disk_stat="$1" disk_sha256="$2" metadata_tmp
  metadata_tmp="$(mktemp "$STATE_DIR/.disk-$DISK_ID.base.XXXXXX")" || fail 'could not create temporary base metadata'
  if ! printf 'version=1\nformat=%s\nstat=%s\nsha256=%s\n' \
    "$DISK_FORMAT" "$disk_stat" "$disk_sha256" >"$metadata_tmp"; then
    rm -f -- "$metadata_tmp"
    fail 'could not write disk base metadata'
  fi
  chmod 600 -- "$metadata_tmp" || {
    rm -f -- "$metadata_tmp"
    fail 'could not secure disk base metadata'
  }
  if ! mv -f -- "$metadata_tmp" "$DISK_BASE_META"; then
    rm -f -- "$metadata_tmp"
    fail 'could not install disk base metadata'
  fi
}

if [[ -L "$DISK_BASE_META" || ( -e "$DISK_BASE_META" && ! -f "$DISK_BASE_META" ) ]]; then
  fail "disk base metadata is not a regular file: $DISK_BASE_META"
fi
if [[ -e "$DISK_OVERLAY" && ! -e "$DISK_BASE_META" ]]; then
  fail "existing overlay has no base identity metadata: $DISK_OVERLAY (back it up and migrate deliberately before continuing)"
fi

if [[ -e "$DISK_BASE_META" ]]; then
  [[ -r "$DISK_BASE_META" ]] || fail "disk base metadata is not readable: $DISK_BASE_META"
  BASE_META_VERSION=''
  BASE_META_FORMAT=''
  BASE_META_STAT=''
  BASE_META_SHA256=''
  while IFS='=' read -r metadata_key metadata_value; do
    case "$metadata_key" in
      version) BASE_META_VERSION="$metadata_value" ;;
      format) BASE_META_FORMAT="$metadata_value" ;;
      stat) BASE_META_STAT="$metadata_value" ;;
      sha256) BASE_META_SHA256="$metadata_value" ;;
    esac
  done <"$DISK_BASE_META"
  [[ "$BASE_META_VERSION" == 1 && "$BASE_META_FORMAT" == "$DISK_FORMAT" && -n "$BASE_META_STAT" && "$BASE_META_SHA256" =~ ^[a-f0-9]{64}$ ]] \
    || fail "disk base metadata is invalid or does not match DISK_FORMAT: $DISK_BASE_META"
  if [[ "$DISK_STAT" != "$BASE_META_STAT" ]]; then
    if ! CURRENT_DISK_SHA256="$(sha256sum -- "$DISK_PATH" | awk '{print $1}')"; then
      fail 'could not verify the restored DISK image'
    fi
    [[ "$CURRENT_DISK_SHA256" =~ ^[a-f0-9]{64}$ ]] || fail 'could not calculate a valid DISK checksum'
    [[ "$CURRENT_DISK_SHA256" == "$BASE_META_SHA256" ]] \
      || fail "DISK contents changed for the existing overlay: $DISK_OVERLAY (restore the matching base image or use a new STATE_DIR to start fresh)"
    write_disk_base_metadata "$DISK_STAT" "$BASE_META_SHA256"
  fi
else
  if ! BASE_DISK_SHA256="$(sha256sum -- "$DISK_PATH" | awk '{print $1}')"; then
    fail 'could not calculate the DISK checksum'
  fi
  [[ "$BASE_DISK_SHA256" =~ ^[a-f0-9]{64}$ ]] || fail 'could not calculate a valid DISK checksum'
  write_disk_base_metadata "$DISK_STAT" "$BASE_DISK_SHA256"
fi

if [[ ! -e "$VARS_FILE" ]]; then
  VARS_TMP="$VARS_FILE.tmp.$$"
  install -m 600 -- "$OVMF_VARS_TEMPLATE" "$VARS_TMP" || {
    rm -f -- "$VARS_TMP"
    fail 'could not initialize writable OVMF variables'
  }
  if ! mv -n -- "$VARS_TMP" "$VARS_FILE" 2>/dev/null && [[ ! -e "$VARS_FILE" ]]; then
    rm -f -- "$VARS_TMP"
    fail 'could not install writable OVMF variables'
  fi
  rm -f -- "$VARS_TMP"
fi
OVMF_VARS_FORMAT="$(ovmf_firmware_format "$VARS_FILE")"
if [[ ! -e "$DISK_OVERLAY" ]]; then
  OVERLAY_TMP="$DISK_OVERLAY.tmp.$$"
  if ! "$QEMU_IMG_PATH" create -q -f qcow2 -F "$DISK_FORMAT" -b "$DISK_PATH" "$OVERLAY_TMP"; then
    rm -f -- "$OVERLAY_TMP"
    fail 'could not create persistent qcow2 disk overlay'
  fi
  if ! mv -n -- "$OVERLAY_TMP" "$DISK_OVERLAY" 2>/dev/null && [[ ! -e "$DISK_OVERLAY" ]]; then
    rm -f -- "$OVERLAY_TMP"
    fail 'could not install persistent qcow2 disk overlay'
  fi
  rm -f -- "$OVERLAY_TMP"
fi

SMC_CONFIG="$(mktemp "$STATE_DIR/qemu-smc.XXXXXX")" || fail 'could not create temporary Apple SMC configuration'
SMC_OSK_CONFIG="${APPLE_SMC_OSK//\\/\\\\}"
SMC_OSK_CONFIG="${SMC_OSK_CONFIG//\"/\\\"}"
if ! printf '[device "xnulinux-applesmc"]\n  driver = "isa-applesmc"\n  osk = "%s"\n' "$SMC_OSK_CONFIG" >"$SMC_CONFIG"; then
  rm -f -- "$SMC_CONFIG"
  fail 'could not write temporary Apple SMC configuration'
fi
chmod 600 -- "$SMC_CONFIG" || {
  rm -f -- "$SMC_CONFIG"
  fail 'could not secure temporary Apple SMC configuration'
}
unset APPLE_SMC_OSK SMC_OSK_CONFIG
trap 'rm -f -- "$SMC_CONFIG"' EXIT

QEMU_ARGS=(
  -name xnulinux-tahoe26
  -machine q35
  -accel "$ACCEL_ARG"
  -cpu "$CPU_MODEL"
  -smp "$CPUS"
  -m "$RAM_MB"
  -drive "file=${OVMF_CODE//,/,,},if=pflash,format=${OVMF_CODE_FORMAT},readonly=on"
  -drive "file=${VARS_FILE//,/,,},if=pflash,format=${OVMF_VARS_FORMAT}"
  -readconfig "$SMC_CONFIG"
  -device ich9-ahci,id=sata
  -drive "file=${DISK_OVERLAY//,/,,},format=qcow2,if=none,id=disk0"
  -device ide-hd,drive=disk0,bus=sata.2
  -netdev user,id=net0
  -device e1000-82545em,netdev=net0
  -device qemu-xhci,id=xhci
  -device usb-kbd
  -device usb-tablet
  -vga std
  -display "$QEMU_DISPLAY"
  -monitor none
  -serial stdio
)

"$QEMU_PATH" "${QEMU_ARGS[@]}"
