#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTALLER="$ROOT_DIR/scripts/vm-install-fki-service.sh"
FIXTURE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/xnulinux-fki-bls.XXXXXX")"
trap 'rm -rf -- "$FIXTURE_DIR"' EXIT

mkdir -p -- "$FIXTURE_DIR/bin" "$FIXTURE_DIR/sbin"
cat >"$FIXTURE_DIR/iomem" <<'IOMEM'
00000000-7ffffffff : System RAM
IOMEM

cat >"$FIXTURE_DIR/bin/sudo" <<'STUB'
#!/usr/bin/env bash
if [[ "${1:-}" == sh && "${2:-}" == -c ]]; then
  shift 2
  exec env "PATH=$SUDO_SECURE_PATH:/usr/bin:/bin" /bin/sh -c "$@"
fi
exec "$@"
STUB

cat >"$FIXTURE_DIR/bin/python3" <<'STUB'
#!/usr/bin/env bash
if [[ "${1:-}" == */scripts/detect-mem.py ]]; then
  exec /usr/bin/python3 "$@" --iomem "$FIXTURE_IOMEM"
fi
exec /usr/bin/python3 "$@"
STUB

cat >"$FIXTURE_DIR/bin/systemctl" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$SYSTEMCTL_LOG"
STUB

cat >"$FIXTURE_DIR/sbin/grubby" <<'STUB'
#!/usr/bin/env bash
if [[ "${1:-}" == --info=ALL ]]; then
  if [[ -n "${FIXTURE_KERNELOPTS_FILE:-}" && -f "$FIXTURE_KERNELOPTS_FILE" ]]; then
    printf 'index=0\nkernel="/boot/vmlinuz"\nargs="%s"\n' "$(cat "$FIXTURE_KERNELOPTS_FILE")"
    exit 0
  fi
  exit 1
fi
[[ "${1:-}" == --update-kernel=ALL && "${2:-}" == --args=* ]] || exit 2
args="${2#--args=}"
printf '%s\n' "$*" >>"$GRUBBY_LOG"
if [[ -n "${FIXTURE_KERNELOPTS_FILE:-}" && -f "$FIXTURE_KERNELOPTS_FILE" ]]; then
  printf '%s %s\n' "$(cat "$FIXTURE_KERNELOPTS_FILE")" "$args" >"$FIXTURE_KERNELOPTS_FILE"
  exit 0
