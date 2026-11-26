#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# On an arm64 Linux host (once): reserve the hosted kernel's DRAM outside Linux's memory map and load fki.ko early in
# every boot. By default, scripts/detect-mem.py inspects /proc/iomem to size LINUX_MEM, CARVE_BASE, and CARVE_MB for
# the machine's actual RAM layout (override any of them in the environment, e.g. LINUX_MEM=15G CARVE_BASE=0x400000000).
# Usage: vm-install-fki-service.sh [--dry-run]
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
resolve_command() {
  local name="$1" resolved
  resolved=$(command -v "$name" 2>/dev/null || true)
  if [ -z "$resolved" ]; then
    # Installers may be launched with a user PATH that omits sbin directories.
    resolved=$(sudo sh -c 'command -v "$1"' sh "$name" 2>/dev/null || true)
  fi
  printf '%s' "$resolved"
}
DRY_RUN=0
[ "${1:-}" = --dry-run ] || [ "${1:-}" = --print ] && DRY_RUN=1

if [ -z "${LINUX_MEM:-}" ] || [ -z "${CARVE_BASE:-}" ] || [ -z "${CARVE_MB:-}" ]; then
  if [ -r /proc/iomem ] && grep -qE '^[0-9a-fA-F]*[1-9a-fA-F][0-9a-fA-F]*-.*: System RAM$' /proc/iomem 2>/dev/null; then
    eval "$(python3 "$P/scripts/detect-mem.py")"
  else
    eval "$(sudo python3 "$P/scripts/detect-mem.py")"
  fi
fi

CMDLINE_ARGS="mem=$LINUX_MEM nokaslr log_buf_len=128M"
if [ "$DRY_RUN" = 1 ]; then
  echo "LINUX_MEM=$LINUX_MEM CARVE_BASE=$CARVE_BASE CARVE_MB=$CARVE_MB RAM_MB=${RAM_MB:-}"
  echo "kernel cmdline: $CMDLINE_ARGS"
  echo "fki.ko args: carve_base=$CARVE_BASE carve_mb=$CARVE_MB"
  exit 0
fi

