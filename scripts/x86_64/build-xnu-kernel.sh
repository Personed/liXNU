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
Intel macOS is the native build target; Apple Silicon cross-build support is
experimental until the CI probe produces and validates its artifact.
The build uses pinned AvailabilityVersions, XNU, and libdispatch revisions
with Xcode 26.1.1's macOS 26.1 SDK. It does not use or modify a system disk.

Environment:
  XNU_X86_BUILD_DIR  Absolute build directory with no spaces (default: ${TMPDIR:-/tmp}/xnulinux-x86_64-xnu-build)
  XNU_X86_JOBS       Parallel jobs (default: logical CPU count)

--check validates pinned checkouts, the staged SDK dependencies, the built
x86_64 libfirehose_kernel archive, Xcode tools, and target without downloading
or building. The default action builds kernel.development and writes the
result manifest next to it. XNU's Makefiles cannot safely handle build paths
with spaces, so such paths are rejected.
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
readonly AVAILABILITY_URL='https://github.com/apple-oss-distributions/AvailabilityVersions.git'
readonly AVAILABILITY_COMMIT='149b1777f3e8c2133042d8e188f72e3adf6a7110'
readonly AVAILABILITY_TAG='AvailabilityVersions-157.2'
readonly KERNEL_VERSION='25.5.0'
readonly SOURCE_VERSION='12377.121.6'
readonly SDK_VERSION='26.1'
readonly XCODE_VERSION='26.1.1'

HOST_OS="$(uname -s)"
HOST_PROCESS_ARCH="$(uname -m)"
[[ "$HOST_OS" == Darwin ]] || fail 'the pinned Apple build tools require a macOS build host'
HOST_PROCESS_TRANSLATED="$(sysctl -in sysctl.proc_translated 2>/dev/null || true)"
case "$HOST_PROCESS_TRANSLATED" in
  1)
    # uname -m reports the Rosetta process architecture, not Apple Silicon.
    HOST_ARCH='arm64'
    BUILD_HOST_MODE='cross'
    HOST_PROCESS_TRANSLATED='true'
    ;;
  ''|0)
    HOST_ARCH="$HOST_PROCESS_ARCH"
    HOST_PROCESS_TRANSLATED='false'
    case "$HOST_ARCH" in
      x86_64) BUILD_HOST_MODE='native' ;;
      arm64) BUILD_HOST_MODE='cross' ;;
      *) fail "the x86_64 XNU build requires an Intel or Apple Silicon macOS host (found $HOST_ARCH)" ;;
    esac
    ;;
  *) fail "unexpected sysctl.proc_translated value: $HOST_PROCESS_TRANSLATED" ;;
esac

for tool in git make xcrun xcodebuild sysctl python3 shasum ditto; do
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
AVAILABILITY_SOURCE_DIR="$BUILD_ROOT/availability-versions"
OUT_DIR="$BUILD_ROOT/out"
BASE_SDK_DIR=''
SDK_DIR="$OUT_DIR/sdk/MacOSX26.1.sdk"
OBJROOT="$OUT_DIR/obj"
SYMROOT="$OUT_DIR/sym"
DSTROOT="$OUT_DIR/dst"
LOG="$OUT_DIR/xnu-build.log"
AVAILABILITY_BUILD_DIR="$OUT_DIR/availability"
AVAILABILITY_DSTROOT="$AVAILABILITY_BUILD_DIR/dst"
AVAILABILITY_LOG="$OUT_DIR/availability-build.log"
XNU_HEADERS_BUILD_DIR="$OUT_DIR/xnu-headers"
XNU_HEADERS_OBJROOT="$XNU_HEADERS_BUILD_DIR/obj"
XNU_HEADERS_DSTROOT="$XNU_HEADERS_BUILD_DIR/dst"
XNU_EXPORTED_OSFMK_HEADERS_DIR="$XNU_HEADERS_OBJROOT/EXPORT_HDRS/osfmk"
XNU_EXPORT_HEADERS_LOG="$OUT_DIR/xnu-export-headers.log"
XNU_HEADERS_LOG="$OUT_DIR/xnu-header-install.log"
LIBDISPATCH_BUILD_DIR="$OUT_DIR/libdispatch"
LIBDISPATCH_LOG="$OUT_DIR/libdispatch-build.log"
LIBDISPATCH_DSTROOT="$LIBDISPATCH_BUILD_DIR/dst"
LIBDISPATCH_ARCHIVE="$LIBDISPATCH_DSTROOT/usr/local/lib/kernel/libfirehose_kernel.a"
RESULT="$OUT_DIR/build-result.json"
SDK_PUBLIC_AVAILABILITY_SHA256_FILE="$OUT_DIR/sdk-public-availability.sha256"
SDK_PUBLIC_AVAILABILITY_SHA256=''

