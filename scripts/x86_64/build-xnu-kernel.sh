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

Build the latest publicly published Tahoe 26 XNU source as an x86_64 macOS
development kernel. The runtime goal is Tahoe 26.6.2, but the latest published
matching XNU source is 26.5; this artifact does not prove 26.6.2 compatibility.
Intel macOS is the native build target; Apple Silicon cross-build support is
experimental until the CI probe produces and validates its artifact.
The build uses pinned AvailabilityVersions, XNU, and libdispatch revisions
with Xcode 26.6's macOS 26.5 SDK. It does not use or modify a system disk.

Environment:
  XNU_X86_BUILD_DIR  Absolute build directory with no spaces (default: ${TMPDIR:-/tmp}/xnulinux-x86_64-xnu-build)
  XNU_X86_JOBS       Parallel jobs (default: logical CPU count)
  XNU_X86_KDKROOT    macOS 26.5 build 25F71 KDK for the pinned source build;
                     required on Apple Silicon hosts; Intel-only builds may
                     omit it when the pinned SDK contains TrustCache/API.h

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

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
source "$SCRIPT_DIR/xnu-trustcache-scan-workspace.sh"

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
readonly XNU_SOURCE_RELEASE='26.5'
readonly RUNTIME_TARGET_VERSION='26.6.2'
readonly SDK_VERSION='26.5'
readonly XCODE_VERSION='26.6'
readonly KDK_VERSION='26.5'
readonly KDK_BUILD='25F71'

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

for tool in git make xcrun xcodebuild sysctl python3 shasum ditto rsync; do
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
HOST_CLANG=''
HOST_OS_VERSION=''
CLANG=''
CXX_CLANG=''
HOST_SYS_UNISTD_BASE_SHA256=''
HOST_SYS_UNISTD_OVERLAY_SHA256=''
XNU_HEADER_TREE_DIAGNOSTICS_RECORDED='false'
XNU_TARGET_INCLUDE_PATHS_VERIFIED='false'
XNU_TARGET_SYSROOT_MODE='unknown'
XNU_WARNING_POLICY_VERIFIED='false'
XNU_GENERIC_SDK_HEADER_DEPENDENCY_COUNT='unknown'
XNU_TARGET_DEPENDENCY_FILE_COUNT='unknown'
XNU_DEPENDENCY_FILE_ENTRY_COUNT='unknown'
XNU_HEADER_DEPENDENCY_COUNT='unknown'
XNU_MISSING_DEPENDENCY_FILE_COUNT='unknown'
XNU_TARGET_SDKROOT_OVERLAY_NOTE_COUNT='unknown'
XNU_TARGET_SDKROOT_BASE_NOTE_COUNT='unknown'
XNU_TARGET_NO_EXPLICIT_SYSROOT_COMMAND_COUNT='unknown'
XNU_OVERLAY_HEADER_DEPENDENCY_COUNT='unknown'
HOST_TOOL_UNISTD_BASE_PROBE_LOG="$OUT_DIR/host-tool-unistd-base-probe.log"
HOST_TOOL_UNISTD_OVERLAY_PROBE_LOG="$OUT_DIR/host-tool-unistd-overlay-probe.log"
SDK_DIR="$OUT_DIR/sdk/MacOSX${SDK_VERSION}.sdk"
KDKROOT="${XNU_X86_KDKROOT:-/Library/Developer/KDKs/KDK_${KDK_VERSION}_${KDK_BUILD}.kdk}"
KDK_PRIVATE_HEADER_SHA256=''
KDK_TRUSTCACHE_HEADER=''
KDK_PRESENT='false'
KDK_REQUIRED='false'
KDK_PRODUCT_VERSION=''
KDK_PRODUCT_BUILD=''
KDK_XNU_VERSION=''
TRUSTCACHE_HEADER=''
TRUSTCACHE_HEADER_SHA256=''
TRUSTCACHE_HEADER_SOURCE=''
XNU_KERNEL_FRAMEWORK_DIR="$SDK_DIR/System/Library/Frameworks/Kernel.framework"
XNU_KERNEL_PRIVATE_HEADERS="$XNU_KERNEL_FRAMEWORK_DIR/PrivateHeaders"
XNU_KERNEL_HEADERS="$XNU_KERNEL_FRAMEWORK_DIR/Headers"
OBJROOT="$OUT_DIR/obj"
SYMROOT="$OUT_DIR/sym"
DSTROOT="$OUT_DIR/dst"
LOG="$OUT_DIR/xnu-build.log"
XNU_WARNING_DIAGNOSTIC_LOG="$OUT_DIR/xnu-warning-policy-probe.log"
AVAILABILITY_BUILD_DIR="$OUT_DIR/availability"
AVAILABILITY_DSTROOT="$AVAILABILITY_BUILD_DIR/dst"
AVAILABILITY_LOG="$OUT_DIR/availability-build.log"
XNU_HEADERS_BUILD_DIR="$OUT_DIR/xnu-headers"
XNU_HEADERS_OBJROOT="$XNU_HEADERS_BUILD_DIR/obj"
XNU_HEADERS_DSTROOT="$XNU_HEADERS_BUILD_DIR/dst"
XNU_EXPORTED_OSFMK_HEADERS_DIR="$XNU_HEADERS_OBJROOT/EXPORT_HDRS/osfmk"
XNU_EXPORT_HEADERS_LOG="$OUT_DIR/xnu-export-headers.log"
XNU_HEADERS_LOG="$OUT_DIR/xnu-header-install.log"
XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG="$OUT_DIR/xnu-target-include-diagnostics.log"
LIBDISPATCH_BUILD_DIR="$OUT_DIR/libdispatch"
LIBDISPATCH_LOG="$OUT_DIR/libdispatch-build.log"
LIBDISPATCH_DSTROOT="$LIBDISPATCH_BUILD_DIR/dst"
LIBDISPATCH_ARCHIVE="$LIBDISPATCH_DSTROOT/usr/local/lib/kernel/libfirehose_kernel.a"
RESULT="$OUT_DIR/build-result.json"
SDK_PUBLIC_AVAILABILITY_SHA256_FILE="$OUT_DIR/sdk-public-availability.sha256"
SDK_PUBLIC_AVAILABILITY_SHA256=''
HOST_SDK_DIAGNOSTIC_LOG="$OUT_DIR/host-sdk-diagnostic.log"
XNU_HEADER_TREES_DIAGNOSTIC_LOG="$OUT_DIR/xnu-staged-header-trees.log"
KDK_DIAGNOSTIC_LOG="$OUT_DIR/kdk-preflight.log"
KDK_DIAGNOSTIC_TARGET="$KDK_DIAGNOSTIC_LOG"

if (( ! CHECK_ONLY )); then
  mkdir -p "$OUT_DIR"
  : > "$KDK_DIAGNOSTIC_LOG"
else
  KDK_DIAGNOSTIC_TARGET='/dev/stderr'
fi

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
  verify_public_sys_unistd_header
}

