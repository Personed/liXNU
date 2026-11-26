#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
LAUNCHER="$ROOT_DIR/scripts/x86_64/xnulinux-boot.sh"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf -- "$TMP_DIR"' EXIT

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

assert_contains() {
  local file="$1" expected="$2"
  grep -Fq -- "$expected" "$file" || {
    cat "$file" >&2
    fail "expected output to contain: $expected"
  }
}

FIXTURE_DIR="$TMP_DIR/fixture with,comma"
BIN_DIR="$TMP_DIR/bin path,comma"
mkdir -p -- "$FIXTURE_DIR" "$BIN_DIR"

FAKE_QEMU="$BIN_DIR/qemu-system-x86_64"
FAKE_QEMU_IMG="$BIN_DIR/qemu-img"
cat >"$FAKE_QEMU" <<'FAKE_QEMU'
#!/usr/bin/env bash
set -Eeuo pipefail
if [[ "${1-}" == "--version" ]]; then
  printf 'QEMU emulator version 10.0.13\n'
  exit 0
fi
printf '%s\0' "$@" >"$QEMU_LOG"
SMC_CONFIG=''
for ((index = 1; index <= $#; index++)); do
  if [[ "${!index}" == -readconfig ]]; then
    next=$((index + 1))
    SMC_CONFIG="${!next}"
    break
  fi
done
[[ -n "$SMC_CONFIG" && -r "$SMC_CONFIG" ]] || { printf 'QEMU SMC config missing\n' >&2; exit 3; }
cat -- "$SMC_CONFIG" >"$QEMU_SMC_CONFIG_LOG"
stat -c '%a' -- "$SMC_CONFIG" >"$QEMU_SMC_CONFIG_MODE"
if [[ -v APPLE_SMC_OSK ]]; then
  printf 'present\n' >"$QEMU_OSK_ENV_LOG"
else
  printf 'absent\n' >"$QEMU_OSK_ENV_LOG"
fi
printf 'fake qemu invoked\n'
if [[ "${QEMU_HOLD_SECONDS:-0}" =~ ^[1-9][0-9]*$ ]]; then
  sleep "$QEMU_HOLD_SECONDS"
fi
FAKE_QEMU
cat >"$FAKE_QEMU_IMG" <<'FAKE_QEMU_IMG'
#!/usr/bin/env bash
set -Eeuo pipefail
if [[ "${1-}" == "--version" ]]; then
  printf 'qemu-img version 10.0.13\n'
  exit 0
fi
if [[ "${1-}" == info ]]; then
  exit 0
fi
count=0
if [[ -f "$QEMU_IMG_COUNT" ]]; then
  count="$(<"$QEMU_IMG_COUNT")"
fi
printf '%s\n' "$((count + 1))" >"$QEMU_IMG_COUNT"
printf '%s\n' "$@" >"$QEMU_IMG_LOG"
touch -- "${@: -1}"
if [[ "${1-}" == create && "${QEMU_IMG_FAIL_CREATE:-0}" == 1 ]]; then
  exit 1
fi
FAKE_QEMU_IMG
chmod +x -- "$FAKE_QEMU" "$FAKE_QEMU_IMG"

DISK="$FIXTURE_DIR/tahoe source.raw"
OVMF_CODE="$FIXTURE_DIR/OVMF code.fd"
OVMF_VARS_TEMPLATE="$FIXTURE_DIR/OVMF vars.fd"
CPUINFO_FULL="$FIXTURE_DIR/cpuinfo full"
CPUINFO_MISSING="$FIXTURE_DIR/cpuinfo missing"
STATE_DIR="$TMP_DIR/vm state,comma"
QEMU_LOG="$TMP_DIR/qemu args.bin"
QEMU_IMG_LOG="$TMP_DIR/qemu-img args.txt"
QEMU_IMG_COUNT="$TMP_DIR/qemu-img count"
QEMU_SMC_CONFIG_LOG="$TMP_DIR/qemu smc config.log"
QEMU_SMC_CONFIG_MODE="$TMP_DIR/qemu smc mode.log"
QEMU_OSK_ENV_LOG="$TMP_DIR/qemu osk environment.log"
KVM_STATE_DIR="$TMP_DIR/kvm vm state"
KVM_QEMU_IMG_COUNT="$TMP_DIR/kvm qemu-img count"
SMC_KEY='0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'

printf 'immutable source disk\0with binary-like bytes\n' >"$DISK"
printf 'ovmf code firmware\n' >"$OVMF_CODE"
printf 'ovmf variables template\n' >"$OVMF_VARS_TEMPLATE"
cat >"$CPUINFO_FULL" <<'CPUINFO'
processor : 0
flags : fpu vmx sse4_2 avx fma movbe popcnt lzcnt bmi1 avx2 bmi2
CPUINFO
cat >"$CPUINFO_MISSING" <<'CPUINFO'
processor : 0
flags : fpu vmx
CPUINFO

BASE_ENV=(
  "QEMU=$FAKE_QEMU"
  "QEMU_IMG=$FAKE_QEMU_IMG"
  "QEMU_LOG=$QEMU_LOG"
  "QEMU_IMG_LOG=$QEMU_IMG_LOG"
  "QEMU_IMG_COUNT=$QEMU_IMG_COUNT"
  "QEMU_SMC_CONFIG_LOG=$QEMU_SMC_CONFIG_LOG"
  "QEMU_SMC_CONFIG_MODE=$QEMU_SMC_CONFIG_MODE"
  "QEMU_OSK_ENV_LOG=$QEMU_OSK_ENV_LOG"
  "DISK=$DISK"
  "DISK_FORMAT=raw"
  "OVMF_CODE=$OVMF_CODE"
  "OVMF_VARS_TEMPLATE=$OVMF_VARS_TEMPLATE"
  "STATE_DIR=$STATE_DIR"
  "QEMU_DISPLAY=gtk,show-cursor=on with-space"
  "APPLE_SMC_OSK=$SMC_KEY"
  "HOST_ARCH=x86_64"
  "CPUINFO_FILE=$CPUINFO_FULL"
  "KVM_DEVICE=/dev/null"
)

expect_check_failure() {
  local name="$1" expected="$2"
  shift 2
  local output="$TMP_DIR/$name.out"
  if env "${BASE_ENV[@]}" ACCEL=tcg "$@" "$LAUNCHER" --check >"$output" 2>&1; then
    fail "$name unexpectedly passed preflight"
  fi
  assert_contains "$output" "$expected"
  [[ ! -e "$QEMU_LOG" ]] || fail "$name started QEMU"
}

[[ -x "$LAUNCHER" ]] || fail "launcher is missing or not executable: $LAUNCHER"

expect_check_failure missing_disk 'DISK is not a readable file' "DISK=$FIXTURE_DIR/missing disk.raw"
expect_check_failure missing_ovmf_code 'OVMF_CODE is not a readable file' "OVMF_CODE=$FIXTURE_DIR/missing code.fd"
expect_check_failure missing_ovmf_vars 'OVMF_VARS_TEMPLATE is not a readable file' "OVMF_VARS_TEMPLATE=$FIXTURE_DIR/missing vars.fd"
expect_check_failure missing_smc 'APPLE_SMC_OSK must be set' APPLE_SMC_OSK=
expect_check_failure invalid_smc 'APPLE_SMC_OSK must contain exactly 64 characters' APPLE_SMC_OSK=short
expect_check_failure invalid_disk_format 'DISK_FORMAT must be raw or qcow2' DISK_FORMAT=vmdk
expect_check_failure wrong_arch 'requires an x86_64 Linux host' HOST_ARCH=aarch64
expect_check_failure missing_qemu_img 'qemu-img executable not found' QEMU_IMG="$BIN_DIR/missing qemu-img"
expect_check_failure missing_flock 'flock executable not found' FLOCK="$BIN_DIR/missing flock"

UNREADABLE_STATE_DIR="$TMP_DIR/unreadable state directory"
mkdir -p -- "$UNREADABLE_STATE_DIR"
chmod 300 -- "$UNREADABLE_STATE_DIR"
expect_check_failure unreadable_state_dir 'STATE_DIR must be a readable, writable, and searchable directory' "STATE_DIR=$UNREADABLE_STATE_DIR"
chmod 700 -- "$UNREADABLE_STATE_DIR"

KVM_OUTPUT="$TMP_DIR/kvm-unavailable.out"
if env "${BASE_ENV[@]}" ACCEL=kvm KVM_DEVICE="$TMP_DIR/missing-kvm" "$LAUNCHER" --check >"$KVM_OUTPUT" 2>&1; then
  fail 'missing KVM device unexpectedly passed preflight'
fi
assert_contains "$KVM_OUTPUT" 'KVM device is missing or inaccessible'
[[ ! -e "$QEMU_LOG" ]] || fail 'missing KVM check started QEMU'

CPU_OUTPUT="$TMP_DIR/cpu-missing.out"
if env "${BASE_ENV[@]}" ACCEL=kvm CPUINFO_FILE="$CPUINFO_MISSING" "$LAUNCHER" --check >"$CPU_OUTPUT" 2>&1; then
  fail 'unsupported CPU feature set unexpectedly passed preflight'
fi
assert_contains "$CPU_OUTPUT" 'Missing required CPU features:'
assert_contains "$CPU_OUTPUT" 'SSE4.2'
for feature in AVX FMA MOVBE POPCNT LZCNT BMI1 AVX2 BMI2; do
  assert_contains "$CPU_OUTPUT" "$feature"
done
[[ ! -e "$QEMU_LOG" ]] || fail 'unsupported CPU check started QEMU'

STATE_FILE="$TMP_DIR/not-a-directory"
printf 'not a directory\n' >"$STATE_FILE"
expect_check_failure unusable_state_dir 'STATE_DIR must be a readable, writable, and searchable directory' "STATE_DIR=$STATE_FILE"

LEGACY_STATE_DIR="$TMP_DIR/legacy overlay state"
LEGACY_OVERLAY_OUTPUT="$TMP_DIR/legacy-overlay.out"
mkdir -p -- "$LEGACY_STATE_DIR"
printf 'unassociated saved disk state\n' >"$LEGACY_STATE_DIR/disk-0123456789abcdefabcd.qcow2"
if env "${BASE_ENV[@]}" STATE_DIR="$LEGACY_STATE_DIR" ACCEL=tcg "$LAUNCHER" >"$LEGACY_OVERLAY_OUTPUT" 2>&1; then
  fail 'unassociated legacy overlay unexpectedly started a fresh VM state'
fi
assert_contains "$LEGACY_OVERLAY_OUTPUT" 'unassociated disk overlay exists in STATE_DIR'
[[ ! -e "$QEMU_LOG" ]] || fail 'unassociated legacy overlay reached QEMU'

FAILED_STATE_DIR="$TMP_DIR/failed overlay state"
FAILED_OVERLAY_OUTPUT="$TMP_DIR/failed-overlay.out"
if env "${BASE_ENV[@]}" STATE_DIR="$FAILED_STATE_DIR" QEMU_IMG_FAIL_CREATE=1 ACCEL=tcg "$LAUNCHER" >"$FAILED_OVERLAY_OUTPUT" 2>&1; then
  fail 'failed qemu-img overlay creation unexpectedly launched QEMU'
fi
assert_contains "$FAILED_OVERLAY_OUTPUT" 'could not create persistent qcow2 disk overlay'
if find "$FAILED_STATE_DIR" -maxdepth 1 -type f \( -name '*.qcow2' -o -name '*.tmp.*' \) -print -quit | grep -Fq .; then
  fail 'failed overlay creation left a partial disk image behind'
fi
[[ ! -e "$QEMU_LOG" ]] || fail 'failed overlay creation started QEMU'

rm -f -- "$QEMU_IMG_COUNT"
CHECK_OUTPUT="$TMP_DIR/check.out"
env "${BASE_ENV[@]}" ACCEL=tcg "$LAUNCHER" --check >"$CHECK_OUTPUT" 2>&1 || {
  cat "$CHECK_OUTPUT" >&2
  fail 'valid TCG preflight failed'
}
assert_contains "$CHECK_OUTPUT" 'Preflight OK'
[[ ! -e "$STATE_DIR" ]] || fail '--check created the state directory'
[[ ! -e "$QEMU_LOG" ]] || fail '--check started QEMU'
[[ ! -e "$QEMU_IMG_COUNT" ]] || fail '--check created a disk overlay'

SOURCE_BEFORE="$(sha256sum -- "$DISK" | awk '{print $1}')"
LAUNCH_OUTPUT="$TMP_DIR/launch.out"
env "${BASE_ENV[@]}" ACCEL=tcg "$LAUNCHER" >"$LAUNCH_OUTPUT" 2>&1 || {
  cat "$LAUNCH_OUTPUT" >&2
  fail 'valid TCG launch failed'
}
[[ "$(<"$QEMU_IMG_COUNT")" == 1 ]] || fail 'launch did not create exactly one persistent disk overlay'
[[ -s "$QEMU_IMG_LOG" ]] || fail 'qemu-img was not given overlay arguments'
[[ -s "$QEMU_LOG" ]] || fail 'QEMU received no launch arguments'

VARS_FILE="$STATE_DIR/OVMF_VARS.fd"
[[ -s "$VARS_FILE" ]] || fail 'writable OVMF variables copy was not created'
cmp -s -- "$OVMF_VARS_TEMPLATE" "$VARS_FILE" || fail 'OVMF variables copy differs from its template'
printf 'persistent NVRAM state\n' >"$VARS_FILE"

python3 - "$QEMU_LOG" "$QEMU_IMG_LOG" "$SMC_KEY" "$STATE_DIR" <<'PY'
from pathlib import Path
import sys

qemu_log, img_log, smc_key, state_dir = sys.argv[1:]
args = Path(qemu_log).read_bytes().split(b"\0")
args = [arg.decode() for arg in args if arg]
img_args = Path(img_log).read_text().splitlines()
escaped_state_dir = state_dir.replace(",", ",,")

def value_after(option):
    index = args.index(option)
    return args[index + 1]

assert value_after("-machine") == "q35"
assert value_after("-accel") == "tcg"
assert value_after("-cpu") == "max"
assert value_after("-display") == "gtk,show-cursor=on with-space"
assert "-readconfig" in args
assert smc_key not in " ".join(args)
assert any(arg.startswith("file=") and "/disk-" in arg and arg.endswith(".qcow2,format=qcow2,if=none,id=disk0") for arg in args)
assert any(arg.startswith("file=") and "fixture with,,comma/OVMF code.fd" in arg and "if=pflash" in arg for arg in args)
assert any(arg.startswith("file=") and f"{escaped_state_dir}/OVMF_VARS.fd" in arg and "if=pflash" in arg for arg in args)
assert "e1000-82545em,netdev=net0" in args
assert "qemu-xhci,id=xhci" in args
assert "usb-kbd" in args and "usb-tablet" in args
assert value_after("-vga") == "std"
assert "qcow2" in img_args and "raw" in img_args
print("fake QEMU argument contract passed")
PY

grep -Fq "osk = \"$SMC_KEY\"" "$QEMU_SMC_CONFIG_LOG" || fail 'temporary SMC config did not contain the configured key'
[[ "$(<"$QEMU_SMC_CONFIG_MODE")" == 600 ]] || fail 'temporary SMC config permissions were not 0600'
[[ "$(<"$QEMU_OSK_ENV_LOG")" == absent ]] || fail 'Apple SMC key was inherited by the QEMU process environment'
! compgen -G "$STATE_DIR/qemu-smc.*" >/dev/null || fail 'temporary SMC config was not removed after QEMU exited'

[[ "$SOURCE_BEFORE" == "$(sha256sum -- "$DISK" | awk '{print $1}')" ]] || fail 'source disk was modified'

rm -f -- "$QEMU_LOG"
env "${BASE_ENV[@]}" ACCEL=tcg QEMU_HOLD_SECONDS=3 "$LAUNCHER" >"$TMP_DIR/launch-held.out" 2>&1 &
HELD_LAUNCH_PID=$!
for attempt in {1..50}; do
  [[ -s "$QEMU_LOG" ]] && break
  sleep 0.1
done
[[ -s "$QEMU_LOG" ]] || {
  kill "$HELD_LAUNCH_PID" 2>/dev/null || true
  wait "$HELD_LAUNCH_PID" 2>/dev/null || true
  fail 'first process did not reach the QEMU handoff for the state-lock check'
}
ARGS_BEFORE_LOCKED_RETRY="$(sha256sum -- "$QEMU_LOG" | awk '{print $1}')"
LOCKED_OUTPUT="$TMP_DIR/locked-state.out"
if env "${BASE_ENV[@]}" ACCEL=tcg QEMU_DISPLAY=none "$LAUNCHER" >"$LOCKED_OUTPUT" 2>&1; then
  kill "$HELD_LAUNCH_PID" 2>/dev/null || true
  wait "$HELD_LAUNCH_PID" 2>/dev/null || true
  fail 'second launch reused writable state while the first QEMU was still running'
fi
assert_contains "$LOCKED_OUTPUT" 'STATE_DIR is already in use'
[[ "$(sha256sum -- "$QEMU_LOG" | awk '{print $1}')" == "$ARGS_BEFORE_LOCKED_RETRY" ]] || fail 'second launch reached QEMU despite the state lock'
wait "$HELD_LAUNCH_PID" || {
  cat "$TMP_DIR/launch-held.out" >&2
  fail 'first process failed while holding the state lock'
}

env "${BASE_ENV[@]}" ACCEL=tcg "$LAUNCHER" >"$TMP_DIR/launch-second.out" 2>&1 || {
  cat "$TMP_DIR/launch-second.out" >&2
  fail 'second TCG launch failed'
}
[[ "$(<"$VARS_FILE")" == 'persistent NVRAM state' ]] || fail 'OVMF variables state was overwritten'
[[ "$(<"$QEMU_IMG_COUNT")" == 1 ]] || fail 'existing overlay was recreated on the second launch'
[[ "$(<"$LAUNCH_OUTPUT")" != *"$SMC_KEY"* ]] || fail 'launcher printed the Apple SMC key'

# Replacing the source file with byte-identical content must retain the same
# persistent overlay even though the restored file has a different inode.
RESTORED_DISK="$TMP_DIR/restored disk.raw"
cp -p -- "$DISK" "$RESTORED_DISK"
mv -f -- "$RESTORED_DISK" "$DISK"
env "${BASE_ENV[@]}" ACCEL=tcg "$LAUNCHER" >"$TMP_DIR/launch-restored-base.out" 2>&1 || {
  cat "$TMP_DIR/launch-restored-base.out" >&2
  fail 'byte-identical restored base image was not accepted'
}
[[ "$(<"$QEMU_IMG_COUNT")" == 1 ]] || fail 'byte-identical restored base caused a new overlay to be created'

# KVM gets an actual argument-construction check with injected, readable
# fixtures; this does not claim hardware virtualization support.
KVM_OUTPUT="$TMP_DIR/kvm-launch.out"
env "${BASE_ENV[@]}" ACCEL=kvm STATE_DIR="$KVM_STATE_DIR" QEMU_IMG_COUNT="$KVM_QEMU_IMG_COUNT" "$LAUNCHER" >"$KVM_OUTPUT" 2>&1 || {
  cat "$KVM_OUTPUT" >&2
  fail 'valid KVM launcher argument construction failed'
}
python3 - "$QEMU_LOG" <<'PY'
from pathlib import Path
import sys

args = [arg.decode() for arg in Path(sys.argv[1]).read_bytes().split(b"\0") if arg]
assert args[args.index("-accel") + 1] == "kvm,device=/dev/null"
assert args[args.index("-cpu") + 1] == "host"
print("fake KVM argument contract passed")
PY

# A changed base is a hard error before the guest starts, preserving the
# existing overlay and making the recovery choice explicit.
printf 'changed base image content\n' >"$DISK"
QEMU_LOG_BEFORE_CHANGED_BASE="$(sha256sum -- "$QEMU_LOG" | awk '{print $1}')"
CHANGED_BASE_OUTPUT="$TMP_DIR/changed-base.out"
if env "${BASE_ENV[@]}" ACCEL=tcg "$LAUNCHER" >"$CHANGED_BASE_OUTPUT" 2>&1; then
  fail 'changed base image unexpectedly reused the existing overlay'
fi
assert_contains "$CHANGED_BASE_OUTPUT" 'DISK contents changed for the existing overlay'
[[ "$(sha256sum -- "$QEMU_LOG" | awk '{print $1}')" == "$QEMU_LOG_BEFORE_CHANGED_BASE" ]] || fail 'changed base reached QEMU'
[[ "$(<"$QEMU_IMG_COUNT")" == 1 ]] || fail 'changed base created an additional overlay'

# A user disk can be stored inside its state directory and may even have a
# hash-shaped disk-*.qcow2 filename; it must not be mistaken for an old overlay.
SOURCE_STATE_DIR="$TMP_DIR/source disk state"
SOURCE_DISK_IN_STATE="$SOURCE_STATE_DIR/disk-0123456789abcdefabcd.qcow2"
SOURCE_STATE_IMG_COUNT="$TMP_DIR/source state qemu-img count"
mkdir -p -- "$SOURCE_STATE_DIR"
printf 'source image stored next to VM state\n' >"$SOURCE_DISK_IN_STATE"
env "${BASE_ENV[@]}" DISK="$SOURCE_DISK_IN_STATE" STATE_DIR="$SOURCE_STATE_DIR" QEMU_IMG_COUNT="$SOURCE_STATE_IMG_COUNT" ACCEL=tcg \
  "$LAUNCHER" >"$TMP_DIR/source-in-state.out" 2>&1 || {
  cat "$TMP_DIR/source-in-state.out" >&2
  fail 'source disk inside STATE_DIR was mistaken for a legacy overlay'
}
[[ "$(<"$SOURCE_STATE_IMG_COUNT")" == 1 ]] || fail 'source disk inside STATE_DIR did not receive a separate overlay'

printf 'PASS: launcher preflight, TCG/KVM arguments, protected SMC handling, and persistent disk identity\n'