sdk_availability_headers_sha256() {
  local sdk_root="$1" header material=''
  for header in \
    Availability.h \
    AvailabilityInternal.h \
    AvailabilityInternalLegacy.h \
    AvailabilityMacros.h \
    AvailabilityVersions.h \
    os/availability.h; do
    [[ -s "$sdk_root/usr/include/$header" ]] || return 1
    material+="$header:$(shasum -a 256 "$sdk_root/usr/include/$header" | awk '{print $1}')"
  done
  printf '%s\n' "$material" | shasum -a 256 | awk '{print $1}'
}

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
  [[ -x "$SDK_DIR/usr/local/libexec/availability.pl" ]] \
    || fail "macOS $SDK_VERSION SDK is missing AvailabilityVersions availability.pl required by XNU"
  [[ -s "$SDK_DIR/usr/local/include/os/atomic_private.h" ]] \
    || fail 'SDK overlay is missing XNU private header os/atomic_private.h'
  [[ -s "$SDK_DIR/usr/local/include/firehose/tracepoint_private.h" ]] \
    || fail 'SDK overlay is missing XNU private firehose headers'
  [[ -s "$SDK_DIR/usr/local/include/machine/cpu_number.h" ]] \
    || fail 'SDK overlay is missing exported XNU machine/cpu_number.h'
  [[ -s "$SDK_DIR/usr/local/include/i386/cpu_number.h" ]] \
    || fail 'SDK overlay is missing exported x86_64 i386/cpu_number.h'
  [[ -s "$SDK_DIR/usr/local/lib/kernel/libfirehose_kernel.a" ]] \
    || fail 'SDK overlay is missing the staged libfirehose_kernel archive'
  verify_machine_header_exports
  verify_public_availability_header
}

verify_machine_header_exports() {
  local generic_header="$XNU_EXPORTED_OSFMK_HEADERS_DIR/machine/cpu_number.h"
  local x86_header="$XNU_EXPORTED_OSFMK_HEADERS_DIR/i386/cpu_number.h"
  local staged_generic_header="$SDK_DIR/usr/local/include/machine/cpu_number.h"
  local staged_x86_header="$SDK_DIR/usr/local/include/i386/cpu_number.h"
  [[ -s "$generic_header" ]] || fail "XNU exporthdrs did not produce $generic_header"
  [[ -s "$x86_header" ]] || fail "XNU exporthdrs did not produce $x86_header"
  grep -Fq '#include "i386/cpu_number.h"' "$generic_header" \
    || fail 'exported machine/cpu_number.h no longer includes i386/cpu_number.h'
  [[ -s "$staged_generic_header" ]] || fail "SDK overlay is missing $staged_generic_header"
  [[ -s "$staged_x86_header" ]] || fail "SDK overlay is missing $staged_x86_header"
  cmp -s "$generic_header" "$staged_generic_header" \
    || fail 'SDK machine/cpu_number.h does not match the pinned XNU export'
  cmp -s "$x86_header" "$staged_x86_header" \
    || fail 'SDK i386/cpu_number.h does not match the pinned XNU export'
}

