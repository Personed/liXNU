#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

xnu_trustcache_scan_workspace() {
  local build_root="$1"
  local mode="$2"
  case "$mode" in
    check)
      local temp_parent="${TMPDIR:-/tmp}" temp_template temp_workspace='' temp_basename temp_workspace_parent
      [[ "$temp_parent" == / ]] || temp_parent="${temp_parent%/}"
      temp_template="${temp_parent%/}/xnulinux-trustcache-scan.XXXXXX"
      [[ "$temp_parent" != / ]] || temp_template="/xnulinux-trustcache-scan.XXXXXX"
      local temp_status=0
      if temp_workspace="$(mktemp -d "$temp_template")"; then
        printf '%s\n' "$temp_workspace"
      else
        temp_status=$?
        # mktemp normally leaves no path on failure, but clean up a partial
        # directory from a failing implementation before returning the error.
        if [[ -n "$temp_workspace" ]]; then
          temp_workspace_parent="${temp_workspace%/*}"
          [[ -n "$temp_workspace_parent" ]] || temp_workspace_parent=/
          temp_basename="${temp_workspace##*/}"
          if [[ "$temp_workspace_parent" == "$temp_parent" && \
              "$temp_basename" == xnulinux-trustcache-scan.* && \
              -d "$temp_workspace" && ! -L "$temp_workspace" ]]; then
            if ! rm -rf -- "$temp_workspace"; then
              printf 'mktemp failed and partial scan workspace cleanup failed: %s\n' "$temp_workspace" >&2
            fi
          fi
        fi
        return "$temp_status"
      fi
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

  if [[ "$scan_status" == complete ]] && \
      ! mv "$sorted_candidates_file" "$XNU_TRUSTCACHE_SCAN_CANDIDATES_FILE" \
        2>> "$XNU_TRUSTCACHE_SCAN_ERRORS_FILE"; then
    scan_status='failed'
  fi

  if [[ "$scan_status" != complete ]]; then
    xnu_trustcache_scan_workspace_report_failure \
      "$scan_mode" "$XNU_TRUSTCACHE_SCAN_CANDIDATES_FILE" "$XNU_TRUSTCACHE_SCAN_ERRORS_FILE" "$diagnostic_target"
    xnu_trustcache_scan_workspace_cleanup "$scan_mode" "$XNU_TRUSTCACHE_SCAN_ROOT"
    return 1
  fi
}

xnu_trustcache_scan_workspace_self_test() {
  local scratch_root build_root='' temp_root sdk_root fake_bin scan_root diagnostics_file scan_status
  local find_marker partial_workspace real_mktemp real_mv
  local PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}"
  local CHECK_ONLY=1
  real_mktemp="$(command -v mktemp)"
  real_mv="$(command -v mv)"
  scratch_root="$(mktemp -d "${TMPDIR:-/tmp}/xnulinux-readonly-check-test.XXXXXX")" \
    || { printf 'could not create self-test scratch directory\n' >&2; return 1; }
  trap '[[ -z "$build_root" ]] || chmod u+w "$build_root" 2>/dev/null || true; rm -rf -- "$scratch_root"' RETURN

  build_root="$scratch_root/build"
  temp_root="$scratch_root/tmp"
  sdk_root="$scratch_root/sdk"
  fake_bin="$scratch_root/bin"
  diagnostics_file="$scratch_root/check-diagnostics.log"
  find_marker="$temp_root/find-was-called"
  mkdir -p "$build_root" "$temp_root" "$sdk_root" "$fake_bin"
  chmod 555 "$build_root"
  chmod 700 "$temp_root"

  TMPDIR="$temp_root"
  export TMPDIR
  cat > "$fake_bin/mktemp" <<'MKTEMP'
#!/bin/sh
if [ "${XNU_TRUSTCACHE_TEST_MKTEMP_MODE:-real}" = partial-failure ]; then
  partial="${TMPDIR:-/tmp}/xnulinux-trustcache-scan.partial-fixture"
  mkdir "$partial" || exit 93
  printf '%s\n' "$partial"
  printf '%s\n' 'simulated mktemp partial-creation failure' >&2
  exit 1
