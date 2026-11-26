#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -Eeuo pipefail

fail() {
  printf 'xnu x86_64 build: %s\n' "$*" >&2
  exit 2
}

usage() {
  cat <<'USAGE'
Usage: build-xnu-kernel.sh [--check]

Build the pinned Tahoe 26 XNU source as an x86_64 macOS development kernel.
The build uses an Xcode toolchain and the public, checksum-pinned macOS 26.1
SDK. It does not use or modify a macOS system disk.

Environment:
  XNU_X86_BUILD_DIR  Build/download directory (default: ${TMPDIR:-/tmp}/xnulinux-x86_64-xnu-build)
  XNU_X86_JOBS       Parallel jobs (default: logical CPU count)

--check validates the source pin, SDK, Xcode tools, and x86_64 target without
downloading or building. The default action builds kernel.development and
writes build-result.json next to it.
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

readonly XNU_URL='https://github.com/apple-oss-distributions/xnu.git'
readonly XNU_COMMIT='ac9718fb1af618d5ce8678d0dc6e8a58f252216f'
readonly XNU_TAG='xnu-12377.121.6'
readonly KERNEL_VERSION='25.5.0'
readonly SOURCE_VERSION='12377.121.6'
readonly SDK_VERSION='26.1'
readonly XCODE_VERSION='26.1.1'
readonly SDK_URL='https://github.com/joseluisq/macosx-sdks/releases/download/26.1/MacOSX26.1.sdk.tar.xz'
readonly SDK_SHA256='beee7212d265a6d2867d0236cc069314b38d5fb3486a6515734e76fa210c784c'

HOST_OS="$(uname -s)"
HOST_ARCH="$(uname -m)"
[[ "$HOST_OS" == Darwin ]] || fail 'the pinned Apple build tools require a macOS build host'
[[ "$HOST_ARCH" == x86_64 ]] || fail "run this x86_64 artifact build on an Intel macOS host (found $HOST_ARCH)"

for tool in git make curl tar shasum xcrun xcodebuild sysctl python3; do
  command -v "$tool" >/dev/null 2>&1 || fail "required host tool not found: $tool"
done

DEFAULT_TMPDIR="${TMPDIR:-/tmp}"
BUILD_ROOT="${XNU_X86_BUILD_DIR:-${DEFAULT_TMPDIR%/}/xnulinux-x86_64-xnu-build}"
SOURCE_DIR="$BUILD_ROOT/source"
SDK_CACHE="$BUILD_ROOT/cache/MacOSX26.1.sdk.tar.xz"
SDK_PARENT="$BUILD_ROOT/sdk"
SDK_DIR="$SDK_PARENT/MacOSX26.1.sdk"
OUT_DIR="$BUILD_ROOT/out"
OBJROOT="$OUT_DIR/obj"
SYMROOT="$OUT_DIR/sym"
DSTROOT="$OUT_DIR/dst"
LOG="$OUT_DIR/xnu-build.log"
RESULT="$OUT_DIR/build-result.json"

verify_source() {
  [[ -d "$SOURCE_DIR/.git" || -f "$SOURCE_DIR/.git" ]] || fail "missing pinned XNU source at $SOURCE_DIR"
  local actual
  actual="$(git -C "$SOURCE_DIR" rev-parse HEAD)"
  [[ "$actual" == "$XNU_COMMIT" ]] || fail "XNU source is $actual; expected $XNU_COMMIT ($XNU_TAG)"
  [[ -z "$(git -C "$SOURCE_DIR" status --porcelain)" ]] || fail 'refusing to build from a modified XNU source tree'
  grep -q '^SUPPORTED_ARCH_CONFIGS.*X86_64' "$SOURCE_DIR/makedefs/MakeInc.def" \
    || fail 'pinned XNU source does not declare X86_64'
  grep -q '^SUPPORTED_X86_64_MACHINE_CONFIGS[[:space:]]*=[[:space:]]*NONE$' "$SOURCE_DIR/makedefs/MakeInc.def" \
    || fail 'pinned XNU source does not declare the X86_64/NONE machine configuration'
  [[ -f "$SOURCE_DIR/config/MASTER.x86_64" ]] || fail 'pinned XNU source is missing config/MASTER.x86_64'
}

verify_sdk() {
  [[ -f "$SDK_DIR/SDKSettings.json" ]] || fail "missing extracted macOS $SDK_VERSION SDK at $SDK_DIR"
  local version
  version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["Version"])' "$SDK_DIR/SDKSettings.json")"
  [[ "$version" == "$SDK_VERSION" ]] || fail "SDK at $SDK_DIR is $version; expected $SDK_VERSION"
}

if (( CHECK_ONLY )); then
  verify_source
  verify_sdk
  actual_xcode="$(xcodebuild -version | awk '/^Xcode / { print $2; exit }')"
  [[ "$actual_xcode" == "$XCODE_VERSION" ]] || fail "Xcode is $actual_xcode; expected $XCODE_VERSION"
  xcodebuild -version
  xcrun --find clang >/dev/null
  xcrun --find ld >/dev/null
  xcrun --find iig >/dev/null
  printf 'int xnu_x86_64_target_probe;\n' |
    xcrun clang -target x86_64-apple-macosx26.0 -mkernel -ffreestanding -fsyntax-only -x c -
  printf 'XNU source, macOS SDK %s, and Xcode tools are ready for X86_64/NONE.\n' "$SDK_VERSION"
  exit 0
fi