verify_public_availability_header() {
  local current_sha256 base_sha256
  [[ -n "$SDK_PUBLIC_AVAILABILITY_SHA256" ]] \
    || fail 'missing baseline SHA-256 for the Xcode SDK public availability headers'
  current_sha256="$(sdk_availability_headers_sha256 "$SDK_DIR")" \
    || fail 'SDK overlay is missing one or more public availability headers'
  [[ "$current_sha256" == "$SDK_PUBLIC_AVAILABILITY_SHA256" ]] \
    || fail 'AvailabilityVersions staging modified the Xcode SDK public availability headers'
  if [[ -n "$BASE_SDK_DIR" ]]; then
    base_sha256="$(sdk_availability_headers_sha256 "$BASE_SDK_DIR")" \
      || fail 'Xcode SDK is missing one or more public availability headers'
    [[ "$base_sha256" == "$SDK_PUBLIC_AVAILABILITY_SHA256" ]] \
      || fail 'Xcode SDK public availability headers no longer match the recorded build baseline'
  fi
}

verify_firehose_header_closure() {
  if ! printf '#define KERNEL_PRIVATE 1\n#include <machine/cpu_number.h>\n' |
    xcrun clang -target x86_64-apple-macosx26.0 -isysroot "$SDK_DIR" \
      -I "$SDK_DIR/usr/local/include" -fsyntax-only -x c -; then
    fail 'Firehose XNU machine-header include closure does not compile for x86_64'
  fi
}