verify_kdk() {
  local explicit_kdk='false'
  [[ -n "${XNU_X86_KDKROOT:-}" ]] && explicit_kdk='true'
  [[ "$BUILD_HOST_MODE" == cross ]] && KDK_REQUIRED='true'

  if [[ -e "$KDKROOT" || -L "$KDKROOT" ]]; then
    [[ -d "$KDKROOT" ]] || fail "KDK path exists but is not a directory: $KDKROOT"
    [[ "$KDKROOT" != *" "* ]] || fail 'KDK paths cannot contain spaces because XNU Makefiles cannot safely handle them'
    local requested_kdk="$KDKROOT"
    KDKROOT="$(cd "$KDKROOT" && pwd -P)"
    [[ "$(basename "$KDKROOT")" == "KDK_${KDK_VERSION}_${KDK_BUILD}.kdk" ]] \
      || fail "resolved KDK bundle is '$(basename "$KDKROOT")'; expected KDK_${KDK_VERSION}_${KDK_BUILD}.kdk for source release $XNU_SOURCE_RELEASE (requested path: $requested_kdk)"
    local metadata_json
    if ! metadata_json="$(python3 "$SCRIPT_DIR/verify-xnu-kdk.py" --kdk-root "$KDKROOT" \
      --expected-version "$KDK_VERSION" --expected-build "$KDK_BUILD" \
      --expected-xnu-version "$SOURCE_VERSION" 2>>"$KDK_DIAGNOSTIC_TARGET")"; then
      fail "KDK metadata does not match the pinned XNU source; see $KDK_DIAGNOSTIC_TARGET"
    fi
    KDK_PRESENT='true'
    KDK_PRODUCT_VERSION="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["product_version"])' <<<"$metadata_json")"
    KDK_PRODUCT_BUILD="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["product_build"])' <<<"$metadata_json")"
    KDK_XNU_VERSION="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["xnu_version"])' <<<"$metadata_json")"
    KDK_TRUSTCACHE_HEADER="$KDKROOT/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/TrustCache/API.h"
    [[ -s "$KDK_TRUSTCACHE_HEADER" ]] \
      || fail "matching KDK is missing its private TrustCache API header: $KDK_TRUSTCACHE_HEADER"
    [[ -d "$KDKROOT/System/Library/KernelSupport" ]] \
      || fail "matching KDK is missing KernelSupport: $KDKROOT/System/Library/KernelSupport"
    KDK_PRIVATE_HEADER_SHA256="$(shasum -a 256 "$KDK_TRUSTCACHE_HEADER" | awk '{print $1}')"
    [[ "$KDK_PRIVATE_HEADER_SHA256" =~ ^[[:xdigit:]]{64}$ ]] || fail 'could not compute KDK TrustCache header SHA-256'
    TRUSTCACHE_HEADER="$KDK_TRUSTCACHE_HEADER"
    TRUSTCACHE_HEADER_SOURCE='matching-kdk'
    TRUSTCACHE_HEADER_SHA256="$KDK_PRIVATE_HEADER_SHA256"
    printf 'kdk_root=%s\nkdk_product_version=%s\nkdk_product_build=%s\nkdk_xnu_version=%s\ntrustcache_header=%s\ntrustcache_header_sha256=%s\n' \
      "$KDKROOT" "$KDK_PRODUCT_VERSION" "$KDK_PRODUCT_BUILD" "$KDK_XNU_VERSION" \
      "$TRUSTCACHE_HEADER" "$TRUSTCACHE_HEADER_SHA256" > "$KDK_DIAGNOSTIC_TARGET"
  elif [[ "$explicit_kdk" == true || "$KDK_REQUIRED" == true ]]; then
    printf 'kdk_required=true\nkdk_present=false\nexpected_version=%s\nexpected_build=%s\nrequested_path=%s\n' \
      "$KDK_VERSION" "$KDK_BUILD" "$KDKROOT" > "$KDK_DIAGNOSTIC_TARGET"
    fail "matching Kernel Debug Kit $KDK_VERSION build $KDK_BUILD is required on Apple Silicon hosts or when XNU_X86_KDKROOT is set; install Apple's official KDK at $KDKROOT (the official download requires an Apple Developer account)"
  else
    KDKROOT=''
    printf 'kdk_required=false\nkdk_present=false\nkdk_identity_status=not-present-not-required-for-intel\nreason=upstream XNU README permits Intel-only builds without a KDK\n' > "$KDK_DIAGNOSTIC_TARGET"
  fi
}

