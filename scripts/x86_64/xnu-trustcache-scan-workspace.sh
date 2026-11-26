#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

xnu_trustcache_scan_workspace() {
  local build_root="$1"
  local mode="$2"
  case "$mode" in
    check)
      mktemp -d "${TMPDIR:-/tmp}/xnulinux-trustcache-scan.XXXXXX"
      ;;
    build)
      printf '%s\n' "$build_root"
      ;;
    *)
      printf 'unknown TrustCache scan workspace mode: %s\n' "$mode" >&2
      return 2
      ;;
  esac
}

xnu_trustcache_scan_workspace_cleanup() {
  local mode="$1"
  local scan_root="$2"
  if [[ "$mode" == check ]]; then
    rm -rf -- "$scan_root"
  fi
}

xnu_trustcache_scan_workspace_report_failure() {
  local mode="$1"
  local candidates_file="$2"
  local errors_file="$3"
  local diagnostic_target="$4"
  if [[ "$mode" == check ]]; then
    {
      printf 'trustcache_header_scan_status=failed\ntrustcache_header_scan_candidates_begin\n'
      cat "$candidates_file" || printf '[candidate list could not be read]\n'
      printf 'trustcache_header_scan_errors_begin\n'
      cat "$errors_file" || printf '[scan error log could not be read]\n'
      printf 'trustcache_header_scan_diagnostics_end\n'
    } >> "$diagnostic_target"
  else
    printf 'trustcache_header_scan_status=failed\ntrustcache_header_scan_candidates=%s\ntrustcache_header_scan_errors=%s\n' \
      "$candidates_file" "$errors_file" >> "$diagnostic_target"
    [[ ! -s "$errors_file" ]] || cat "$errors_file" >&2
  fi
}

xnu_trustcache_scan_workspace_self_test() {
  local scratch_root build_root temp_root scan_root candidates_file errors_file diagnostics_file
  scratch_root="$(mktemp -d "${TMPDIR:-/tmp}/xnulinux-readonly-check-test.XXXXXX")" \
    || { printf 'could not create self-test scratch directory\n' >&2; return 1; }
  trap 'chmod u+w "$build_root" 2>/dev/null || true; rm -rf -- "$scratch_root"' RETURN

  build_root="$scratch_root/build"
  temp_root="$scratch_root/tmp"
  mkdir -p "$build_root" "$temp_root"
  chmod 555 "$build_root"
  chmod 700 "$temp_root"

  TMPDIR="$temp_root"
  export TMPDIR
  scan_root="$(xnu_trustcache_scan_workspace "$build_root" check)"
  candidates_file="$scan_root/trustcache-sdk-header-candidates.txt"
  errors_file="$scan_root/trustcache-sdk-header-scan-errors.log"
  diagnostics_file="$scratch_root/check-diagnostics.log"
  [[ "$scan_root" == "$temp_root/"* ]] \
    || { printf 'check workspace was not placed under TMPDIR: %s\n' "$scan_root" >&2; return 1; }
  printf '%s\n' '/fake/sdk/System/Library/Frameworks/Kernel.framework/PrivateHeaders/TrustCache/API.h' > "$candidates_file"
  printf '%s\n' 'simulated find permission failure' > "$errors_file"
  python3 - "$build_root" <<'PY'
import os
import stat
import sys

mode = stat.S_IMODE(os.stat(sys.argv[1]).st_mode)
assert mode & 0o222 == 0, oct(mode)
PY
  [[ -w "$temp_root" ]] \
    || { printf 'TMPDIR is not writable: %s\n' "$temp_root" >&2; return 1; }
  : > "$temp_root/writable-probe"
  xnu_trustcache_scan_workspace_report_failure check "$candidates_file" "$errors_file" "$diagnostics_file"
  grep -Fq '/fake/sdk/System/Library/Frameworks/Kernel.framework/PrivateHeaders/TrustCache/API.h' "$diagnostics_file" \
    || { printf 'check diagnostics omitted the candidate list before cleanup\n' >&2; return 1; }
  grep -Fq 'simulated find permission failure' "$diagnostics_file" \
    || { printf 'check diagnostics omitted the scan error before cleanup\n' >&2; return 1; }
  ! grep -Fq "$scan_root" "$diagnostics_file" \
    || { printf 'check diagnostics point at temporary files that will be removed\n' >&2; return 1; }
  [[ -z "$(find "$build_root" -mindepth 1 -maxdepth 1 -print -quit)" ]] \
    || { printf 'read-only BUILD_ROOT was modified\n' >&2; return 1; }

  xnu_trustcache_scan_workspace_cleanup check "$scan_root"
  [[ ! -e "$scan_root" ]] \
    || { printf 'check workspace was not cleaned: %s\n' "$scan_root" >&2; return 1; }
  [[ "$(xnu_trustcache_scan_workspace "$build_root" build)" == "$build_root" ]] \
    || { printf 'build-mode diagnostics no longer use BUILD_ROOT\n' >&2; return 1; }

  printf 'read-only --check workspace self-test passed: BUILD_ROOT unchanged; TMPDIR writable; scan failures are printed before temporary files are removed\n'
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  set -Eeuo pipefail
  case "${1-}" in
    --self-test)
      xnu_trustcache_scan_workspace_self_test
      ;;
    *)
      printf 'Usage: %s --self-test\n' "$0" >&2
      exit 2
      ;;
  esac
fi