verify_firehose_compile_arguments() {
  if ! python3 - "$LIBDISPATCH_LOG" "$LIBDISPATCH_BUILD_DIR/obj" "$SDK_DIR" <<'PY'
import os
import pathlib
import shlex
import sys
import tempfile

log_path = pathlib.Path(sys.argv[1])
objroot = pathlib.Path(sys.argv[2]).resolve()
sdk_root = pathlib.Path(sys.argv[3]).resolve()
if not log_path.is_file():
    raise SystemExit(f"missing libdispatch build log: {log_path}")

log_lines = log_path.read_text(errors="replace").splitlines()
compile_commands = []
for line in log_lines:
    if "firehose_buffer.c" not in line:
        continue
    try:
        tokens = shlex.split(line)
    except ValueError:
        continue
    clang_indexes = [
        index for index, token in enumerate(tokens)
        if pathlib.PurePath(token).name == "clang"
    ]
    if len(clang_indexes) == 1:
        clang_index = clang_indexes[0]
        compile_commands.append((line, tokens, clang_index))

def resolve_response_file(args, objroot):
    paths = [
        token[1:] for token in args
        if token.startswith("@") and pathlib.PurePath(token[1:]).name.endswith("common-args.resp")
    ]
    if len(paths) != 1:
        raise SystemExit(f"expected one Firehose compiler response file, found {paths}")
    path = pathlib.Path(paths[0]).resolve()
    if os.path.commonpath((str(objroot), str(path))) != str(objroot):
        raise SystemExit(f"Firehose compiler response file is outside libdispatch OBJROOT: {path}")
    if not path.is_file():
        raise SystemExit(f"missing Firehose compiler response file: {path}")
    return path


# Exercise tokenization and the full response-file path check with the escaped
# Xcode path and hash-prefixed response-file names produced by Xcode builds.
with tempfile.TemporaryDirectory() as fixture_dir:
    fixture_objroot = pathlib.Path(fixture_dir).resolve()
    fixture_response = fixture_objroot / "b52759a1-common-args.resp"
    fixture_response.write_text(r"-isysroot /tmp/sdk\ path\ with\ spaces" + "\n")
    fixture = (
        r"/Applications/Xcode\ 26.1.1.app/Contents/Developer/Toolchains/"
        r"XcodeDefault.xctoolchain/usr/bin/clang @"
        + shlex.quote(str(fixture_response))
        + r" -isysroot /tmp/sdk\ path -I/tmp/include\ path"
    )
    fixture_tokens = shlex.split(fixture)
    if pathlib.PurePath(fixture_tokens[0]).name != "clang":
        raise SystemExit("could not parse a clang command with an escaped Xcode path")
    fixture_path = resolve_response_file(fixture_tokens[1:], fixture_objroot)
    if fixture_path != fixture_response.resolve():
        raise SystemExit("could not parse an Xcode response file with a hash-prefixed name")
    if shlex.split(fixture_path.read_text()) != ["-isysroot", "/tmp/sdk path with spaces"]:
        raise SystemExit("could not parse arguments from the hash-prefixed response file")

if len(compile_commands) != 1:
    raise SystemExit(
        f"expected one logged clang command for firehose_buffer.c, found {len(compile_commands)}"
    )
_, compile_tokens, clang_index = compile_commands[0]
command_args = compile_tokens[clang_index + 1:]
response_path = resolve_response_file(command_args, objroot)

response_args = shlex.split(response_path.read_text(errors="replace"))
argument_sets = (command_args, response_args)

sysroots = []
for args in argument_sets:
    for index, arg in enumerate(args):
        if arg == "-isysroot" and index + 1 < len(args):
            sysroots.append(args[index + 1])
        elif arg.startswith("-isysroot") and arg != "-isysroot":
            # Clang's joined form is -isysroot<dir>; preserve '=' as part of
            # the path so an unsupported -isysroot=<dir> cannot false-pass.
            sysroots.append(arg[len("-isysroot"):])
if not sysroots or any(pathlib.Path(path).resolve() != sdk_root for path in sysroots):
    raise SystemExit(f"Firehose compile uses SDKROOT {sysroots}; expected only {sdk_root}")

include_paths = []
for args in argument_sets:
    index = 0
    while index < len(args):
        arg = args[index]
        if arg == "-I" and index + 1 < len(args):
            include_paths.append(args[index + 1])
            index += 1
        elif arg.startswith("-I") and len(arg) > 2:
            include_paths.append(arg[2:])
        index += 1

required = {
    sdk_root / "usr/local/include",
    sdk_root / "usr/local/include/os",
    sdk_root / "usr/local/include/firehose",
}
resolved_includes = {pathlib.Path(path).resolve() for path in include_paths}
missing = sorted(str(path) for path in required - resolved_includes)
if missing:
    raise SystemExit(f"Firehose compiler is missing staged SDK include paths: {missing}")

print(f"Verified Firehose compiler response file: {response_path}")
print(f"Verified Firehose SDKROOT and staged include paths: {sdk_root}")
PY
  then
    fail 'Firehose compile command did not use the staged XNU SDK overlay'
  fi
}

verify_sdk_version() {
  local sdk_root="$1"
  [[ -f "$sdk_root/SDKSettings.json" ]] || fail "missing macOS SDK settings at $sdk_root"
  local version
  version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["Version"])' "$sdk_root/SDKSettings.json")"
  [[ "$version" == "$SDK_VERSION" ]] || fail "SDK at $sdk_root is $version; expected $SDK_VERSION"
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

verify_availability_source() {
  [[ -d "$AVAILABILITY_SOURCE_DIR/.git" || -f "$AVAILABILITY_SOURCE_DIR/.git" ]] \
    || fail "missing pinned AvailabilityVersions source at $AVAILABILITY_SOURCE_DIR"
  local actual
  actual="$(git -C "$AVAILABILITY_SOURCE_DIR" rev-parse HEAD)"
  [[ "$actual" == "$AVAILABILITY_COMMIT" ]] \
    || fail "AvailabilityVersions source is $actual; expected $AVAILABILITY_COMMIT ($AVAILABILITY_TAG)"
  [[ -z "$(git -C "$AVAILABILITY_SOURCE_DIR" status --porcelain)" ]] \
    || fail 'refusing to build from a modified AvailabilityVersions source tree'
  [[ -f "$AVAILABILITY_SOURCE_DIR/availability.dsl" && -f "$AVAILABILITY_SOURCE_DIR/Makefile" ]] \
    || fail 'pinned AvailabilityVersions source is missing its build inputs'
}

