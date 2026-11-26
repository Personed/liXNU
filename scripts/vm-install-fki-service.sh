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
G=/etc/default/grub.d/99-xnulinux.cfg
GRUB_DEFAULT=/etc/default/grub
BLS_ENTRIES=/boot/loader/entries
if [ -f "$GRUB_DEFAULT" ]; then
  sudo mkdir -p /etc/default/grub.d
  echo "GRUB_CMDLINE_LINUX=\"\$GRUB_CMDLINE_LINUX $CMDLINE_ARGS\"" | sudo tee "$G" >/dev/null

  if [ -d "$BLS_ENTRIES" ]; then
    GRUBBY_BIN=$(resolve_command grubby)
    if [ -n "$GRUBBY_BIN" ]; then
      sudo "$GRUBBY_BIN" --update-kernel=ALL --args="$CMDLINE_ARGS"
    else
      GRUB2_MKCONFIG_BIN=$(resolve_command grub2-mkconfig)
      if [ -z "$GRUB2_MKCONFIG_BIN" ]; then
        echo "error: BLS boot entries found, but neither grubby nor grub2-mkconfig is available to apply '$CMDLINE_ARGS'" >&2
        exit 1
      fi
      GRUB2_MKCONFIG_HELP="$("$GRUB2_MKCONFIG_BIN" --help 2>&1 || true)"
      if ! grep -q -- '--update-bls-cmdline' <<<"$GRUB2_MKCONFIG_HELP"; then
        echo "error: BLS boot entries found, but neither grubby nor grub2-mkconfig --update-bls-cmdline is available to apply '$CMDLINE_ARGS'" >&2
        exit 1
      fi
      GRUB_CFG=
      for cfg in /boot/grub2/grub.cfg /boot/efi/EFI/*/grub.cfg; do
        if [ -e "$cfg" ]; then GRUB_CFG=$cfg; break; fi
      done
      if [ -z "$GRUB_CFG" ]; then
        echo "error: BLS boot entries found, but no GRUB configuration file was found" >&2
        exit 1
      fi
      sudo "$GRUB2_MKCONFIG_BIN" --update-bls-cmdline -o "$GRUB_CFG" >/dev/null 2>&1
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
    else
      echo "note: GRUB configuration file not found; add '$CMDLINE_ARGS' to your bootloader's kernel command line manually" >&2
    fi
  else
    echo "note: GRUB not found; add '$CMDLINE_ARGS' to your bootloader's kernel command line manually" >&2
  fi
else
  echo "note: GRUB not found; add '$CMDLINE_ARGS' to your bootloader's kernel command line manually" >&2
fi

if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
  sudo tee /etc/systemd/system/xnulinux-fki.service >/dev/null <<UNIT
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
