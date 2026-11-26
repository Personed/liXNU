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
The build uses pinned XNU/libdispatch revisions and Xcode 26.1.1 with its
bundled macOS 26.1 SDK. It does not use or modify a macOS system disk.

Environment:
  XNU_X86_BUILD_DIR  Absolute build directory with no spaces (default: ${TMPDIR:-/tmp}/xnulinux-x86_64-xnu-build)
  XNU_X86_JOBS       Parallel jobs (default: logical CPU count)

--check validates the pinned source checkouts, bundled SDK, built x86_64
libfirehose_kernel archive, Xcode tools, and target without downloading or
building. The default action builds kernel.development and writes the result
manifest next to it. XNU's Makefiles cannot safely handle build paths with
spaces, so such paths are rejected.
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
readonly LIBDISPATCH_URL='https://github.com/apple-oss-distributions/libdispatch.git'
readonly LIBDISPATCH_COMMIT='701f4d1a24ae9c6863901bbbb22624b7d1b87321'
readonly LIBDISPATCH_TAG='libdispatch-1542.100.32'
readonly KERNEL_VERSION='25.5.0'
readonly SOURCE_VERSION='12377.121.6'
readonly SDK_VERSION='26.1'
readonly XCODE_VERSION='26.1.1'

HOST_OS="$(uname -s)"
HOST_ARCH="$(uname -m)"
[[ "$HOST_OS" == Darwin ]] || fail 'the pinned Apple build tools require a macOS build host'
[[ "$HOST_ARCH" == x86_64 ]] || fail "run this x86_64 artifact build on an Intel macOS host (found $HOST_ARCH)"

for tool in git make xcrun xcodebuild sysctl python3 shasum; do
  command -v "$tool" >/dev/null 2>&1 || fail "required host tool not found: $tool"
done