fi
for entry in "$FIXTURE_BLS_DIR"/*.conf; do
  [[ -f "$entry" ]] || continue
  temporary="$entry.tmp"
  awk -v extra="$args" '
    $1 == "options" { print $0, extra; found = 1; next }
    { print }
    END { if (!found) exit 1 }
  ' "$entry" >"$temporary"
  mv -- "$temporary" "$entry"
done
STUB
chmod +x -- "$FIXTURE_DIR/bin/sudo" "$FIXTURE_DIR/bin/python3" "$FIXTURE_DIR/bin/systemctl" "$FIXTURE_DIR/sbin/grubby"

setup_case() {
  local name="$1" initial_options="$2"
  CASE_DIR="$FIXTURE_DIR/$name"
  CASE_BLS="$CASE_DIR/boot/loader/entries"
  CASE_DEFAULT="$CASE_DIR/etc/default/grub"
  CASE_DROPIN_DIR="$CASE_DIR/etc/default/grub.d"
  CASE_SYSTEMD_DIR="$CASE_DIR/run/systemd/system"
  CASE_UNIT="$CASE_DIR/etc/systemd/system/xnulinux-fki.service"
  CASE_LOG="$CASE_DIR/grubby.log"
  CASE_SYSCTL_LOG="$CASE_DIR/systemctl.log"
  CASE_KERNELOPTS=""
  mkdir -p -- "$CASE_BLS" "$(dirname -- "$CASE_DEFAULT")" "$(dirname -- "$CASE_UNIT")" "$CASE_SYSTEMD_DIR"
  printf 'GRUB_CMDLINE_LINUX="quiet"\n' >"$CASE_DEFAULT"
  printf 'title fixture\nlinux /vmlinuz\noptions %s\n' "$initial_options" >"$CASE_BLS/fixture.conf"
}

run_installer() {
  PATH="$FIXTURE_DIR/bin:/usr/bin:/bin" \
    SUDO_SECURE_PATH="$FIXTURE_DIR/sbin" \
    GRUBBY_LOG="$CASE_LOG" SYSTEMCTL_LOG="$CASE_SYSCTL_LOG" \
    FIXTURE_BLS_DIR="$CASE_BLS" FIXTURE_KERNELOPTS_FILE="$CASE_KERNELOPTS" \
    FIXTURE_IOMEM="$FIXTURE_DIR/iomem" \
    LINUX_MEM=7G CARVE_BASE=0x200000000 CARVE_MB=8156 RAM_MB=7900 \
    XNULINUX_GRUB_DEFAULT="$CASE_DEFAULT" \
    XNULINUX_GRUB_DEFAULT_DIR="$CASE_DROPIN_DIR" \
    XNULINUX_GRUB_CMDLINE_FILE="$CASE_DROPIN_DIR/99-xnulinux.cfg" \
    XNULINUX_BLS_ENTRIES="$CASE_BLS" \
    XNULINUX_SYSTEMD_RUNTIME_DIR="$CASE_SYSTEMD_DIR" \
    XNULINUX_SYSTEMD_UNIT_FILE="$CASE_UNIT" \
    "$INSTALLER"
}

# 1. Valid RHEL 9-style BLS entry with grubby only in sudo's secure PATH -> installs service
setup_case good 'root=UUID=fixture ro quiet'
run_installer >"$CASE_DIR/out" 2>&1 || {
  cat "$CASE_DIR/out" >&2
  echo 'installer failed for BLS entries updated by grubby' >&2
  exit 1
}
[[ -s "$CASE_LOG" ]] || { echo 'installer did not invoke grubby' >&2; exit 1; }
[[ -f "$CASE_UNIT" ]] || { echo 'installer did not create systemd unit on valid BLS' >&2; exit 1; }

# 2. Valid RHEL 8-style BLS entry using $kernelopts expanded by grubby --info=ALL -> installs service
setup_case rhel8-kernelopts '$kernelopts'
CASE_KERNELOPTS="$CASE_DIR/kernelopts"
printf 'root=UUID=fixture ro quiet\n' >"$CASE_KERNELOPTS"
run_installer >"$CASE_DIR/out" 2>&1 || {
  cat "$CASE_DIR/out" >&2
  echo 'installer failed for RHEL 8 $kernelopts BLS entry' >&2
  exit 1
}
[[ -f "$CASE_UNIT" ]] || { echo 'installer did not create systemd unit on RHEL 8 $kernelopts' >&2; exit 1; }

# 3. Conflicting mem= argument -> fails BEFORE creating systemd unit
setup_case duplicate 'root=UUID=fixture ro mem=4G'
if run_installer >"$CASE_DIR/out" 2>&1; then
  echo 'installer accepted conflicting mem= arguments in a BLS entry' >&2
  exit 1
fi
grep -Fq 'does not contain exactly one effective mem=7G argument' "$CASE_DIR/out"
[[ ! -e "$CASE_UNIT" ]] || { echo 'installer enabled the FKI service despite a BLS validation failure' >&2; exit 1; }

# 4. Missing grubby -> fails BEFORE creating systemd unit
setup_case missing-grubby 'root=UUID=fixture ro quiet'
mv -- "$FIXTURE_DIR/sbin/grubby" "$FIXTURE_DIR/sbin/grubby.disabled"
if run_installer >"$CASE_DIR/out" 2>&1; then
  echo 'installer accepted BLS entries without grubby' >&2
  exit 1
fi
mv -- "$FIXTURE_DIR/sbin/grubby.disabled" "$FIXTURE_DIR/sbin/grubby"
grep -Fq 'grubby is required to apply and verify' "$CASE_DIR/out"
[[ ! -e "$CASE_UNIT" ]] || { echo 'installer enabled the FKI service without a BLS updater' >&2; exit 1; }

echo 'PASS: BLS entries (RHEL 8 $kernelopts and RHEL 9) require grubby via sudo PATH and block service setup on failure'