verify_firehose_archive() {
  local archive="$1"
  [[ -s "$archive" ]] || fail "missing libfirehose_kernel archive at $archive"
  local actual_archs
  actual_archs="$(xcrun lipo -archs "$archive")"
  [[ "$actual_archs" == x86_64 ]] \
    || fail "libfirehose_kernel architecture is '$actual_archs'; expected exactly x86_64"
}

verify_xcode_and_sdk() {
  ACTUAL_XCODE_VERSION="$(xcodebuild -version | tr '\n' ' ')"
  [[ "$ACTUAL_XCODE_VERSION" == "Xcode $XCODE_VERSION "* ]] || fail "Xcode does not match pinned version $XCODE_VERSION"
  BASE_SDK_DIR="$(xcrun --sdk macosx --show-sdk-path)"
  [[ -n "$BASE_SDK_DIR" && -d "$BASE_SDK_DIR" ]] || fail 'Xcode did not resolve its macOS SDK'
  verify_sdk_version "$BASE_SDK_DIR"
}

if (( CHECK_ONLY )); then
  verify_source
  verify_libdispatch_source
  verify_availability_source
  verify_xcode_and_sdk
  [[ -s "$SDK_PUBLIC_AVAILABILITY_SHA256_FILE" ]] \
    || fail "missing recorded SDK public availability header baseline at $SDK_PUBLIC_AVAILABILITY_SHA256_FILE"
  IFS= read -r SDK_PUBLIC_AVAILABILITY_SHA256 < "$SDK_PUBLIC_AVAILABILITY_SHA256_FILE"
  [[ "$SDK_PUBLIC_AVAILABILITY_SHA256" =~ ^[[:xdigit:]]{64}$ ]] \
    || fail 'recorded SDK public availability header SHA-256 is malformed'
  verify_sdk
  xcodebuild -version
  xcrun --find clang >/dev/null
  xcrun --find ld >/dev/null
  xcrun --find iig >/dev/null
  xcrun --find cmake >/dev/null
  xcrun --find ninja >/dev/null
  verify_firehose_archive "$SDK_DIR/usr/local/lib/kernel/libfirehose_kernel.a"
  verify_firehose_compile_arguments
  verify_firehose_header_closure
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

if [[ ! -d "$AVAILABILITY_SOURCE_DIR/.git" && ! -f "$AVAILABILITY_SOURCE_DIR/.git" ]]; then
  [[ ! -d "$AVAILABILITY_SOURCE_DIR" || -z "$(find "$AVAILABILITY_SOURCE_DIR" -mindepth 1 -maxdepth 1 -print -quit)" ]] \
    || fail "refusing to clone into a non-empty AvailabilityVersions directory: $AVAILABILITY_SOURCE_DIR"
  mkdir -p "$AVAILABILITY_SOURCE_DIR"
  git -C "$AVAILABILITY_SOURCE_DIR" init -q
  git -C "$AVAILABILITY_SOURCE_DIR" remote add origin "$AVAILABILITY_URL"
  git -C "$AVAILABILITY_SOURCE_DIR" fetch --depth 1 origin "$AVAILABILITY_COMMIT"
  git -C "$AVAILABILITY_SOURCE_DIR" checkout -q --detach FETCH_HEAD
fi
verify_availability_source

verify_xcode_and_sdk
CLANG="$(xcrun --find clang)"
LD="$(xcrun --find ld)"
IIG="$(xcrun --find iig)"
xcrun --find cmake >/dev/null
xcrun --find ninja >/dev/null
JOBS="${XNU_X86_JOBS:-$(sysctl -n hw.logicalcpu)}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail 'XNU_X86_JOBS must be a positive integer'

