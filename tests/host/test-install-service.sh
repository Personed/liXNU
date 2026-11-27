#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
mkdir -p "$fixture/bin"

cat > "$fixture/bin/uname" <<'UNAME'
#!/bin/sh
case "$1" in
  -sm) printf 'Linux aarch64\n' ;;
  -s) printf 'Linux\n' ;;
  -m) printf 'aarch64\n' ;;
  -r) printf 'xnulinux-fixture\n' ;;
  *) exit 1 ;;
esac
UNAME
cat > "$fixture/bin/getconf" <<'GETCONF'
#!/bin/sh
printf '4096\n'
GETCONF
cat > "$fixture/bin/make" <<'MAKE'
#!/bin/sh
: > "$TEST_CASE/make-called"
test "${TEST_LOAD_XNU:-}" = yes
MAKE
cp "$fixture/bin/make" "$fixture/bin/modinfo"
cat > "$fixture/bin/sudo" <<'SUDO'
#!/bin/sh
set -eu
printf '%s\n' "$*" >> "$TEST_CASE/sudo.log"
case "$1" in
  tee) cat > "$TEST_CASE/${2##*/}" ;;
  systemctl) ;;
  dmesg|insmod|rmmod) test "${TEST_LOAD_XNU:-}" = yes ;;
  setsid)
    test "${TEST_LOAD_XNU:-}" = yes
    test "$2 $3 $4" = "nohup insmod xnu-ko/xnu.ko"
    printf '%s\n' "$@" > "$TEST_CASE/xnu-args"
    ;;
  *) exit 1 ;;
esac
SUDO
chmod +x "$fixture/bin/"*

cat > "$fixture/config-checks.sh" <<'CHECKS'
[() {
  case "$*" in
    "! -f /etc/xnulinux.conf ]") [[ ! -f "$TEST_CASE/xnulinux.conf" ]] ;;
    "-f /etc/xnulinux.conf ]") [[ -f "$TEST_CASE/xnulinux.conf" ]] ;;
    "! -f /var/tmp/mac.conf ]") [[ "$TEST_DEFAULT_DISK" == absent ]] ;;
    "! -f /lib/modules/xnulinux-fixture/build/Makefile ]") return 1 ;;
    "-d /sys/module/xnu ]") return 1 ;;
    *) builtin [ "$@" ;;
  esac
}
.() {
  if [[ "$1" == /etc/xnulinux.conf ]]; then
    builtin . "$TEST_CASE/xnulinux.conf"
  else
    builtin . "$@"
  fi
}
nproc() { printf '1\n'; }
sync() { :; }
sleep() { wait; }
CHECKS

run_installer() {
  local name=$1 default_disk=$2
  local case_dir="$fixture/$name"
  mkdir -p "$case_dir"
  PATH="$fixture/bin:$PATH" BASH_ENV="$fixture/config-checks.sh" TEST_CASE="$case_dir" \
    TEST_DEFAULT_DISK="$default_disk" \
    "$REPO/scripts/install-service.sh" > "$case_dir/output.log" 2>&1
}

custom="$fixture/custom"
mkdir -p "$custom"
printf 'MAC_CONF=%q\nDISK=%q\n' "$custom/pd.conf" "$custom/pd.img" > "$custom/xnulinux.conf"
cp "$custom/xnulinux.conf" "$custom/config-before"
run_installer custom absent
cmp "$custom/config-before" "$custom/xnulinux.conf"
grep -Fxq "ExecStart=$REPO/scripts/xnulinux-boot.sh --wait" "$custom/xnulinux.service"
grep -Fxq 'systemctl enable xnulinux.service' "$custom/sudo.log"
cp "$custom/sudo.log" "$custom/sudo-before"

for field in ROOT_UUID BOOT_UUID; do
  cat > "$custom/pd.conf" <<'DISK_CONF'
test "$DISK" = "$TEST_CASE/pd.img" || exit 2
: > "$TEST_CASE/custom-conf-read"
DISK_CONF
  if [ "$field" = ROOT_UUID ]; then
    printf 'BOOT_UUID=fixture-boot\n' >> "$custom/pd.conf"
  else
    printf 'ROOT_UUID=fixture-root\n' >> "$custom/pd.conf"
  fi
  if PATH="$fixture/bin:$PATH" BASH_ENV="$fixture/config-checks.sh" TEST_CASE="$custom" \
      MAC_CONF= DISK= ROOT_UUID= BOOT_UUID= \
      "$REPO/scripts/xnulinux-boot.sh" --wait > "$custom/boot.log" 2>&1; then
    echo "boot accepted a custom disk without $field" >&2
    exit 1
  fi
  if ! grep -Fq "no $field (check $custom/pd.conf or /etc/xnulinux.conf)" "$custom/boot.log"; then
    cat "$custom/boot.log" >&2
    exit 1
  fi
  test -f "$custom/custom-conf-read"
  rm -- "$custom/custom-conf-read"
  test ! -e "$custom/make-called"
  cmp "$custom/sudo-before" "$custom/sudo.log"
done

# Run the actual startup command in a private tree. Redirect its fixed log
# path and FKI parameter reads to the fixture, and replace networking and sudo.
mkdir -p "$fixture/runtime/scripts" "$custom/fki/parameters"
sed 's@/var/tmp/xnu-insmod.log@"$TEST_CASE/xnu-insmod.log"@g; s@/sys/module/fki@"$TEST_CASE/fki"@g' \
  "$REPO/scripts/xnulinux-boot.sh" > "$fixture/runtime/scripts/xnulinux-boot.sh"
printf 'exit 0\n' > "$fixture/runtime/scripts/vm-net-up.sh"
printf '0\n' > "$custom/fki/parameters/cpu_list"
printf '1024\n' > "$custom/fki/parameters/carve_mb"
printf 'ROOT_UUID=fixture-root\nBOOT_UUID=fixture-boot\n' > "$custom/pd.conf"
if ! PATH="$fixture/bin:$PATH" BASH_ENV="$fixture/config-checks.sh" TEST_CASE="$custom" TEST_LOAD_XNU=yes \
    MAC_CONF= DISK= ROOT_UUID= BOOT_UUID= GPU=off CPUS=1 SOC_NAME=fixture \
    CONSOLE_IN="$custom/console" INPUT_IN="$custom/input" FB_DUMP="$custom/fb" KEYSTORE="$custom/keystore" \
    bash "$fixture/runtime/scripts/xnulinux-boot.sh" > "$custom/start.log" 2>&1; then
  cat "$custom/start.log" >&2
  exit 1
fi
grep -Fxq "disk=$custom/pd.img" "$custom/xnu-args"
grep -Fxq 'root_uuid=fixture-root' "$custom/xnu-args"
grep -Fxq 'boot_uuid=fixture-boot' "$custom/xnu-args"

run_installer default present
test -s "$fixture/default/xnulinux.conf"
test -s "$fixture/default/xnulinux.service"
grep -Fxq 'systemctl enable xnulinux.service' "$fixture/default/sudo.log"

if run_installer missing absent; then
  echo 'installer accepted a host without a service or default disk configuration' >&2
  exit 1
fi
grep -Fq 'No default disk configuration at /var/tmp/mac.conf' "$fixture/missing/output.log"
test ! -e "$fixture/missing/sudo.log"