# nokaslr: arm64's EFI stub randomises the Linux kernel's physical load address across all EFI memory before mem=
# takes effect, which otherwise places the kernel image inside the carve-out on some boots (fki then refuses to map it).
GRUB_DEFAULT="${XNULINUX_GRUB_DEFAULT:-/etc/default/grub}"
GRUB_DEFAULT_DIR="${XNULINUX_GRUB_DEFAULT_DIR:-/etc/default/grub.d}"
G="${XNULINUX_GRUB_CMDLINE_FILE:-$GRUB_DEFAULT_DIR/99-xnulinux.cfg}"
BLS_ENTRIES="${XNULINUX_BLS_ENTRIES:-/boot/loader/entries}"
if [ -f "$GRUB_DEFAULT" ]; then
  sudo mkdir -p "$GRUB_DEFAULT_DIR"
  echo "GRUB_CMDLINE_LINUX=\"\$GRUB_CMDLINE_LINUX $CMDLINE_ARGS\"" | sudo tee "$G" >/dev/null

  BLS_ENABLED=0
  if [ -d "$BLS_ENTRIES" ]; then
    BLS_CFG_VAL=$(awk -F= '/^[[:space:]]*GRUB_ENABLE_BLSCFG=/{v=$2; gsub(/["'\''[:space:]]/, "", v); last=v} END{print last}' "$GRUB_DEFAULT" 2>/dev/null || true)
    case "$BLS_CFG_VAL" in
      false|no|0) BLS_ENABLED=0 ;;
      *) BLS_ENABLED=1 ;;
    esac
  fi

  if [ "$BLS_ENABLED" = 1 ]; then
    GRUBBY_BIN=$(resolve_command grubby)
    if [ -z "$GRUBBY_BIN" ]; then
      echo "error: BLS boot entries found, but grubby is required to apply and verify '$CMDLINE_ARGS'" >&2
      exit 1
    fi
    sudo "$GRUBBY_BIN" --update-kernel=ALL --args="$CMDLINE_ARGS"
    if GRUBBY_INFO=$(sudo "$GRUBBY_BIN" --info=ALL 2>/dev/null) && printf '%s\n' "$GRUBBY_INFO" | grep -q '^args='; then
      if ! printf '%s\n' "$GRUBBY_INFO" | awk -v expected="mem=$LINUX_MEM" '
        /^args=/ {
          val = substr($0, 6)
          gsub(/^"|"$/, "", val)
          entries++
          n = split(val, a, /[[:space:]]+/)
          count = 0
          for (i = 1; i <= n; i++) {
            if (a[i] ~ /^mem=/) {
              count++
              if (a[i] != expected) wrong = 1
            }
          }
          if (count != 1) wrong = 1
        }
        END { exit (wrong || entries == 0) }
      '; then
        echo "error: BLS entries do not contain exactly one effective mem=$LINUX_MEM argument (checked via grubby --info=ALL)" >&2
        exit 1
      fi
    else
      if ! sudo sh -c '
        dir=$1; expected=$2; count=0
        for entry in "$dir"/*.conf; do
          [ -f "$entry" ] || continue
          count=$((count + 1))
          awk -v expected="$expected" "
            \$1 == \"options\" {
              option_lines++
              c = 0
              for (i = 2; i <= NF; i++) {
                if (\$i ~ /^mem=/) {
                  c++
                  if (\$i != expected) wrong = 1
                }
              }
              if (c != 1) wrong = 1
            }
            END { exit (wrong || option_lines != 1) }
          " "$entry" || { echo "error: BLS entry does not contain exactly one effective $expected argument: $entry" >&2; exit 1; }
        done
        if [ "$count" -eq 0 ]; then
          echo "error: BLS boot entries directory contains no kernel entries: $dir" >&2
          exit 1
        fi
      ' sh "$BLS_ENTRIES" "mem=$LINUX_MEM"; then
        exit 1
      fi
    fi
  elif UPDATE_GRUB_BIN=$(resolve_command update-grub) && [ -n "$UPDATE_GRUB_BIN" ]; then
    sudo "$UPDATE_GRUB_BIN" >/dev/null 2>&1
  elif GRUB2_MKCONFIG_BIN=$(resolve_command grub2-mkconfig) && [ -n "$GRUB2_MKCONFIG_BIN" ]; then
    GRUB_CFG=
    for cfg in /boot/grub2/grub.cfg /boot/efi/EFI/*/grub.cfg; do
      if [ -e "$cfg" ]; then GRUB_CFG=$cfg; break; fi
    done
    if [ -n "$GRUB_CFG" ]; then
      sudo "$GRUB2_MKCONFIG_BIN" -o "$GRUB_CFG" >/dev/null 2>&1
      if ! sudo grep -qw "mem=$LINUX_MEM" "$GRUB_CFG" 2>/dev/null; then
        echo "error: '$GRUB_CFG' does not contain 'mem=$LINUX_MEM' after grub2-mkconfig (this distro may ignore $G; add '$CMDLINE_ARGS' to GRUB_CMDLINE_LINUX in $GRUB_DEFAULT and rerun)" >&2
        exit 1
      fi
    else
      echo "note: GRUB configuration file not found; add '$CMDLINE_ARGS' to your bootloader's kernel command line manually" >&2
    fi
  else
    echo "note: GRUB not found; add '$CMDLINE_ARGS' to your bootloader's kernel command line manually" >&2
  fi
else
  echo "note: GRUB not found; add '$CMDLINE_ARGS' to your bootloader's kernel command line manually" >&2
fi

SYSTEMD_RUNTIME_DIR="${XNULINUX_SYSTEMD_RUNTIME_DIR:-/run/systemd/system}"
SYSTEMD_UNIT_FILE="${XNULINUX_SYSTEMD_UNIT_FILE:-/etc/systemd/system/xnulinux-fki.service}"
if command -v systemctl >/dev/null 2>&1 && [ -d "$SYSTEMD_RUNTIME_DIR" ]; then
  sudo tee "$SYSTEMD_UNIT_FILE" >/dev/null <<UNIT
[Unit]
Description=xnulinux: fki.ko with the hosted kernel's DRAM reserved
DefaultDependencies=no
After=local-fs.target
Before=sysinit.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/sbin/insmod $P/fki-ko/fki.ko carve_base=$CARVE_BASE carve_mb=$CARVE_MB

[Install]
WantedBy=sysinit.target
UNIT
  sudo systemctl daemon-reload
  sudo systemctl enable xnulinux-fki.service
else
  echo "note: systemd not active; load fki.ko at boot with: insmod $P/fki-ko/fki.ko carve_base=$CARVE_BASE carve_mb=$CARVE_MB" >&2
fi
sync
echo "after a reboot: Linux on $LINUX_MEM, fki.ko loaded with the $CARVE_MB MB carve-out at $CARVE_BASE (rebuild fki-ko before a reboot when it changes)"