rm -rf -- "$SDK_DIR"
mkdir -p "$(dirname "$SDK_DIR")"
ditto "$BASE_SDK_DIR" "$SDK_DIR"
verify_sdk_version "$SDK_DIR"
SDK_PUBLIC_AVAILABILITY_SHA256="$(sdk_availability_headers_sha256 "$BASE_SDK_DIR")" \
  || fail 'Xcode SDK is missing one or more public availability headers'
printf '%s\n' "$SDK_PUBLIC_AVAILABILITY_SHA256" > "$SDK_PUBLIC_AVAILABILITY_SHA256_FILE"
verify_public_availability_header

mkdir -p "$AVAILABILITY_BUILD_DIR/obj" "$AVAILABILITY_BUILD_DIR/sym" "$AVAILABILITY_DSTROOT"
printf 'Building AvailabilityVersions %s for the pinned macOS SDK\n' "$AVAILABILITY_TAG"
if make -C "$AVAILABILITY_SOURCE_DIR" install \
  SRCROOT="$AVAILABILITY_SOURCE_DIR" \
  OBJROOT="$AVAILABILITY_BUILD_DIR/obj" SYMROOT="$AVAILABILITY_BUILD_DIR/sym" \
  DSTROOT="$AVAILABILITY_DSTROOT" >"$AVAILABILITY_LOG" 2>&1; then
  tail -n 20 "$AVAILABILITY_LOG"
else
  tail -n 100 "$AVAILABILITY_LOG" >&2
  fail "AvailabilityVersions build failed; full log: $AVAILABILITY_LOG"
fi
[[ -x "$AVAILABILITY_DSTROOT/usr/local/libexec/availability.pl" ]] \
  || fail 'AvailabilityVersions install did not produce availability.pl'
mkdir -p "$SDK_DIR/usr/local/libexec"
ditto "$AVAILABILITY_DSTROOT/usr/local/libexec/availability.pl" \
  "$SDK_DIR/usr/local/libexec/availability.pl"
chmod 755 "$SDK_DIR/usr/local/libexec/availability.pl"
verify_public_availability_header

mkdir -p "$XNU_HEADERS_OBJROOT" "$XNU_HEADERS_DSTROOT"
printf 'Exporting XNU kernel headers from %s before private header installation\n' "$XNU_TAG"
if make -C "$SOURCE_DIR" exporthdrs \
  SDKROOT=macosx SDKROOT_RESOLVED="$SDK_DIR" HOST_SDKROOT=macosx HOST_SDKROOT_RESOLVED="$SDK_DIR" \
  SDKVERSION="$SDK_VERSION" PLATFORM=MacOSX \
  ARCH_CONFIGS=X86_64 MACHINE_CONFIGS=NONE KERNEL_CONFIGS=DEVELOPMENT \
  RC_DARWIN_KERNEL_VERSION="$KERNEL_VERSION" RC_ProjectSourceVersion="$SOURCE_VERSION" \
  OBJROOT="$XNU_HEADERS_OBJROOT" SYMROOT="$XNU_HEADERS_BUILD_DIR/sym" DSTROOT="$XNU_HEADERS_DSTROOT" \
  MAKEJOBS="-j$JOBS" >"$XNU_EXPORT_HEADERS_LOG" 2>&1; then
  tail -n 20 "$XNU_EXPORT_HEADERS_LOG"
else
  tail -n 100 "$XNU_EXPORT_HEADERS_LOG" >&2
  fail "XNU exporthdrs failed; full log: $XNU_EXPORT_HEADERS_LOG"