verify_trustcache_header() {
  local sdk_root="$1"
  local sdk_header="$sdk_root/usr/local/include/TrustCache/API.h"
  local sdk_framework_header="$sdk_root/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/TrustCache/API.h"
  if [[ "$KDK_PRESENT" == true ]]; then
    [[ -s "$KDK_TRUSTCACHE_HEADER" ]] \
      || fail "matching KDK TrustCache API header disappeared: $KDK_TRUSTCACHE_HEADER"
    TRUSTCACHE_HEADER="$KDK_TRUSTCACHE_HEADER"
    TRUSTCACHE_HEADER_SOURCE='matching-kdk'
  elif [[ -s "$sdk_header" ]]; then
    TRUSTCACHE_HEADER="$sdk_header"
    TRUSTCACHE_HEADER_SOURCE='pinned-xcode-sdk'
  elif [[ -s "$sdk_framework_header" ]]; then
    TRUSTCACHE_HEADER="$sdk_framework_header"
    TRUSTCACHE_HEADER_SOURCE='pinned-xcode-sdk'
  else
    local sdk_usr_include_header="$sdk_root/usr/include/TrustCache/API.h"
    local sdk_header_scan_mode='build'
    (( CHECK_ONLY )) && sdk_header_scan_mode='check'
    local sdk_header_scan_root
    sdk_header_scan_root="$(xnu_trustcache_scan_workspace "$BUILD_ROOT" "$sdk_header_scan_mode")" \
      || fail "cannot create the TrustCache SDK scan workspace in $sdk_header_scan_mode mode"
    local sdk_header_candidates_file="$sdk_header_scan_root/trustcache-sdk-header-candidates.txt"
    local sdk_header_sorted_candidates_file="$sdk_header_scan_root/trustcache-sdk-header-candidates.sorted.txt"
    local sdk_header_scan_errors="$sdk_header_scan_root/trustcache-sdk-header-scan-errors.log"
    local sdk_header_scan_status='complete'
    : > "$sdk_header_candidates_file"
    : > "$sdk_header_sorted_candidates_file"
    : > "$sdk_header_scan_errors"
    if ! find -L "$sdk_root" -type f -path '*/TrustCache/API.h' -print \
      > "$sdk_header_candidates_file" 2> "$sdk_header_scan_errors"; then
      sdk_header_scan_status='failed'
    elif ! LC_ALL=C sort "$sdk_header_candidates_file" \
      > "$sdk_header_sorted_candidates_file" 2>> "$sdk_header_scan_errors"; then
      sdk_header_scan_status='failed'
    elif [[ -s "$sdk_header_scan_errors" ]]; then
      sdk_header_scan_status='failed'
    fi
    if [[ "$sdk_header_scan_status" != complete ]]; then
      printf 'trustcache_header_scan_status=failed\ntrustcache_header_scan_candidates=%s\ntrustcache_header_scan_errors=%s\n' \
        "$sdk_header_candidates_file" "$sdk_header_scan_errors" >> "$KDK_DIAGNOSTIC_TARGET"
      [[ ! -s "$sdk_header_scan_errors" ]] || cat "$sdk_header_scan_errors" >&2
      xnu_trustcache_scan_workspace_cleanup "$sdk_header_scan_mode" "$sdk_header_scan_root"
      fail "full Xcode SDK TrustCache header scan failed; no absence claim is made (candidate list: $sdk_header_candidates_file; errors: $sdk_header_scan_errors)"
    fi
    mv "$sdk_header_sorted_candidates_file" "$sdk_header_candidates_file"
    local sdk_header_candidates
    sdk_header_candidates="$(<"$sdk_header_candidates_file")"
    local sdk_header_candidates_flat="${sdk_header_candidates//$'\n'/|}"
    [[ -n "$sdk_header_candidates_flat" ]] || sdk_header_candidates_flat='none'
    printf 'trustcache_header_status=missing\ntrustcache_header_scan_status=complete\nchecked_target_search_paths=%s|%s\nunsupported_public_sdk_path=%s\nfull_sdk_header_scan=%s\n' \
      "$sdk_header" "$sdk_framework_header" "$sdk_usr_include_header" "$sdk_header_candidates_flat" \
      >> "$KDK_DIAGNOSTIC_TARGET"
    xnu_trustcache_scan_workspace_cleanup "$sdk_header_scan_mode" "$sdk_header_scan_root"
    fail "XNU source requires real TrustCache/API.h in the target's explicit include paths; none exists in the macOS $SDK_VERSION SDK and no matching KDK is available. Full SDK scan found: $sdk_header_candidates_flat. A copy under usr/include is not accepted because this target uses -nostdlibinc and does not search that tree; no header stub will be used (checked $sdk_header and $sdk_framework_header; unsupported path $sdk_usr_include_header)"
  fi
  TRUSTCACHE_HEADER_SHA256="$(shasum -a 256 "$TRUSTCACHE_HEADER" | awk '{print $1}')"
  [[ "$TRUSTCACHE_HEADER_SHA256" =~ ^[[:xdigit:]]{64}$ ]] \
    || fail 'could not compute TrustCache API header SHA-256'
  printf 'trustcache_source=%s\ntrustcache_header=%s\ntrustcache_header_sha256=%s\n' \
    "$TRUSTCACHE_HEADER_SOURCE" "$TRUSTCACHE_HEADER" "$TRUSTCACHE_HEADER_SHA256" \
    >> "$KDK_DIAGNOSTIC_TARGET"
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

verify_public_sys_unistd_header() {
  [[ -n "$BASE_SDK_DIR" ]] || fail 'missing base Xcode SDK for public sys/unistd.h comparison'
  local base_header="$BASE_SDK_DIR/usr/include/sys/unistd.h"
  local overlay_header="$SDK_DIR/usr/include/sys/unistd.h"
  [[ -s "$base_header" && -s "$overlay_header" ]] \
    || fail 'base or target SDK is missing public sys/unistd.h'
  cmp -s "$base_header" "$overlay_header" \
    || fail 'target SDK overlay changed Xcode public sys/unistd.h'
}

count_header_tree_files() {
  local header_tree="$1"
  if [[ -d "$header_tree" ]]; then
    find -L "$header_tree" -type f -name '*.h' -print 2>/dev/null | wc -l | awk '{print $1}'
  else
    printf '0\n'
  fi
}

record_xnu_header_tree_diagnostics() {
  local label suffix policy base_count install_count overlay_count base_dir install_dir overlay_dir
  : > "$XNU_HEADER_TREES_DIAGNOSTIC_LOG"
  while IFS='|' read -r label suffix policy; do
    [[ -n "$label" ]] || continue
    base_count="$(count_header_tree_files "$BASE_SDK_DIR/$suffix")"
    install_count="$(count_header_tree_files "$XNU_HEADERS_DSTROOT/$suffix")"
    overlay_count="$(count_header_tree_files "$SDK_DIR/$suffix")"
    [[ -d "$BASE_SDK_DIR/$suffix" ]] && base_dir='present' || base_dir='absent'
    [[ -d "$XNU_HEADERS_DSTROOT/$suffix" ]] && install_dir='present' || install_dir='absent'
    [[ -d "$SDK_DIR/$suffix" ]] && overlay_dir='present' || overlay_dir='absent'
    printf '%s policy=%s base_dir=%s base_files=%s installhdrs_dir=%s installhdrs_files=%s overlay_dir=%s overlay_files=%s\n' \
      "$label" "$policy" "$base_dir" "$base_count" "$install_dir" "$install_count" "$overlay_dir" "$overlay_count" \
      >> "$XNU_HEADER_TREES_DIAGNOSTIC_LOG"
  done <<'TREES'
kernel_usr_local|usr/local/include/kernel|excluded-sdk-root-for-x86_64
iBoot_usr_local|usr/local/include/iBoot|excluded-sdk-root-for-x86_64
Kernel_framework_AppleFeatures|System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/AppleFeatures|excluded-sdk-root-for-x86_64
Kernel_framework_platform|System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform|excluded-sdk-root-for-x86_64
TREES
  XNU_HEADER_TREE_DIAGNOSTICS_RECORDED='true'
}

run_host_tool_unistd_probe() {
  local sdk_root="$1" output_file="${2:-/dev/stderr}"
  printf '#include <unistd.h>\nint xnu_host_unistd_probe;\n' |
    "$HOST_CLANG" -isysroot "$sdk_root" -mmacosx-version-min="$HOST_OS_VERSION" \
      -g -O0 -I "$SOURCE_DIR" -I "$SOURCE_DIR/SETUP/config" -DYY_NO_INPUT -Wall \
      -fsyntax-only -x c - >"$output_file" 2>&1
}

verify_host_tool_unistd_probe() {
  local sdk_root="$1" label="$2" output_file="${3:-/dev/stderr}"
  if ! run_host_tool_unistd_probe "$sdk_root" "$output_file"; then
    fail "XNU host-tool <unistd.h> probe failed with $label SDK root $sdk_root"
  fi
}

verify_xnu_warning_policy() {
  local -a warning_exceptions=(
    -Wno-error=reserved-identifier
    -Wno-error=unused-but-set-variable
    -Wno-error=void-pointer-to-enum-cast
  )
  : > "$XNU_WARNING_DIAGNOSTIC_LOG"
  if ! cat <<'PROBES' | "$CLANG" -Weverything -Werror "${warning_exceptions[@]}" -fsyntax-only -x c - \
      >>"$XNU_WARNING_DIAGNOSTIC_LOG" 2>&1; then
extern int __xnu_reserved_warning_probe;
enum xnu_void_enum_probe { XNU_VOID_ENUM_VALUE };
enum xnu_void_enum_probe xnu_convert_void_pointer(void *);
enum xnu_void_enum_probe xnu_convert_void_pointer(void *value) {
  return (enum xnu_void_enum_probe)value;
}
int main(void) {
  enum xnu_void_enum_probe converted;
  int xnu_set_but_unused;
  xnu_set_but_unused = 1;
  converted = xnu_convert_void_pointer((void *)0);
  return (int)converted + __xnu_reserved_warning_probe;
}
PROBES
    fail "selective XNU warning-policy probe failed; see $XNU_WARNING_DIAGNOSTIC_LOG"
  fi
  grep -Fq 'reserved-identifier' "$XNU_WARNING_DIAGNOSTIC_LOG" \
    || fail "reserved-identifier probe did not exercise the expected warning; see $XNU_WARNING_DIAGNOSTIC_LOG"
  grep -Fq 'unused-but-set-variable' "$XNU_WARNING_DIAGNOSTIC_LOG" \
    || fail "unused-but-set-variable probe did not exercise the expected warning; see $XNU_WARNING_DIAGNOSTIC_LOG"
  grep -Fq 'void-pointer-to-enum-cast' "$XNU_WARNING_DIAGNOSTIC_LOG" \
    || fail "void-pointer-to-enum-cast probe did not exercise the expected warning; see $XNU_WARNING_DIAGNOSTIC_LOG"

  if ! printf 'int main(void) { int unused; return 0; }\n' |
    "$CLANG" -Weverything -Wunused-variable -fsyntax-only -x c - \
      >>"$XNU_WARNING_DIAGNOSTIC_LOG" 2>&1; then
    fail "baseline unused-variable probe failed without global -Werror; see $XNU_WARNING_DIAGNOSTIC_LOG"
  fi
  grep -Fq 'warning: unused variable' "$XNU_WARNING_DIAGNOSTIC_LOG" \
    || fail "baseline probe did not emit unused-variable warning; see $XNU_WARNING_DIAGNOSTIC_LOG"
  if printf 'int main(void) { int unused; return 0; }\n' |
    "$CLANG" -Weverything -Wunused-variable -Werror "${warning_exceptions[@]}" \
      -fsyntax-only -x c - >>"$XNU_WARNING_DIAGNOSTIC_LOG" 2>&1; then
    fail "general warning probe unexpectedly passed with global -Werror; see $XNU_WARNING_DIAGNOSTIC_LOG"
  fi
  grep -Fq 'error: unused variable' "$XNU_WARNING_DIAGNOSTIC_LOG" \
    || fail "global -Werror did not promote the unrelated warning; see $XNU_WARNING_DIAGNOSTIC_LOG"
  printf 'reserved_identifier_warning=nonfatal\nunused_but_set_variable_warning=nonfatal\nvoid_pointer_to_enum_cast_warning=nonfatal\nother_warnings_remain_errors=true\n' \
    >> "$XNU_WARNING_DIAGNOSTIC_LOG"
  XNU_WARNING_POLICY_VERIFIED='true'
}

record_host_sdk_diagnostics() {
  local base_unistd="$BASE_SDK_DIR/usr/include/sys/unistd.h"
  local overlay_unistd="$SDK_DIR/usr/include/sys/unistd.h"
  local headers_match='false' overlay_probe='failed'
  [[ -s "$base_unistd" ]] || fail "base Xcode SDK is missing $base_unistd"
  [[ -s "$overlay_unistd" ]] || fail "XNU SDK overlay is missing $overlay_unistd"
  HOST_SYS_UNISTD_BASE_SHA256="$(shasum -a 256 "$base_unistd" | awk '{print $1}')"
  HOST_SYS_UNISTD_OVERLAY_SHA256="$(shasum -a 256 "$overlay_unistd" | awk '{print $1}')"
  if cmp -s "$base_unistd" "$overlay_unistd"; then
    headers_match='true'
  fi

  : > "$HOST_TOOL_UNISTD_BASE_PROBE_LOG"
  verify_host_tool_unistd_probe "$BASE_SDK_DIR" 'base Xcode' "$HOST_TOOL_UNISTD_BASE_PROBE_LOG"
  if run_host_tool_unistd_probe "$SDK_DIR" "$HOST_TOOL_UNISTD_OVERLAY_PROBE_LOG"; then
    overlay_probe='passed'
  fi

  {
    printf 'host_clang=%s\n' "$HOST_CLANG"
    printf 'host_os_version=%s\n' "$HOST_OS_VERSION"
    printf 'host_sdkroot=%s\n' "$BASE_SDK_DIR"
    printf 'target_sdkroot=%s\n' "$SDK_DIR"
    printf 'sys_unistd_base_sha256=%s\n' "$HOST_SYS_UNISTD_BASE_SHA256"
    printf 'sys_unistd_overlay_sha256=%s\n' "$HOST_SYS_UNISTD_OVERLAY_SHA256"
    printf 'sys_unistd_headers_match=%s\n' "$headers_match"
    printf 'host_tool_base_unistd_probe=passed\n'
    printf 'host_tool_overlay_unistd_probe=%s\n' "$overlay_probe"
    printf 'probe_flags=-isysroot <sdk-root> -mmacosx-version-min=%s -g -O0 -I %s -I %s/SETUP/config -DYY_NO_INPUT -Wall -fsyntax-only -x c\n' \
      "$HOST_OS_VERSION" "$SOURCE_DIR" "$SOURCE_DIR"
  } > "$HOST_SDK_DIAGNOSTIC_LOG"
  if [[ "$headers_match" == false ]]; then
    {
      printf '\n--- sys/unistd.h base/overlay diff (first 100 lines) ---\n'
      diff -u "$base_unistd" "$overlay_unistd" | sed -n '1,100p' || true
    } >> "$HOST_SDK_DIAGNOSTIC_LOG"
  fi
  if [[ "$overlay_probe" == failed ]]; then
    {
      printf '\n--- overlay host-tool unistd probe failure ---\n'
      cat "$HOST_TOOL_UNISTD_OVERLAY_PROBE_LOG"
    } >> "$HOST_SDK_DIAGNOSTIC_LOG"
  fi
  printf 'Host-tool SDK diagnostics: base sys/unistd.h %s; overlay %s; base probe passed, overlay probe %s\n' \
    "$([[ "$headers_match" == true ]] && printf 'matches overlay' || printf 'differs from overlay')" \
    "$HOST_SYS_UNISTD_OVERLAY_SHA256" "$overlay_probe"
  [[ "$headers_match" == true ]] \
    || fail "XNU header staging replaced Xcode public sys/unistd.h; see $HOST_SDK_DIAGNOSTIC_LOG"
}

verify_xnu_host_tool_compile_commands() {
  if ! python3 - "$LOG" "$BASE_SDK_DIR" "$SDK_DIR" "$HOST_CLANG" <<'PY'
import pathlib
import re
import shlex
import sys

log_path = pathlib.Path(sys.argv[1])
base_sdk = pathlib.Path(sys.argv[2]).resolve()
target_sdk = pathlib.Path(sys.argv[3]).resolve()
host_clang = pathlib.Path(sys.argv[4]).resolve()
ansi = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
required_sources = ("kextsymboltool.c", "mkheaders.c")
commands = {source: [] for source in required_sources}
for raw_line in log_path.read_text(errors="replace").splitlines():
    line = ansi.sub("", raw_line)
    try:
        tokens = shlex.split(line)
    except ValueError:
        continue
    if not tokens:
        continue
    for source in required_sources:
        if any(token == source or token.endswith("/" + source) for token in tokens):
            if "-c" in tokens:
                commands[source].append(tokens)

for source, invocations in commands.items():
    if not invocations:
        raise SystemExit(f"missing verbose host-tool clang invocation for {source} in {log_path}")
    for tokens in invocations:
        if pathlib.Path(tokens[0]).resolve() != host_clang:
            raise SystemExit(f"{source} was not compiled by the pinned host clang: {tokens[0]}")
        roots = [
            str(pathlib.Path(tokens[i + 1]).resolve())
            for i, token in enumerate(tokens[:-1]) if token == "-isysroot"
        ]
        if roots != [str(base_sdk)]:
            raise SystemExit(f"{source} host clang sysroots are {roots}; expected {base_sdk}")
        if str(target_sdk) in roots:
            raise SystemExit(f"{source} host clang unexpectedly uses XNU target overlay {target_sdk}")
        if "-g" not in tokens or "-O0" not in tokens:
            raise SystemExit(f"{source} command is missing XNU's host-tool debug flags")
        if source == "mkheaders.c" and "-DYY_NO_INPUT" not in tokens:
            raise SystemExit("mkheaders.c command is missing XNU's YY_NO_INPUT define")
    print(f"Verified {source}: host clang uses registered Xcode sysroot {base_sdk}")
PY
  then
    fail 'XNU verbose host-tool compile commands did not use the registered Xcode SDK'
  fi
}

verify_xnu_target_compile_commands() {
  if ! python3 "$SCRIPT_DIR/verify-xnu-target-build.py" \
    --log "$LOG" --objroot "$OBJROOT" --sdk-root "$SDK_DIR" \
    --base-sdk-root "$BASE_SDK_DIR" --source-root "$SOURCE_DIR" \
    --diagnostic "$XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG" \
    --clang "$CLANG" --clangxx "$CXX_CLANG"; then
    fail "XNU target include commands/dependencies are inconsistent; see $XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG"
  fi
  XNU_TARGET_INCLUDE_PATHS_VERIFIED='true'
  XNU_GENERIC_SDK_HEADER_DEPENDENCY_COUNT="$(awk -F= '$1 == "excluded_sdk_header_dependencies" { print $2 }' "$XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG")"
  XNU_TARGET_DEPENDENCY_FILE_COUNT="$(awk -F= '$1 == "target_dependency_files" { print $2 }' "$XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG")"
  XNU_DEPENDENCY_FILE_ENTRY_COUNT="$(awk -F= '$1 == "target_dependency_file_entries" { print $2 }' "$XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG")"
  XNU_HEADER_DEPENDENCY_COUNT="$(awk -F= '$1 == "target_header_dependencies" { print $2 }' "$XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG")"
  XNU_MISSING_DEPENDENCY_FILE_COUNT="$(awk -F= '$1 == "missing_dependency_files" { print $2 }' "$XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG")"
  XNU_TARGET_SYSROOT_MODE="$(awk -F= '$1 == "target_sdkroot_resolution_mode" { print $2 }' "$XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG")"
  XNU_TARGET_SDKROOT_OVERLAY_NOTE_COUNT="$(awk -F= '$1 == "sdkroot_overlay_notes" { print $2 }' "$XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG")"
  XNU_TARGET_SDKROOT_BASE_NOTE_COUNT="$(awk -F= '$1 == "sdkroot_base_notes" { print $2 }' "$XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG")"
  XNU_TARGET_NO_EXPLICIT_SYSROOT_COMMAND_COUNT="$(awk -F= '$1 == "target_commands_without_explicit_isysroot" { print $2 }' "$XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG")"
  XNU_OVERLAY_HEADER_DEPENDENCY_COUNT="$(awk -F= '$1 == "overlay_header_dependencies" { print $2 }' "$XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG")"
  [[ "$XNU_GENERIC_SDK_HEADER_DEPENDENCY_COUNT" =~ ^[0-9]+$ && \
     "$XNU_TARGET_DEPENDENCY_FILE_COUNT" =~ ^[1-9][0-9]*$ && \
     "$XNU_DEPENDENCY_FILE_ENTRY_COUNT" =~ ^[1-9][0-9]*$ && \
     "$XNU_HEADER_DEPENDENCY_COUNT" =~ ^[1-9][0-9]*$ && \
     "$XNU_MISSING_DEPENDENCY_FILE_COUNT" == 0 && \
     "$XNU_TARGET_SYSROOT_MODE" == explicit-isysroot-overlay && \
     "$XNU_TARGET_SDKROOT_OVERLAY_NOTE_COUNT" =~ ^[0-9]+$ && \
     "$XNU_TARGET_SDKROOT_BASE_NOTE_COUNT" =~ ^[0-9]+$ && \
     "$XNU_TARGET_NO_EXPLICIT_SYSROOT_COMMAND_COUNT" =~ ^[0-9]+$ && \
     "$XNU_OVERLAY_HEADER_DEPENDENCY_COUNT" =~ ^[1-9][0-9]*$ ]] \
    || fail "XNU SDK-root and dependency diagnostics are incomplete or invalid in $XNU_TARGET_INCLUDE_DIAGNOSTIC_LOG"
}

verify_firehose_header_closure() {
  if ! printf '#define KERNEL_PRIVATE 1\n#include <machine/cpu_number.h>\n' |
    xcrun clang -target x86_64-apple-macosx26.0 -isysroot "$SDK_DIR" \
      -I "$SDK_DIR/usr/local/include" -fsyntax-only -x c -; then
    fail 'Firehose XNU machine-header include closure does not compile for x86_64'
  fi
}

verify_firehose_compile_arguments() {
  if ! python3 - "$LIBDISPATCH_LOG" "$LIBDISPATCH_BUILD_DIR/obj" "$SDK_DIR" "$BASE_SDK_DIR" <<'PY'
import os
import pathlib
import shlex
import sys
import tempfile

log_path = pathlib.Path(sys.argv[1])
objroot = pathlib.Path(sys.argv[2]).resolve()
sdk_root = pathlib.Path(sys.argv[3]).resolve()
base_sdk_root = pathlib.Path(sys.argv[4]).resolve()
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


def expand_response_arguments(args, response_path, response_args):
    expanded = []
    response_count = 0
    for arg in args:
        if arg.startswith("@"):
            if pathlib.Path(arg[1:]).resolve() != response_path:
                raise SystemExit(f"unexpected Firehose compiler response file: {arg[1:]}")
            response_count += 1
            expanded.extend(response_args)
        else:
            expanded.append(arg)
    if response_count != 1:
        raise SystemExit(f"expected one Firehose response-file reference, found {response_count}")
    return expanded


def verify_sdk_and_overlay_search_paths(args, overlay_root, base_sdk_root):
    sysroots = []
    for index, arg in enumerate(args):
        if arg == "-isysroot" and index + 1 < len(args):
            sysroots.append(args[index + 1])
        elif arg.startswith("-isysroot") and arg != "-isysroot":
            # Clang's joined form is -isysroot<dir>; preserve '=' as part of
            # the path so an unsupported -isysroot=<dir> cannot false-pass.
            sysroots.append(arg[len("-isysroot"):])
    if not sysroots or any(pathlib.Path(path).resolve() != base_sdk_root for path in sysroots):
        raise SystemExit(f"Firehose compile uses SDKROOT {sysroots}; expected only {base_sdk_root}")

    include_paths = []
    index = 0
    while index < len(args):
        arg = args[index]
        if arg == "-I" and index + 1 < len(args):
            include_paths.append(args[index + 1])
            index += 1
        elif arg.startswith("-I") and len(arg) > 2:
            include_paths.append(arg[2:])
        index += 1

    resolved_include_paths = [pathlib.Path(path).resolve() for path in include_paths]
    required_overlay_paths = {
        overlay_root / "System/Library/Frameworks/Kernel.framework/PrivateHeaders",
        overlay_root / "System/Library/Frameworks/Kernel.framework/Headers",
        overlay_root / "usr/local/include",
        overlay_root / "usr/local/include/os",
        overlay_root / "usr/local/include/firehose",
    }
    resolved_required = {path.resolve() for path in required_overlay_paths}
    missing = sorted(str(path) for path in resolved_required - set(resolved_include_paths))
    if missing:
        raise SystemExit(f"Firehose compiler is missing staged overlay include paths: {missing}")

    for suffix in (
        "System/Library/Frameworks/Kernel.framework/PrivateHeaders",
        "System/Library/Frameworks/Kernel.framework/Headers",
    ):
        overlay_path = (overlay_root / suffix).resolve()
        base_path = (base_sdk_root / suffix).resolve()
        overlay_positions = [i for i, path in enumerate(resolved_include_paths) if path == overlay_path]
        base_positions = [i for i, path in enumerate(resolved_include_paths) if path == base_path]
        if not overlay_positions or not base_positions:
            raise SystemExit(
                f"Firehose compile must search both staged and base Kernel.framework headers: {suffix}"
            )
        if min(overlay_positions) >= min(base_positions):
            raise SystemExit(
                f"staged Kernel.framework headers do not precede base SDK headers: {suffix}"
            )


# Exercise tokenization and the full response-file path check with the escaped
# Xcode path and hash-prefixed response-file names produced by Xcode builds.
with tempfile.TemporaryDirectory() as fixture_dir:
    fixture_objroot = pathlib.Path(fixture_dir).resolve()
    fixture_response = fixture_objroot / "b52759a1-common-args.resp"
    fixture_base_sdk = fixture_objroot / "Xcode SDK with spaces"
    fixture_overlay = fixture_objroot / "staged SDK with spaces"
    framework_suffixes = (
        "System/Library/Frameworks/Kernel.framework/PrivateHeaders",
        "System/Library/Frameworks/Kernel.framework/Headers",
    )
    fixture_includes = []
    for suffix in framework_suffixes:
        fixture_includes.extend(("-I", str(fixture_overlay / suffix)))
    for suffix in (
        "usr/local/include",
        "usr/local/include/os",
        "usr/local/include/firehose",
        *framework_suffixes,
    ):
        fixture_includes.extend(("-I", str(fixture_overlay / suffix)))
    for suffix in framework_suffixes:
        fixture_includes.extend(("-I", str(fixture_base_sdk / suffix)))
    fixture_arguments = ["-isysroot", str(fixture_base_sdk), *fixture_includes]
    fixture_response.write_text(shlex.join(fixture_arguments) + "\n")
    fixture = (
        r"/Applications/Xcode\ 26.6.app/Contents/Developer/Toolchains/"
        r"XcodeDefault.xctoolchain/usr/bin/clang @"
        + shlex.quote(str(fixture_response))
        + " -isysroot " + shlex.quote(str(fixture_base_sdk))
        + r" -I/tmp/include\ path"
    )
    fixture_tokens = shlex.split(fixture)
    if pathlib.PurePath(fixture_tokens[0]).name != "clang":
        raise SystemExit("could not parse a clang command with an escaped Xcode path")
    fixture_path = resolve_response_file(fixture_tokens[1:], fixture_objroot)
    if fixture_path != fixture_response.resolve():
        raise SystemExit("could not parse an Xcode response file with a hash-prefixed name")
    expanded_fixture = expand_response_arguments(
        fixture_tokens[1:], fixture_path, shlex.split(fixture_path.read_text())
    )
    verify_sdk_and_overlay_search_paths(
        expanded_fixture, fixture_overlay, fixture_base_sdk
    )

if len(compile_commands) != 1:
    raise SystemExit(
        f"expected one logged clang command for firehose_buffer.c, found {len(compile_commands)}"
    )
_, compile_tokens, clang_index = compile_commands[0]
command_args = compile_tokens[clang_index + 1:]
response_path = resolve_response_file(command_args, objroot)

response_args = shlex.split(response_path.read_text(errors="replace"))
effective_args = expand_response_arguments(command_args, response_path, response_args)
verify_sdk_and_overlay_search_paths(effective_args, sdk_root, base_sdk_root)

print(f"Verified Firehose compiler response file: {response_path}")
print(f"Verified Firehose base SDKROOT: {base_sdk_root}")
print(f"Verified staged XNU Kernel.framework precedence and include paths: {sdk_root}")
PY
  then
    fail 'Firehose compile command did not use the Xcode base SDK and staged XNU header overlay'
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
  HOST_CLANG="$(xcrun --sdk macosx --find clang)"
  [[ -x "$HOST_CLANG" ]] || fail 'Xcode did not resolve its host clang'
  HOST_OS_VERSION="$(sw_vers -productVersion)"
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
  verify_kdk
  verify_trustcache_header "$SDK_DIR"
  verify_host_tool_unistd_probe "$BASE_SDK_DIR" 'base Xcode'
  python3 "$SCRIPT_DIR/verify-xnu-kdk.py" --self-test
  python3 "$SCRIPT_DIR/verify-xnu-target-build.py" --self-test
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
  printf 'XNU source release %s, SDK %s, and Xcode %s are ready for X86_64/NONE; KDK status=%s; runtime target is %s and remains unverified.\n' \
    "$XNU_SOURCE_RELEASE" "$SDK_VERSION" "$XCODE_VERSION" \
    "$([[ "$KDK_PRESENT" == true ]] && printf 'matching %s/%s' "$KDK_VERSION" "$KDK_BUILD" || printf 'not required for Intel')" \
    "$RUNTIME_TARGET_VERSION"
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
verify_kdk
verify_trustcache_header "$BASE_SDK_DIR"
python3 "$SCRIPT_DIR/verify-xnu-target-build.py" --self-test
python3 "$SCRIPT_DIR/verify-xnu-kdk.py" --self-test
: > "$HOST_TOOL_UNISTD_BASE_PROBE_LOG"
verify_host_tool_unistd_probe "$BASE_SDK_DIR" 'base Xcode' "$HOST_TOOL_UNISTD_BASE_PROBE_LOG"
CLANG="$(xcrun --find clang)"
CXX_CLANG="$(xcrun --find clang++)"
LD="$(xcrun --find ld)"
IIG="$(xcrun --find iig)"
verify_xnu_warning_policy
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
  SDKROOT=macosx SDKROOT_RESOLVED="$SDK_DIR" HOST_SDKROOT=macosx HOST_SDKROOT_RESOLVED="$BASE_SDK_DIR" \
  SDKVERSION="$SDK_VERSION" KDKROOT="$KDKROOT" PLATFORM=MacOSX \
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
  SDKROOT=macosx SDKROOT_RESOLVED="$SDK_DIR" HOST_SDKROOT=macosx HOST_SDKROOT_RESOLVED="$BASE_SDK_DIR" \
  SDKVERSION="$SDK_VERSION" KDKROOT="$KDKROOT" PLATFORM=MacOSX \
  ARCH_CONFIGS=X86_64 MACHINE_CONFIGS=NONE KERNEL_CONFIGS=DEVELOPMENT \
  RC_DARWIN_KERNEL_VERSION="$KERNEL_VERSION" RC_ProjectSourceVersion="$SOURCE_VERSION" \
  OBJROOT="$XNU_HEADERS_OBJROOT" SYMROOT="$XNU_HEADERS_BUILD_DIR/sym" DSTROOT="$XNU_HEADERS_DSTROOT" \
  MAKEJOBS="-j$JOBS" >"$XNU_HEADERS_LOG" 2>&1; then
  tail -n 20 "$XNU_HEADERS_LOG"
else
  tail -n 100 "$XNU_HEADERS_LOG" >&2
  fail "XNU installhdrs failed; full log: $XNU_HEADERS_LOG"
fi
XNU_KERNEL_FRAMEWORK_DSTROOT="$XNU_HEADERS_DSTROOT/System/Library/Frameworks/Kernel.framework"
XNU_PRIVATE_INCLUDE_DSTROOT="$XNU_HEADERS_DSTROOT/usr/local/include"
[[ -d "$XNU_KERNEL_FRAMEWORK_DSTROOT" ]] \
  || fail 'XNU installhdrs did not produce Kernel.framework headers'
[[ -d "$XNU_PRIVATE_INCLUDE_DSTROOT" ]] \
  || fail 'XNU installhdrs did not produce usr/local/include private headers'
# Merge only XNU's private trees while preserving base SDK headers used by
# XNU. The X86_64/NONE build omits generic SDK roots that are empty and unused.
mkdir -p "$SDK_DIR/System/Library/Frameworks" "$SDK_DIR/usr/local"
rsync -a "$XNU_KERNEL_FRAMEWORK_DSTROOT/" "$XNU_KERNEL_FRAMEWORK_DIR/"
rsync -a "$XNU_PRIVATE_INCLUDE_DSTROOT/" "$SDK_DIR/usr/local/include/"
record_xnu_header_tree_diagnostics
[[ -s "$SDK_DIR/usr/local/include/os/atomic_private.h" ]] \
  || fail 'XNU installhdrs did not stage usr/local/include/os/atomic_private.h'
[[ -s "$SDK_DIR/usr/local/include/firehose/tracepoint_private.h" ]] \
  || fail 'XNU installhdrs did not stage usr/local/include/firehose/tracepoint_private.h'
[[ -d "$XNU_KERNEL_PRIVATE_HEADERS" ]] \
  || fail 'XNU installhdrs did not stage Kernel.framework/PrivateHeaders'
[[ -d "$XNU_KERNEL_HEADERS" ]] \
  || fail 'XNU installhdrs did not stage Kernel.framework/Headers'
[[ -x "$SDK_DIR/usr/local/libexec/availability.pl" ]] \
  || fail 'XNU header installation removed AvailabilityVersions availability.pl'
mkdir -p "$SDK_DIR/usr/local/include/machine" "$SDK_DIR/usr/local/include/i386"
cp "$XNU_EXPORTED_OSFMK_HEADERS_DIR/machine/cpu_number.h" "$SDK_DIR/usr/local/include/machine/cpu_number.h"
cp "$XNU_EXPORTED_OSFMK_HEADERS_DIR/i386/cpu_number.h" "$SDK_DIR/usr/local/include/i386/cpu_number.h"
verify_machine_header_exports
verify_firehose_header_closure
verify_public_availability_header
verify_trustcache_header "$SDK_DIR"
record_host_sdk_diagnostics

mkdir -p "$LIBDISPATCH_BUILD_DIR/obj" "$LIBDISPATCH_BUILD_DIR/sym" "$LIBDISPATCH_DSTROOT"
printf 'Building libfirehose_kernel from %s for x86_64\n' "$LIBDISPATCH_TAG"
if xcodebuild \
  -project "$LIBDISPATCH_SOURCE_DIR/libdispatch.xcodeproj" \
  -target libfirehose_kernel -configuration Release -sdk macosx \
  ARCHS=x86_64 VALID_ARCHS=x86_64 PRODUCT_NAME=firehose_kernel \
  HEADER_SEARCH_PATHS="$XNU_KERNEL_PRIVATE_HEADERS $XNU_KERNEL_HEADERS $SDK_DIR/usr/local/include $SDK_DIR/usr/local/include/os $SDK_DIR/usr/local/include/firehose \$(inherited)" \
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
host_sdkroot=$BASE_SDK_DIR
host_sys_unistd_base_sha256=$HOST_SYS_UNISTD_BASE_SHA256
host_sys_unistd_overlay_sha256=$HOST_SYS_UNISTD_OVERLAY_SHA256
host_sys_unistd_headers_match=$([[ "$HOST_SYS_UNISTD_BASE_SHA256" == "$HOST_SYS_UNISTD_OVERLAY_SHA256" ]] && printf true || printf false)
host_tool_base_unistd_probe=passed
kdk_root=$KDKROOT
kdk_status=$([[ "$KDK_PRESENT" == true ]] && printf verified || printf not-present-not-required-for-intel)
kdk_present=$KDK_PRESENT
kdk_required=$KDK_REQUIRED
kdk_expected_version=$KDK_VERSION
kdk_expected_build=$KDK_BUILD
kdk_product_version=$KDK_PRODUCT_VERSION
kdk_product_build=$KDK_PRODUCT_BUILD
kdk_xnu_version=$KDK_XNU_VERSION
kdk_matches_xnu_source=$([[ "$KDK_PRESENT" == true && "$KDK_XNU_VERSION" == "$SOURCE_VERSION" ]] && printf true || printf false)
kdk_trustcache_api_header_sha256=$KDK_PRIVATE_HEADER_SHA256
trustcache_header_source=$TRUSTCACHE_HEADER_SOURCE
trustcache_header_path=$TRUSTCACHE_HEADER
trustcache_header_sha256=$TRUSTCACHE_HEADER_SHA256
xnu_sdk_header_tree_diagnostics_recorded=$XNU_HEADER_TREE_DIAGNOSTICS_RECORDED
xnu_target_include_paths_verified=$XNU_TARGET_INCLUDE_PATHS_VERIFIED
xnu_generic_sdk_header_dependencies=$XNU_GENERIC_SDK_HEADER_DEPENDENCY_COUNT
xnu_target_dependency_files=$XNU_TARGET_DEPENDENCY_FILE_COUNT
xnu_dependency_file_entries=$XNU_DEPENDENCY_FILE_ENTRY_COUNT
xnu_header_dependencies=$XNU_HEADER_DEPENDENCY_COUNT
xnu_missing_dependency_files=$XNU_MISSING_DEPENDENCY_FILE_COUNT
xnu_target_include_path_policy=KDK Kernel.framework private headers first when supplied; otherwise SDK Kernel.framework private headers; keep \$SDKROOT/usr/local/include; omit generic kernel/iBoot/AppleFeatures/platform SDK roots for X86_64/NONE
xnu_target_sysroot_policy=explicit -isysroot to isolated SDK overlay for every target clang compile
xnu_warning_policy_verified=$XNU_WARNING_POLICY_VERIFIED
xnu_compiler_reserved_identifier_warning=-Wno-error=reserved-identifier
xnu_compiler_unused_but_set_variable_warning=-Wno-error=unused-but-set-variable
xnu_compiler_void_pointer_to_enum_cast_warning=-Wno-error=void-pointer-to-enum-cast
xnu_general_warning_policy=-Werror
xnu_target_sdkroot_resolution_mode=$XNU_TARGET_SYSROOT_MODE
xnu_target_sdkroot_overlay_notes=$XNU_TARGET_SDKROOT_OVERLAY_NOTE_COUNT
xnu_target_sdkroot_base_notes=$XNU_TARGET_SDKROOT_BASE_NOTE_COUNT
xnu_target_commands_without_explicit_isysroot=$XNU_TARGET_NO_EXPLICIT_SYSROOT_COMMAND_COUNT
xnu_overlay_header_dependencies=$XNU_OVERLAY_HEADER_DEPENDENCY_COUNT
sdk_version=$SDK_VERSION
xcode_version=$XCODE_VERSION
host_os_version=$HOST_OS_VERSION
xnu_source_release=$XNU_SOURCE_RELEASE
runtime_target_os_version=$RUNTIME_TARGET_VERSION
runtime_compatibility_verified=false
kernel_source_runtime_match=false
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
rm -rf -- "$OBJROOT" "$SYMROOT" "$DSTROOT"
# Xcode 26.6 enables three source-level warning classes for the pinned x86
# target that are non-actionable there: reserved internal identifiers, dead
# stores (including ARM-only trace arguments), and the intentional enum-as-
# opaque-sysctl-argument round trip. Keep the warnings visible and all other
# warnings fatal; apply these exceptions only to X86_64/NONE.
if make -C "$SOURCE_DIR" build \
  SDKROOT=macosx SDKROOT_RESOLVED="$SDK_DIR" HOST_SDKROOT=macosx HOST_SDKROOT_RESOLVED="$BASE_SDK_DIR" \
  SDKVERSION="$SDK_VERSION" KDKROOT="$KDKROOT" PLATFORM=MacOSX \
  OTHER_CFLAGS='$(RC_NONARCH_CFLAGS) -isysroot $(SDKROOT)' \
  ARCH_CONFIGS=X86_64 MACHINE_CONFIGS=NONE KERNEL_CONFIGS=DEVELOPMENT \
  INCFLAGS_SDK='$(if $(KDKROOT),-I$(KDKROOT)/$(KPINCDIR),-I$(SDKROOT)/$(KPINCDIR)) -I$(SDKROOT)/usr/local/include $(if $(KDKROOT),-I$(KDKROOT)/System/Library/KernelSupport) -Wno-error=reserved-identifier -Wno-error=unused-but-set-variable -Wno-error=void-pointer-to-enum-cast' \
  INCFLAGS_PLATFORM='$(if $(KDKROOT),-I$(KDKROOT)/$(KINCDIR)/platform -I$(KDKROOT)/$(KPINCDIR)/platform)' \
  RC_DARWIN_KERNEL_VERSION="$KERNEL_VERSION" RC_ProjectSourceVersion="$SOURCE_VERSION" \
  OBJROOT="$OBJROOT" SYMROOT="$SYMROOT" DSTROOT="$DSTROOT" VERBOSE=YES \
  MAKEJOBS="-j$JOBS" >"$LOG" 2>&1; then
  tail -n 20 "$LOG"
else
  tail -n 100 "$LOG" >&2
  fail "XNU build failed; see $LOG"
fi
verify_xnu_host_tool_compile_commands
verify_xnu_target_compile_commands

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
  "$HOST_ARCH" "$BUILD_HOST_MODE" "$HOST_PROCESS_ARCH" "$HOST_PROCESS_TRANSLATED" \
  "$BASE_SDK_DIR" "$HOST_SYS_UNISTD_BASE_SHA256" "$HOST_SYS_UNISTD_OVERLAY_SHA256" \
  "$XNU_HEADER_TREE_DIAGNOSTICS_RECORDED" "$XNU_TARGET_INCLUDE_PATHS_VERIFIED" \
  "$XNU_GENERIC_SDK_HEADER_DEPENDENCY_COUNT" "$XNU_TARGET_DEPENDENCY_FILE_COUNT" \
  "$XNU_DEPENDENCY_FILE_ENTRY_COUNT" "$XNU_HEADER_DEPENDENCY_COUNT" \
  "$XNU_MISSING_DEPENDENCY_FILE_COUNT" "$XNU_WARNING_POLICY_VERIFIED" \
  "$XNU_TARGET_SYSROOT_MODE" "$XNU_TARGET_SDKROOT_OVERLAY_NOTE_COUNT" \
  "$XNU_TARGET_SDKROOT_BASE_NOTE_COUNT" "$XNU_TARGET_NO_EXPLICIT_SYSROOT_COMMAND_COUNT" \
  "$XNU_OVERLAY_HEADER_DEPENDENCY_COUNT" "$KDKROOT" "$KDK_VERSION" "$KDK_BUILD" \
  "$KDK_PRIVATE_HEADER_SHA256" "$HOST_OS_VERSION" "$XNU_SOURCE_RELEASE" "$RUNTIME_TARGET_VERSION" \
  "$KDK_PRESENT" "$KDK_REQUIRED" "$KDK_PRODUCT_VERSION" "$KDK_PRODUCT_BUILD" "$KDK_XNU_VERSION" \
  "$TRUSTCACHE_HEADER_SOURCE" "$TRUSTCACHE_HEADER_SHA256" "$TRUSTCACHE_HEADER" <<'PY'
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
    "host_sdkroot": sys.argv[23],
    "host_sys_unistd_base_sha256": sys.argv[24],
    "host_sys_unistd_overlay_sha256": sys.argv[25],
    "host_sys_unistd_headers_match": sys.argv[24] == sys.argv[25],
    "host_tool_base_unistd_probe": "passed",
    "xnu_sdk_header_tree_diagnostics_recorded": sys.argv[26] == "true",
    "xnu_target_include_paths_verified": sys.argv[27] == "true",
    "xnu_generic_sdk_header_paths_removed_for_x86_64": True,
    "xnu_generic_sdk_header_dependencies": int(sys.argv[28]),
    "xnu_target_dependency_files": int(sys.argv[29]),
    "xnu_dependency_file_entries": int(sys.argv[30]),
    "xnu_header_dependencies": int(sys.argv[31]),
    "xnu_missing_dependency_files": int(sys.argv[32]),
    "xnu_warning_policy_verified": sys.argv[33] == "true",
    "xnu_reserved_identifier_warning_nonfatal": True,
    "xnu_unused_but_set_variable_warning_nonfatal": True,
    "xnu_void_pointer_to_enum_cast_warning_nonfatal": True,
    "xnu_other_warnings_remain_errors": True,
    "xnu_compiler_warning_exceptions": [
        "-Wno-error=reserved-identifier",
        "-Wno-error=unused-but-set-variable",
        "-Wno-error=void-pointer-to-enum-cast",
    ],
    "xnu_target_sdkroot_resolution_mode": sys.argv[34],
    "xnu_target_sdkroot_overlay_notes": int(sys.argv[35]),
    "xnu_target_sdkroot_base_notes": int(sys.argv[36]),
    "xnu_target_commands_without_explicit_isysroot": int(sys.argv[37]),
    "xnu_overlay_header_dependencies": int(sys.argv[38]),
    "xnu_target_include_path_policy": "use matching KDK Kernel.framework private headers when supplied, otherwise use SDK Kernel.framework private headers; keep SDKROOT/usr/local/include; omit generic kernel/iBoot/AppleFeatures/platform SDK roots for X86_64/NONE; retain KDK platform paths",
    "xnu_target_sysroot_policy": "explicit -isysroot to isolated SDK overlay for every target clang compile",
    "kdk_root": sys.argv[39] or None,
    "kdk_status": "verified" if sys.argv[46] == "true" else "not-present-not-required-for-intel",
    "kdk_expected_version": sys.argv[40],
    "kdk_expected_build": sys.argv[41],
    "kdk_present": sys.argv[46] == "true",
    "kdk_required_for_build_host": sys.argv[47] == "true",
    "kdk_product_version": sys.argv[48] or None,
    "kdk_product_build": sys.argv[49] or None,
    "kdk_xnu_version": sys.argv[50] or None,
    "kdk_matches_xnu_source": sys.argv[46] == "true" and sys.argv[50] == sys.argv[5],
    "kdk_trustcache_api_header_sha256": sys.argv[42] or None,
    "trustcache_header_source": sys.argv[51],
    "trustcache_header_sha256": sys.argv[52],
    "trustcache_header_path": sys.argv[53],
    "host_os_version": sys.argv[43],
    "xnu_commit": sys.argv[3],
    "xnu_tag": sys.argv[4],
    "xnu_source_version": sys.argv[5],
    "xnu_source_release": sys.argv[44],
    "runtime_target_os_version": sys.argv[45],
    "runtime_compatibility_verified": False,
    "kernel_source_runtime_match": False,
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