mkdir -p "$BUILD_ROOT/cache" "$SDK_PARENT" "$OUT_DIR"

if [[ ! -d "$SOURCE_DIR/.git" && ! -f "$SOURCE_DIR/.git" ]]; then
  [[ ! -d "$SOURCE_DIR" || -z "$(find "$SOURCE_DIR" -mindepth 1 -maxdepth 1 -print -quit)" ]] \
    || fail "refusing to clone into a non-empty source directory: $SOURCE_DIR"
  mkdir -p "$SOURCE_DIR"
  git -C "$SOURCE_DIR" init -q
  git -C "$SOURCE_DIR" remote add origin "$XNU_URL"
  git -C "$SOURCE_DIR" fetch --depth 1 origin "$XNU_COMMIT"
  git -C "$SOURCE_DIR" checkout -q --detach FETCH_HEAD
fi
verify_source

if [[ ! -f "$SDK_CACHE" ]] || [[ "$(shasum -a 256 "$SDK_CACHE" | awk '{print $1}')" != "$SDK_SHA256" ]]; then
  partial="$SDK_CACHE.download"
  rm -f -- "$partial"
  curl --fail --location --retry 3 --output "$partial" "$SDK_URL"
  [[ "$(shasum -a 256 "$partial" | awk '{print $1}')" == "$SDK_SHA256" ]] \
    || fail 'downloaded macOS SDK checksum does not match the pinned value'
  mv -- "$partial" "$SDK_CACHE"
fi
if [[ ! -f "$SDK_DIR/SDKSettings.json" ]]; then
  tar -C "$SDK_PARENT" -xf "$SDK_CACHE"
fi
verify_sdk

ACTUAL_XCODE_VERSION="$(xcodebuild -version | tr '\n' ' ')"
[[ "$ACTUAL_XCODE_VERSION" == "Xcode $XCODE_VERSION "* ]] || fail "Xcode does not match pinned version $XCODE_VERSION"
CLANG="$(xcrun --find clang)"
LD="$(xcrun --find ld)"
IIG="$(xcrun --find iig)"
JOBS="${XNU_X86_JOBS:-$(sysctl -n hw.logicalcpu)}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail 'XNU_X86_JOBS must be a positive integer'

cat >"$OUT_DIR/x86_64-build.env" <<EOF
source=$XNU_URL@$XNU_COMMIT
source_tag=$XNU_TAG
sdk=$SDK_DIR
sdk_version=$SDK_VERSION
platform=MacOSX
arch_config=X86_64
machine_config=NONE
kernel_config=DEVELOPMENT
kernel_version=$KERNEL_VERSION
source_version=$SOURCE_VERSION
compiler=$CLANG
linker=$LD
iig=$IIG
jobs=$JOBS
EOF

printf 'Building XNU %s for X86_64/NONE with %s\n' "$XNU_TAG" "$ACTUAL_XCODE_VERSION"
if make -C "$SOURCE_DIR" build \
  SDKROOT=macosx SDKROOT_RESOLVED="$SDK_DIR" HOST_SDKROOT=macosx HOST_SDKROOT_RESOLVED="$SDK_DIR" \
  SDKVERSION="$SDK_VERSION" PLATFORM=MacOSX \
  ARCH_CONFIGS=X86_64 MACHINE_CONFIGS=NONE KERNEL_CONFIGS=DEVELOPMENT \
  RC_DARWIN_KERNEL_VERSION="$KERNEL_VERSION" RC_ProjectSourceVersion="$SOURCE_VERSION" \
  OBJROOT="$OBJROOT" SYMROOT="$SYMROOT" DSTROOT="$DSTROOT" \
  MAKEJOBS="-j$JOBS" >"$LOG" 2>&1; then
  tail -n 20 "$LOG"
else
  tail -n 100 "$LOG" >&2
  fail "XNU build failed; see $LOG"
fi

KERNEL="$OBJROOT/kernel.development"
[[ -s "$KERNEL" ]] || fail "XNU build did not produce $KERNEL"
ARCHS="$(xcrun lipo -archs "$KERNEL")"
[[ "$ARCHS" == x86_64 ]] || fail "kernel architecture is '$ARCHS'; expected exactly x86_64"
PACKAGE_KERNEL="$OUT_DIR/kernel.development"
cp -L "$KERNEL" "$PACKAGE_KERNEL"

python3 - "$PACKAGE_KERNEL" "$RESULT" "$XNU_COMMIT" "$XNU_TAG" "$SOURCE_VERSION" \
  "$KERNEL_VERSION" "$SDK_VERSION" "$XCODE_VERSION" "$ARCHS" <<'PY'
import hashlib
import json
import pathlib
import sys

kernel = pathlib.Path(sys.argv[1])
digest = hashlib.sha256(kernel.read_bytes()).hexdigest()
result = {
    "artifact": str(kernel),
    "artifact_sha256": digest,
    "artifact_type": "XNU development kernel",
    "architecture": sys.argv[9],
    "xnu_commit": sys.argv[3],
    "xnu_tag": sys.argv[4],
    "xnu_source_version": sys.argv[5],
    "darwin_kernel_version": sys.argv[6],
    "sdk_version": sys.argv[7],
    "xcode_version": sys.argv[8],
    "platform": "MacOSX",
    "machine_config": "NONE",
    "kernel_collection_built": False,
    "boot_verified": False,
    "system_image_required_for_build": False,
}
pathlib.Path(sys.argv[2]).write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
PY