fi
printf 'Installing private headers from XNU %s before Firehose build\n' "$XNU_TAG"
if make -C "$SOURCE_DIR" installhdrs \
  SDKROOT=macosx SDKROOT_RESOLVED="$SDK_DIR" HOST_SDKROOT=macosx HOST_SDKROOT_RESOLVED="$SDK_DIR" \
  SDKVERSION="$SDK_VERSION" PLATFORM=MacOSX \
  ARCH_CONFIGS=X86_64 MACHINE_CONFIGS=NONE KERNEL_CONFIGS=DEVELOPMENT \
  RC_DARWIN_KERNEL_VERSION="$KERNEL_VERSION" RC_ProjectSourceVersion="$SOURCE_VERSION" \
  OBJROOT="$XNU_HEADERS_OBJROOT" SYMROOT="$XNU_HEADERS_BUILD_DIR/sym" DSTROOT="$XNU_HEADERS_DSTROOT" \
  MAKEJOBS="-j$JOBS" >"$XNU_HEADERS_LOG" 2>&1; then
  tail -n 20 "$XNU_HEADERS_LOG"
else
  tail -n 100 "$XNU_HEADERS_LOG" >&2
  fail "XNU installhdrs failed; full log: $XNU_HEADERS_LOG"
fi
ditto "$XNU_HEADERS_DSTROOT" "$SDK_DIR"
[[ -s "$SDK_DIR/usr/local/include/os/atomic_private.h" ]] \
  || fail 'XNU installhdrs did not stage usr/local/include/os/atomic_private.h'
[[ -s "$SDK_DIR/usr/local/include/firehose/tracepoint_private.h" ]] \
  || fail 'XNU installhdrs did not stage usr/local/include/firehose/tracepoint_private.h'
[[ -x "$SDK_DIR/usr/local/libexec/availability.pl" ]] \
  || fail 'XNU header installation removed AvailabilityVersions availability.pl'
mkdir -p "$SDK_DIR/usr/local/include/machine" "$SDK_DIR/usr/local/include/i386"
cp "$XNU_EXPORTED_OSFMK_HEADERS_DIR/machine/cpu_number.h" "$SDK_DIR/usr/local/include/machine/cpu_number.h"
cp "$XNU_EXPORTED_OSFMK_HEADERS_DIR/i386/cpu_number.h" "$SDK_DIR/usr/local/include/i386/cpu_number.h"
verify_machine_header_exports
verify_firehose_header_closure
verify_public_availability_header

mkdir -p "$LIBDISPATCH_BUILD_DIR/obj" "$LIBDISPATCH_BUILD_DIR/sym" "$LIBDISPATCH_DSTROOT"
printf 'Building libfirehose_kernel from %s for x86_64\n' "$LIBDISPATCH_TAG"
if xcodebuild \
  -project "$LIBDISPATCH_SOURCE_DIR/libdispatch.xcodeproj" \
  -target libfirehose_kernel -configuration Release -sdk "$SDK_DIR" \
  ARCHS=x86_64 VALID_ARCHS=x86_64 PRODUCT_NAME=firehose_kernel \
  HEADER_SEARCH_PATHS="\$(inherited) $SDK_DIR/usr/local/include $SDK_DIR/usr/local/include/os $SDK_DIR/usr/local/include/firehose" \
  SDKROOT="$SDK_DIR" \
  OBJROOT="$LIBDISPATCH_BUILD_DIR/obj" SYMROOT="$LIBDISPATCH_BUILD_DIR/sym" \
  DSTROOT="$LIBDISPATCH_DSTROOT" \
  install >"$LIBDISPATCH_LOG" 2>&1; then
  tail -n 20 "$LIBDISPATCH_LOG"
else
  tail -n 100 "$LIBDISPATCH_LOG" >&2
  fail "libdispatch firehose_kernel build failed; full log: $LIBDISPATCH_LOG"
