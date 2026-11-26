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

xnu_trustcache_scan_sdk_headers() {
  local sdk_root="$1"
  local build_root="$2"
  local diagnostic_target="$3"
  local scan_mode='build'
  (( CHECK_ONLY )) && scan_mode='check'

  XNU_TRUSTCACHE_SCAN_MODE="$scan_mode"
  XNU_TRUSTCACHE_SCAN_ROOT="$(xnu_trustcache_scan_workspace "$build_root" "$scan_mode")" \
    || { printf 'cannot create the TrustCache SDK scan workspace in %s mode\n' "$scan_mode" >&2; return 2; }
  XNU_TRUSTCACHE_SCAN_CANDIDATES_FILE="$XNU_TRUSTCACHE_SCAN_ROOT/trustcache-sdk-header-candidates.txt"
  XNU_TRUSTCACHE_SCAN_ERRORS_FILE="$XNU_TRUSTCACHE_SCAN_ROOT/trustcache-sdk-header-scan-errors.log"
  local sorted_candidates_file="$XNU_TRUSTCACHE_SCAN_ROOT/trustcache-sdk-header-candidates.sorted.txt"
  local scan_status='complete'

  : > "$XNU_TRUSTCACHE_SCAN_CANDIDATES_FILE"
  : > "$sorted_candidates_file"
  : > "$XNU_TRUSTCACHE_SCAN_ERRORS_FILE"
  if ! find -L "$sdk_root" -type f -path '*/TrustCache/API.h' -print \
    > "$XNU_TRUSTCACHE_SCAN_CANDIDATES_FILE" 2> "$XNU_TRUSTCACHE_SCAN_ERRORS_FILE"; then
    scan_status='failed'
  elif ! LC_ALL=C sort "$XNU_TRUSTCACHE_SCAN_CANDIDATES_FILE" \
    > "$sorted_candidates_file" 2>> "$XNU_TRUSTCACHE_SCAN_ERRORS_FILE"; then
    scan_status='failed'
  elif [[ -s "$XNU_TRUSTCACHE_SCAN_ERRORS_FILE" ]]; then
    scan_status='failed'
  fi

  if [[ "$scan_status" != complete ]]; then
    xnu_trustcache_scan_workspace_report_failure \
      "$scan_mode" "$XNU_TRUSTCACHE_SCAN_CANDIDATES_FILE" "$XNU_TRUSTCACHE_SCAN_ERRORS_FILE" "$diagnostic_target"
    xnu_trustcache_scan_workspace_cleanup "$scan_mode" "$XNU_TRUSTCACHE_SCAN_ROOT"
    return 1
  fi

  mv "$sorted_candidates_file" "$XNU_TRUSTCACHE_SCAN_CANDIDATES_FILE"
}

xnu_trustcache_scan_workspace_self_test() {
  local scratch_root build_root='' temp_root sdk_root fake_bin scan_root diagnostics_file scan_status
  local PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}"
  local CHECK_ONLY=1
  scratch_root="$(mktemp -d "${TMPDIR:-/tmp}/xnulinux-readonly-check-test.XXXXXX")" \
    || { printf 'could not create self-test scratch directory\n' >&2; return 1; }
  trap '[[ -z "$build_root" ]] || chmod u+w "$build_root" 2>/dev/null || true; rm -rf -- "$scratch_root"' RETURN

  build_root="$scratch_root/build"
  temp_root="$scratch_root/tmp"
  sdk_root="$scratch_root/sdk"
  fake_bin="$scratch_root/bin"
  diagnostics_file="$scratch_root/check-diagnostics.log"
  mkdir -p "$build_root" "$temp_root" "$sdk_root" "$fake_bin"
  chmod 555 "$build_root"
  chmod 700 "$temp_root"

  TMPDIR="$temp_root"
  export TMPDIR
  cat > "$fake_bin/find" <<'FIND'
#!/bin/sh
printf '%s\n' '/fake/sdk/System/Library/Frameworks/Kernel.framework/PrivateHeaders/TrustCache/API.h'
printf '%s\n' 'simulated find permission failure' >&2
exit 1
FIND
  chmod +x "$fake_bin/find"
  PATH="$fake_bin:$PATH"
  export PATH
  if xnu_trustcache_scan_sdk_headers "$sdk_root" "$build_root" /dev/stderr 2> "$diagnostics_file"; then
    printf 'self-test expected the simulated TrustCache SDK scan to fail\n' >&2
    return 1
  else
    scan_status=$?
  fi
  scan_root="$XNU_TRUSTCACHE_SCAN_ROOT"
  [[ "$scan_status" -eq 1 ]] \
    || { printf 'scan returned unexpected status %s\n' "$scan_status" >&2; return 1; }
  [[ "$XNU_TRUSTCACHE_SCAN_MODE" == check ]] \
    || { printf 'read-only check selected wrong scan mode: %s\n' "$XNU_TRUSTCACHE_SCAN_MODE" >&2; return 1; }
  [[ "$scan_root" == "$temp_root/"* ]] \
    || { printf 'check scan workspace was not placed under TMPDIR: %s\n' "$scan_root" >&2; return 1; }
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
  grep -Fq '/fake/sdk/System/Library/Frameworks/Kernel.framework/PrivateHeaders/TrustCache/API.h' "$diagnostics_file" \
    || { printf 'captured --check stderr omitted the candidate list before cleanup\n' >&2; return 1; }
  grep -Fq 'simulated find permission failure' "$diagnostics_file" \
    || { printf 'captured --check stderr omitted the scan error before cleanup\n' >&2; return 1; }
  ! grep -Fq "$scan_root" "$diagnostics_file" \
    || { printf 'captured --check stderr points at temporary files that will be removed\n' >&2; return 1; }
  [[ -z "$(ls -A "$build_root")" ]] \
    || { printf 'read-only BUILD_ROOT was modified\n' >&2; return 1; }

  [[ ! -e "$scan_root" ]] \
    || { printf 'check workspace was not cleaned: %s\n' "$scan_root" >&2; return 1; }
  CHECK_ONLY=0
  [[ "$(xnu_trustcache_scan_workspace "$build_root" build)" == "$build_root" ]] \
    || { printf 'build-mode diagnostics no longer use BUILD_ROOT\n' >&2; return 1; }

  printf 'read-only --check scan self-test passed: verifier scan uses TMPDIR, BUILD_ROOT is unchanged, and captured stderr preserves candidates/errors before cleanup\n'
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
