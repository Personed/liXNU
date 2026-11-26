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

xnu_trustcache_scan_workspace_self_test() {
  local scratch_root build_root temp_root scan_root
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
  [[ "$scan_root" == "$temp_root/"* ]] \
    || { printf 'check workspace was not placed under TMPDIR: %s\n' "$scan_root" >&2; return 1; }
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
  [[ -z "$(find "$build_root" -mindepth 1 -maxdepth 1 -print -quit)" ]] \
    || { printf 'read-only BUILD_ROOT was modified\n' >&2; return 1; }

  xnu_trustcache_scan_workspace_cleanup check "$scan_root"
  [[ ! -e "$scan_root" ]] \
    || { printf 'check workspace was not cleaned: %s\n' "$scan_root" >&2; return 1; }
  [[ "$(xnu_trustcache_scan_workspace "$build_root" build)" == "$build_root" ]] \
    || { printf 'build-mode diagnostics no longer use BUILD_ROOT\n' >&2; return 1; }

  printf 'read-only --check workspace self-test passed: BUILD_ROOT unchanged; TMPDIR writable; temporary scan directory cleaned\n'
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