fi
verify_firehose_compile_arguments
verify_firehose_archive "$LIBDISPATCH_ARCHIVE"
FIREHOSE_ARCHIVE_SHA256="$(shasum -a 256 "$LIBDISPATCH_ARCHIVE" | awk '{print $1}')"
ditto "$LIBDISPATCH_DSTROOT/usr/local" "$SDK_DIR/usr/local"
verify_sdk
verify_firehose_archive "$SDK_DIR/usr/local/lib/kernel/libfirehose_kernel.a"
verify_public_availability_header
AVAILABILITY_TOOL_SHA256="$(shasum -a 256 "$SDK_DIR/usr/local/libexec/availability.pl" | awk '{print $1}')"
MACHINE_CPU_NUMBER_SHA256="$(shasum -a 256 "$SDK_DIR/usr/local/include/machine/cpu_number.h" | awk '{print $1}')"
I386_CPU_NUMBER_SHA256="$(shasum -a 256 "$SDK_DIR/usr/local/include/i386/cpu_number.h" | awk '{print $1}')"

cat >"$OUT_DIR/x86_64-build.env" <<EOF
source=$XNU_URL@$XNU_COMMIT
source_tag=$XNU_TAG
availability_source=$AVAILABILITY_URL@$AVAILABILITY_COMMIT
availability_tag=$AVAILABILITY_TAG
libdispatch_source=$LIBDISPATCH_URL@$LIBDISPATCH_COMMIT
libdispatch_tag=$LIBDISPATCH_TAG
firehose_archive=$LIBDISPATCH_ARCHIVE
firehose_archive_sha256=$FIREHOSE_ARCHIVE_SHA256
availability_tool_sha256=$AVAILABILITY_TOOL_SHA256
sdk_public_availability_headers_sha256=$SDK_PUBLIC_AVAILABILITY_SHA256
machine_cpu_number_header_sha256=$MACHINE_CPU_NUMBER_SHA256
i386_cpu_number_header_sha256=$I386_CPU_NUMBER_SHA256
sdk=$SDK_DIR
sdk_base=$BASE_SDK_DIR
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
build_host_arch=$HOST_ARCH
build_host_mode=$BUILD_HOST_MODE
build_process_arch=$HOST_PROCESS_ARCH
build_process_translated=$HOST_PROCESS_TRANSLATED
target_arch=x86_64
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
  "$KERNEL_VERSION" "$SDK_VERSION" "$XCODE_VERSION" "$ARCHS" \
  "$LIBDISPATCH_COMMIT" "$LIBDISPATCH_TAG" "$FIREHOSE_ARCHIVE_SHA256" \
  "$AVAILABILITY_COMMIT" "$AVAILABILITY_TAG" "$AVAILABILITY_TOOL_SHA256" \
  "$SDK_PUBLIC_AVAILABILITY_SHA256" "$MACHINE_CPU_NUMBER_SHA256" "$I386_CPU_NUMBER_SHA256" \
  "$HOST_ARCH" "$BUILD_HOST_MODE" "$HOST_PROCESS_ARCH" "$HOST_PROCESS_TRANSLATED" <<'PY'
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
    "target_architecture": sys.argv[9],
    "build_host_arch": sys.argv[19],
    "build_host_mode": sys.argv[20],
    "build_process_arch": sys.argv[21],
    "build_process_translated": sys.argv[22] == "true",
    "xnu_commit": sys.argv[3],
    "xnu_tag": sys.argv[4],
    "xnu_source_version": sys.argv[5],
    "darwin_kernel_version": sys.argv[6],
    "sdk_version": sys.argv[7],
    "xcode_version": sys.argv[8],
    "libdispatch_commit": sys.argv[10],
    "libdispatch_tag": sys.argv[11],
    "libfirehose_kernel_sha256": sys.argv[12],
    "availabilityversions_commit": sys.argv[13],
    "availabilityversions_tag": sys.argv[14],
    "availability_tool_sha256": sys.argv[15],
    "sdk_public_availability_headers_sha256": sys.argv[16],
    "machine_cpu_number_header_sha256": sys.argv[17],
    "i386_cpu_number_header_sha256": sys.argv[18],
    "platform": "MacOSX",
    "machine_config": "NONE",
    "kernel_collection_built": False,
    "boot_verified": False,
    "system_image_required_for_build": False,
}
pathlib.Path(sys.argv[2]).write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
PY
