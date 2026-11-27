#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
mkdir -p "$fixture/bin"

cat > "$fixture/bin/uname" <<'UNAME'
#!/bin/sh
printf 'Linux aarch64\n'
UNAME
cat > "$fixture/bin/sudo" <<'SUDO'
#!/bin/sh
set -eu
printf '%s\n' "$*" >> "$TEST_CASE/sudo.log"
case "$1" in
  tee) cat > "$TEST_CASE/${2##*/}" ;;
  systemctl) ;;
  *) exit 1 ;;
esac
SUDO
chmod +x "$fixture/bin/uname" "$fixture/bin/sudo"

cat > "$fixture/config-checks.sh" <<'CHECKS'
[() {
  case "$*" in
    "! -f /etc/xnulinux.conf ]") [[ "$TEST_SERVICE_CONFIG" == absent ]] ;;
    "! -f /var/tmp/mac.conf ]") [[ "$TEST_DEFAULT_DISK" == absent ]] ;;
    *) builtin [ "$@" ;;
  esac
}
CHECKS

run_installer() {
  local name=$1 config=$2 default_disk=$3
  local case_dir="$fixture/$name"
  mkdir -p "$case_dir"
  PATH="$fixture/bin:$PATH" BASH_ENV="$fixture/config-checks.sh" TEST_CASE="$case_dir" \
    TEST_SERVICE_CONFIG="$config" TEST_DEFAULT_DISK="$default_disk" \
    "$REPO/scripts/install-service.sh" > "$case_dir/output.log" 2>&1
}

run_installer custom present absent
test ! -e "$fixture/custom/xnulinux.conf"
test -s "$fixture/custom/xnulinux.service"
grep -Fxq 'systemctl enable xnulinux.service' "$fixture/custom/sudo.log"

run_installer default absent present
test -s "$fixture/default/xnulinux.conf"
test -s "$fixture/default/xnulinux.service"
grep -Fxq 'systemctl enable xnulinux.service' "$fixture/default/sudo.log"

if run_installer missing absent absent; then
  echo 'installer accepted a host without a service or default disk configuration' >&2
  exit 1
fi
grep -Fq 'No default disk configuration at /var/tmp/mac.conf' "$fixture/missing/output.log"
test ! -e "$fixture/missing/sudo.log"
