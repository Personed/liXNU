#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTALLER="$ROOT_DIR/scripts/vm-install-fki-service.sh"
FIXTURE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/xnulinux-fki-bls.XXXXXX")"
trap 'rm -rf -- "$FIXTURE_DIR"' EXIT

mkdir -p -- "$FIXTURE_DIR/bin" "$FIXTURE_DIR/sbin"
cat >"$FIXTURE_DIR/bin/sudo" <<'STUB'
#!/usr/bin/env bash
if [[ "${1:-}" == sh && "${2:-}" == -c ]]; then
  exec env "PATH=$SUDO_SECURE_PATH" /bin/sh -c "$3"
fi
exec "$@"
STUB

cat >"$FIXTURE_DIR/bin/grubby" <<'STUB'
#!/usr/bin/env bash
[[ "${1:-}" == --update-kernel=ALL && "${2:-}" == --args=* ]] || exit 2
args="${2#--args=}"
printf '%s\n' "$*" >>"$GRUBBY_LOG"
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
chmod +x -- "$FIXTURE_DIR/bin/sudo" "$FIXTURE_DIR/bin/grubby"

setup_case() {
  local name="$1" initial_options="$2"
  CASE_DIR="$FIXTURE_DIR/$name"
  CASE_BLS="$CASE_DIR/boot/loader/entries"
  CASE_DEFAULT="$CASE_DIR/etc/default/grub"
  CASE_DROPIN_DIR="$CASE_DIR/etc/default/grub.d"
  CASE_UNIT="$CASE_DIR/etc/systemd/system/xnulinux-fki.service"
  CASE_LOG="$CASE_DIR/grubby.log"
  mkdir -p -- "$CASE_BLS" "$(dirname -- "$CASE_DEFAULT")"
  printf 'GRUB_CMDLINE_LINUX="quiet"\n' >"$CASE_DEFAULT"
  printf 'title fixture\nlinux /vmlinuz\noptions %s\n' "$initial_options" >"$CASE_BLS/fixture.conf"
}

run_installer() {
  PATH="$FIXTURE_DIR/bin:/usr/bin:/bin" \
    SUDO_SECURE_PATH="$FIXTURE_DIR/sbin" \
    GRUBBY_LOG="$CASE_LOG" FIXTURE_BLS_DIR="$CASE_BLS" \
    LINUX_MEM=7G CARVE_BASE=0x200000000 CARVE_MB=8156 RAM_MB=7900 \
    XNULINUX_GRUB_DEFAULT="$CASE_DEFAULT" \
    XNULINUX_GRUB_DEFAULT_DIR="$CASE_DROPIN_DIR" \
    XNULINUX_GRUB_CMDLINE_FILE="$CASE_DROPIN_DIR/99-xnulinux.cfg" \
    XNULINUX_BLS_ENTRIES="$CASE_BLS" \
    XNULINUX_SYSTEMD_RUNTIME_DIR="$CASE_DIR/no-systemd" \
    XNULINUX_SYSTEMD_UNIT_FILE="$CASE_UNIT" \
    "$INSTALLER"
}

setup_case good 'root=UUID=fixture ro quiet'
run_installer >"$CASE_DIR/out" 2>&1 || {
  cat "$CASE_DIR/out" >&2
  echo 'installer failed for BLS entries updated by grubby' >&2
  exit 1
}
awk '
  $1 == "options" {
    count = 0
    for (i = 2; i <= NF; i++) if ($i ~ /^mem=/) { count++; if ($i != "mem=7G") exit 1 }
    if (count != 1) exit 1
    found = 1
  }
  END { if (!found) exit 1 }
' "$CASE_BLS/fixture.conf"
[[ -s "$CASE_LOG" ]] || { echo 'installer did not invoke grubby' >&2; exit 1; }
[[ ! -e "$CASE_UNIT" ]] || { echo 'fixture unexpectedly enabled a systemd service' >&2; exit 1; }

setup_case duplicate 'root=UUID=fixture ro mem=4G'
if run_installer >"$CASE_DIR/out" 2>&1; then
  echo 'installer accepted conflicting mem= arguments in a BLS entry' >&2
  exit 1
fi
grep -Fq 'does not contain exactly one effective mem=7G argument' "$CASE_DIR/out"
[[ ! -e "$CASE_UNIT" ]] || { echo 'installer enabled the FKI service despite a BLS validation failure' >&2; exit 1; }

setup_case missing-grubby 'root=UUID=fixture ro quiet'
mv -- "$FIXTURE_DIR/bin/grubby" "$FIXTURE_DIR/bin/grubby.disabled"
if run_installer >"$CASE_DIR/out" 2>&1; then
  echo 'installer accepted BLS entries without grubby' >&2
  exit 1
fi
mv -- "$FIXTURE_DIR/bin/grubby.disabled" "$FIXTURE_DIR/bin/grubby"
grep -Fq 'grubby is required to apply and verify' "$CASE_DIR/out"
[[ ! -e "$CASE_UNIT" ]] || { echo 'installer enabled the FKI service without a BLS updater' >&2; exit 1; }

echo 'PASS: BLS entries require grubby and each entry must have exactly one effective mem= argument before service setup'