fi
exec "$XNU_TRUSTCACHE_TEST_REAL_MKTEMP" "$@"
MKTEMP
  cat > "$fake_bin/find" <<'FIND'
#!/bin/sh
: > "$XNU_TRUSTCACHE_TEST_FIND_MARKER"
case "${XNU_TRUSTCACHE_TEST_FIND_MODE:-failure}" in
  failure)
    printf '%s\n' '/fake/sdk/System/Library/Frameworks/Kernel.framework/PrivateHeaders/TrustCache/API.h'
    printf '%s\n' 'simulated find permission failure' >&2
    exit 1
    ;;
  success)
    printf '%s\n' '/fake/sdk/System/Library/Frameworks/Kernel.framework/PrivateHeaders/TrustCache/API.h'
    exit 0
    ;;
  *)
    printf 'unknown test find mode: %s\n' "$XNU_TRUSTCACHE_TEST_FIND_MODE" >&2
    exit 94
    ;;
esac
FIND
  cat > "$fake_bin/mv" <<'MV'
#!/bin/sh
if [ "${XNU_TRUSTCACHE_TEST_MV_MODE:-real}" = failure ]; then
  printf '%s\n' 'simulated mv finalization failure' >&2
  exit 1
fi
exec "$XNU_TRUSTCACHE_TEST_REAL_MV" "$@"
MV
  export XNU_TRUSTCACHE_TEST_REAL_MKTEMP="$real_mktemp"
  export XNU_TRUSTCACHE_TEST_REAL_MV="$real_mv"
  export XNU_TRUSTCACHE_TEST_FIND_MARKER="$find_marker"
  export XNU_TRUSTCACHE_TEST_MKTEMP_MODE=real
  export XNU_TRUSTCACHE_TEST_FIND_MODE=failure
  export XNU_TRUSTCACHE_TEST_MV_MODE=real
  chmod +x "$fake_bin/mktemp" "$fake_bin/find" "$fake_bin/mv"
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

  # A mktemp implementation may create a directory and still return failure.
  # The scanner must clean that partial workspace, fail before invoking find,
  # and avoid claiming that a scan or diagnostic cleanup occurred.
  partial_workspace="$temp_root/xnulinux-trustcache-scan.partial-fixture"
  rm -f "$find_marker" "$diagnostics_file"
  XNU_TRUSTCACHE_SCAN_ROOT=''
  XNU_TRUSTCACHE_SCAN_CANDIDATES_FILE=''
  XNU_TRUSTCACHE_SCAN_ERRORS_FILE=''
  XNU_TRUSTCACHE_TEST_MKTEMP_MODE=partial-failure
  if xnu_trustcache_scan_sdk_headers "$sdk_root" "$build_root" /dev/stderr 2> "$diagnostics_file"; then
    printf 'self-test expected partial mktemp failure to abort the TrustCache scan\n' >&2
    return 1
  else
    scan_status=$?
  fi
  scan_root="$XNU_TRUSTCACHE_SCAN_ROOT"
  [[ "$scan_status" -eq 2 ]] \
    || { printf 'partial mktemp failure returned unexpected status %s\n' "$scan_status" >&2; return 1; }
  [[ -z "$scan_root" && ! -e "$partial_workspace" ]] \
    || { printf 'partial mktemp workspace was not safely cleaned: %s\n' "$partial_workspace" >&2; return 1; }
  [[ -z "$XNU_TRUSTCACHE_SCAN_CANDIDATES_FILE" && -z "$XNU_TRUSTCACHE_SCAN_ERRORS_FILE" ]] \
    || { printf 'scanner initialized candidate/error files after mktemp failed\n' >&2; return 1; }
  grep -Fq 'simulated mktemp partial-creation failure' "$diagnostics_file" \
    || { printf 'mktemp failure diagnostic omitted the underlying error\n' >&2; return 1; }
  grep -Fq 'cannot create the TrustCache SDK scan workspace in check mode' "$diagnostics_file" \
    || { printf 'mktemp failure did not report that scan workspace creation failed\n' >&2; return 1; }
  ! grep -Fq 'trustcache_header_scan_status=' "$diagnostics_file" \
    || { printf 'mktemp failure falsely reported a scan result\n' >&2; return 1; }
  ! grep -Fq 'simulated find permission failure' "$diagnostics_file" \
    || { printf 'find ran despite the mktemp failure\n' >&2; return 1; }
  [[ ! -e "$find_marker" ]] || { printf 'find was called after mktemp failed\n' >&2; return 1; }
  [[ -z "$(ls -A "$build_root")" ]] \
    || { printf 'mktemp failure modified read-only BUILD_ROOT\n' >&2; return 1; }

  # If the final rename fails, report both the partial candidate list and the
  # mv error before removing the check workspace; don't return stale/unsorted
  # data as a successful scan result.
  rm -f "$diagnostics_file" "$find_marker"
  XNU_TRUSTCACHE_SCAN_ROOT=''
  XNU_TRUSTCACHE_SCAN_CANDIDATES_FILE=''
  XNU_TRUSTCACHE_SCAN_ERRORS_FILE=''
  XNU_TRUSTCACHE_TEST_MKTEMP_MODE=real
  XNU_TRUSTCACHE_TEST_FIND_MODE=success
  XNU_TRUSTCACHE_TEST_MV_MODE=failure
  if xnu_trustcache_scan_sdk_headers "$sdk_root" "$build_root" /dev/stderr 2> "$diagnostics_file"; then
    printf 'self-test expected the simulated mv finalization failure to abort the scan\n' >&2
    return 1
  else
    scan_status=$?
  fi
  scan_root="$XNU_TRUSTCACHE_SCAN_ROOT"
  [[ "$scan_status" -eq 1 ]] \
    || { printf 'mv finalization failure returned unexpected status %s\n' "$scan_status" >&2; return 1; }
  [[ "$XNU_TRUSTCACHE_SCAN_MODE" == check && "$scan_root" == "$temp_root/"* ]] \
    || { printf 'mv failure did not use the check-mode TMPDIR workspace: %s\n' "$scan_root" >&2; return 1; }
  [[ -e "$find_marker" ]] || { printf 'find did not run before the simulated mv failure\n' >&2; return 1; }
  grep -Fq '/fake/sdk/System/Library/Frameworks/Kernel.framework/PrivateHeaders/TrustCache/API.h' "$diagnostics_file" \
    || { printf 'mv failure diagnostic omitted the partial candidate list\n' >&2; return 1; }
  grep -Fq 'simulated mv finalization failure' "$diagnostics_file" \
    || { printf 'mv failure diagnostic omitted the underlying error\n' >&2; return 1; }
  grep -Fq 'trustcache_header_scan_status=failed' "$diagnostics_file" \
    || { printf 'mv failure diagnostic did not mark the scan as failed\n' >&2; return 1; }
  ! grep -Fq "$scan_root" "$diagnostics_file" \
    || { printf 'mv failure diagnostic points at temporary files removed during cleanup\n' >&2; return 1; }
  [[ ! -e "$scan_root" ]] || { printf 'mv failure workspace was not cleaned: %s\n' "$scan_root" >&2; return 1; }
  [[ -z "$(ls -A "$build_root")" ]] \
    || { printf 'mv failure modified read-only BUILD_ROOT\n' >&2; return 1; }

  XNU_TRUSTCACHE_TEST_MKTEMP_MODE=real
  XNU_TRUSTCACHE_TEST_FIND_MODE=failure
  XNU_TRUSTCACHE_TEST_MV_MODE=real
  CHECK_ONLY=0
  [[ "$(xnu_trustcache_scan_workspace "$build_root" build)" == "$build_root" ]] \
    || { printf 'build-mode diagnostics no longer use BUILD_ROOT\n' >&2; return 1; }

  printf 'read-only --check scan self-test passed: find, mktemp, and mv failures fail closed; diagnostics are captured before cleanup; BUILD_ROOT is unchanged\n'
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