DEFAULT_TMPDIR="${TMPDIR:-/tmp}"
BUILD_ROOT_INPUT="${XNU_X86_BUILD_DIR:-${DEFAULT_TMPDIR%/}/xnulinux-x86_64-xnu-build}"
if [[ "$BUILD_ROOT_INPUT" != /* ]]; then
  BUILD_ROOT_INPUT="$(pwd -P)/$BUILD_ROOT_INPUT"
fi
if (( ! CHECK_ONLY )); then
  mkdir -p "$BUILD_ROOT_INPUT"
fi
[[ -d "$BUILD_ROOT_INPUT" ]] || fail "build directory does not exist: $BUILD_ROOT_INPUT"
BUILD_ROOT="$(cd "$BUILD_ROOT_INPUT" && pwd -P)"
[[ "$BUILD_ROOT" != *" "* ]] || fail 'XNU build paths cannot contain spaces; choose XNU_X86_BUILD_DIR without spaces'
SOURCE_DIR="$BUILD_ROOT/source"
LIBDISPATCH_SOURCE_DIR="$BUILD_ROOT/libdispatch-source"
SDK_DIR=''
OUT_DIR="$BUILD_ROOT/out"
OBJROOT="$OUT_DIR/obj"
SYMROOT="$OUT_DIR/sym"
DSTROOT="$OUT_DIR/dst"
LOG="$OUT_DIR/xnu-build.log"
LIBDISPATCH_BUILD_DIR="$OUT_DIR/libdispatch"
LIBDISPATCH_LOG="$OUT_DIR/libdispatch-build.log"
LIBDISPATCH_DSTROOT="$LIBDISPATCH_BUILD_DIR/dst"
LIBDISPATCH_ARCHIVE="$LIBDISPATCH_DSTROOT/usr/local/lib/kernel/libfirehose_kernel.a"
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
  [[ -f "$SDK_DIR/SDKSettings.json" ]] || fail "missing bundled macOS SDK settings at $SDK_DIR"
  local version
  version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["Version"])' "$SDK_DIR/SDKSettings.json")"
  [[ "$version" == "$SDK_VERSION" ]] || fail "SDK at $SDK_DIR is $version; expected $SDK_VERSION"
  [[ -x "$SDK_DIR/System/DriverKit/usr/local/libexec/availability.pl" ]] \
    || fail "macOS $SDK_VERSION SDK is missing DriverKit availability.pl required by XNU"
}

verify_libdispatch_source() {
  [[ -d "$LIBDISPATCH_SOURCE_DIR/.git" || -f "$LIBDISPATCH_SOURCE_DIR/.git" ]] \
    || fail "missing pinned libdispatch source at $LIBDISPATCH_SOURCE_DIR"
  local actual
  actual="$(git -C "$LIBDISPATCH_SOURCE_DIR" rev-parse HEAD)"
  [[ "$actual" == "$LIBDISPATCH_COMMIT" ]] \
    || fail "libdispatch source is $actual; expected $LIBDISPATCH_COMMIT ($LIBDISPATCH_TAG)"
  [[ -z "$(git -C "$LIBDISPATCH_SOURCE_DIR" status --porcelain)" ]] \
    || fail 'refusing to build from a modified libdispatch source tree'
  grep -Fq 'name = libfirehose_kernel;' "$LIBDISPATCH_SOURCE_DIR/libdispatch.xcodeproj/project.pbxproj" \
    || fail 'pinned libdispatch source does not define the libfirehose_kernel target'
}

verify_firehose_archive() {
  [[ -s "$LIBDISPATCH_ARCHIVE" ]] || fail "missing built libfirehose_kernel archive at $LIBDISPATCH_ARCHIVE"
  local actual_archs
  actual_archs="$(xcrun lipo -archs "$LIBDISPATCH_ARCHIVE")"
  [[ "$actual_archs" == x86_64 ]] \
    || fail "libfirehose_kernel architecture is '$actual_archs'; expected exactly x86_64"
}

verify_xcode_and_sdk() {
  local actual_xcode
  actual_xcode="$(xcodebuild -version | awk '/^Xcode / { print $2; exit }')"
  [[ "$actual_xcode" == "$XCODE_VERSION" ]] || fail "Xcode is $actual_xcode; expected $XCODE_VERSION"
  SDK_DIR="$(xcrun --sdk macosx --show-sdk-path)"
  [[ -n "$SDK_DIR" && -d "$SDK_DIR" ]] || fail 'Xcode did not resolve its macOS SDK'
  verify_sdk
}

if (( CHECK_ONLY )); then
  verify_source
  verify_libdispatch_source
  verify_xcode_and_sdk
  xcodebuild -version
  xcrun --find clang >/dev/null
  xcrun --find ld >/dev/null
  xcrun --find iig >/dev/null
  verify_firehose_archive
  printf 'int xnu_x86_64_target_probe;\n' |
    xcrun clang -target x86_64-apple-macosx26.0 -mkernel -ffreestanding -fsyntax-only -x c -
  printf 'XNU source, macOS SDK %s, and Xcode tools are ready for X86_64/NONE.\n' "$SDK_VERSION"
  exit 0
fi

mkdir -p "$OUT_DIR"

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

if [[ ! -d "$LIBDISPATCH_SOURCE_DIR/.git" && ! -f "$LIBDISPATCH_SOURCE_DIR/.git" ]]; then
  [[ ! -d "$LIBDISPATCH_SOURCE_DIR" || -z "$(find "$LIBDISPATCH_SOURCE_DIR" -mindepth 1 -maxdepth 1 -print -quit)" ]] \
    || fail "refusing to clone into a non-empty libdispatch directory: $LIBDISPATCH_SOURCE_DIR"
  mkdir -p "$LIBDISPATCH_SOURCE_DIR"
  git -C "$LIBDISPATCH_SOURCE_DIR" init -q
  git -C "$LIBDISPATCH_SOURCE_DIR" remote add origin "$LIBDISPATCH_URL"
  git -C "$LIBDISPATCH_SOURCE_DIR" fetch --depth 1 origin "$LIBDISPATCH_COMMIT"
  git -C "$LIBDISPATCH_SOURCE_DIR" checkout -q --detach FETCH_HEAD
fi
verify_libdispatch_source

ACTUAL_XCODE_VERSION="$(xcodebuild -version | tr '\n' ' ')"
[[ "$ACTUAL_XCODE_VERSION" == "Xcode $XCODE_VERSION "* ]] || fail "Xcode does not match pinned version $XCODE_VERSION"
verify_xcode_and_sdk
CLANG="$(xcrun --find clang)"
LD="$(xcrun --find ld)"
IIG="$(xcrun --find iig)"
JOBS="${XNU_X86_JOBS:-$(sysctl -n hw.logicalcpu)}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail 'XNU_X86_JOBS must be a positive integer'

mkdir -p "$LIBDISPATCH_BUILD_DIR/obj" "$LIBDISPATCH_BUILD_DIR/sym" "$LIBDISPATCH_DSTROOT"
printf 'Building libfirehose_kernel from %s for x86_64\n' "$LIBDISPATCH_TAG"
if xcodebuild \
  -project "$LIBDISPATCH_SOURCE_DIR/libdispatch.xcodeproj" \
  -target libfirehose_kernel -configuration Release -sdk macosx \
  ARCHS=x86_64 VALID_ARCHS=x86_64 PRODUCT_NAME=firehose_kernel \
  SDKROOT="$SDK_DIR" \
  OBJROOT="$LIBDISPATCH_BUILD_DIR/obj" SYMROOT="$LIBDISPATCH_BUILD_DIR/sym" \
  DSTROOT="$LIBDISPATCH_DSTROOT" \
  install >"$LIBDISPATCH_LOG" 2>&1; then
  tail -n 20 "$LIBDISPATCH_LOG"
else
  tail -n 100 "$LIBDISPATCH_LOG" >&2
  fail "libdispatch firehose_kernel build failed; full log: $LIBDISPATCH_LOG"
fi
verify_firehose_archive
FIREHOSE_ARCHIVE_SHA256="$(shasum -a 256 "$LIBDISPATCH_ARCHIVE" | awk '{print $1}')"

cat >"$OUT_DIR/x86_64-build.env" <<EOF
source=$XNU_URL@$XNU_COMMIT
source_tag=$XNU_TAG
libdispatch_source=$LIBDISPATCH_URL@$LIBDISPATCH_COMMIT
libdispatch_tag=$LIBDISPATCH_TAG
firehose_archive=$LIBDISPATCH_ARCHIVE
firehose_archive_sha256=$FIREHOSE_ARCHIVE_SHA256
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
  LDFLAGS_KERNEL_SDK="-L$SDK_DIR/usr/local/lib/kernel -L$SDK_DIR/usr/local/lib/kernel/platform -L$LIBDISPATCH_DSTROOT/usr/local/lib/kernel" \
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
  "$KERNEL_VERSION" "$SDK_VERSION" "$XCODE_VERSION" "$ARCHS" \
  "$LIBDISPATCH_COMMIT" "$LIBDISPATCH_TAG" "$FIREHOSE_ARCHIVE_SHA256" <<'PY'
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
    "libdispatch_commit": sys.argv[10],
    "libdispatch_tag": sys.argv[11],
    "libfirehose_kernel_sha256": sys.argv[12],
    "platform": "MacOSX",
    "machine_config": "NONE",
    "kernel_collection_built": False,
    "boot_verified": False,
    "system_image_required_for_build": False,
}
pathlib.Path(sys.argv[2]).write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
PY
