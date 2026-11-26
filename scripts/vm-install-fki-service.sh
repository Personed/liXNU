#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# On an arm64 Linux host (once): reserve the hosted kernel's DRAM outside Linux's memory map and load fki.ko early in
# every boot. By default, scripts/detect-mem.py inspects /proc/iomem to size LINUX_MEM, CARVE_BASE, and CARVE_MB for
# the machine's actual RAM layout (override any of them in the environment, e.g. LINUX_MEM=15G CARVE_BASE=0x400000000).
# Usage: vm-install-fki-service.sh [--dry-run]
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
DRY_RUN=0
[ "${1:-}" = --dry-run ] || [ "${1:-}" = --print ] && DRY_RUN=1

REQUESTED_LINUX_MEM=${LINUX_MEM:-}
REQUESTED_CARVE_BASE=${CARVE_BASE:-}
REQUESTED_CARVE_MB=${CARVE_MB:-}
REQUESTED_RAM_MB=${RAM_MB:-}
if [ -z "$REQUESTED_LINUX_MEM" ] || [ -z "$REQUESTED_CARVE_BASE" ] || [ -z "$REQUESTED_CARVE_MB" ]; then
  if [ -r /proc/iomem ] && grep -qE '^[0-9a-fA-F]*[1-9a-fA-F][0-9a-fA-F]*-.*: System RAM$' /proc/iomem 2>/dev/null; then
    eval "$(python3 "$P/scripts/detect-mem.py")"
  else
    eval "$(sudo python3 "$P/scripts/detect-mem.py")"
  fi
  LINUX_MEM=${REQUESTED_LINUX_MEM:-$LINUX_MEM}
  CARVE_BASE=${REQUESTED_CARVE_BASE:-$CARVE_BASE}
  CARVE_MB=${REQUESTED_CARVE_MB:-$CARVE_MB}
fi

# Validate the final merged values before touching GRUB or installing the service.
# A partial override can otherwise leave mem= above the physical RAM below the
# carve-out, causing fki.ko to reject or overlap Linux memory after reboot.
VALIDATE_ARGS=(
  --validate
  --linux-mem "$LINUX_MEM"
  --carve-base "$CARVE_BASE"
  --carve-mb "$CARVE_MB"
)
# The detector's output is based on the automatic carve size. Validate the final
# geometry first, then derive the suggested XNU size from a caller override if
# present, or from the final carve size when only CARVE_MB was overridden.
VALIDATE_ARGS=(
  --validate
  --linux-mem "$LINUX_MEM"
  --carve-base "$CARVE_BASE"
  --carve-mb "$CARVE_MB"
)
if [ -r /proc/iomem ] && grep -qE '^[0-9a-fA-F]*[1-9a-fA-F][0-9a-fA-F]*-.*: System RAM$' /proc/iomem 2>/dev/null; then
  python3 "$P/scripts/detect-mem.py" "${VALIDATE_ARGS[@]}"
else
  sudo python3 "$P/scripts/detect-mem.py" "${VALIDATE_ARGS[@]}"
fi
if [ -n "$REQUESTED_RAM_MB" ]; then
  RAM_MB=$REQUESTED_RAM_MB
  if [ -r /proc/iomem ] && grep -qE '^[0-9a-fA-F]*[1-9a-fA-F][0-9a-fA-F]*-.*: System RAM$' /proc/iomem 2>/dev/null; then
    python3 "$P/scripts/detect-mem.py" "${VALIDATE_ARGS[@]}" --ram-mb "$RAM_MB"
  else
    sudo python3 "$P/scripts/detect-mem.py" "${VALIDATE_ARGS[@]}" --ram-mb "$RAM_MB"
  fi
else
  CARVE_SIZE_MB=$((10#$CARVE_MB))
  if (( CARVE_SIZE_MB >= 4096 )); then
    RAM_MB=$((CARVE_SIZE_MB - 256))
  else
    RAM_MB=$((CARVE_SIZE_MB - 128))
  fi
  if (( RAM_MB > 7900 )); then RAM_MB=7900; fi
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
if command -v update-grub >/dev/null 2>&1 && [ -f /etc/default/grub ]; then
  sudo mkdir -p /etc/default/grub.d
  echo "GRUB_CMDLINE_LINUX=\"\$GRUB_CMDLINE_LINUX $CMDLINE_ARGS\"" | sudo tee "$G" >/dev/null
  sudo update-grub >/dev/null 2>&1
elif command -v grub2-mkconfig >/dev/null 2>&1 && [ -f /etc/default/grub ]; then
  sudo mkdir -p /etc/default/grub.d
  echo "GRUB_CMDLINE_LINUX=\"\$GRUB_CMDLINE_LINUX $CMDLINE_ARGS\"" | sudo tee "$G" >/dev/null
  for cfg in /boot/grub2/grub.cfg /boot/efi/EFI/*/grub.cfg; do
    [ -e "$cfg" ] && sudo grub2-mkconfig -o "$cfg" >/dev/null 2>&1 && break
  done
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
echo "recommended hosted XNU setting for /etc/xnulinux.conf: RAM_MB=$RAM_MB"
echo "after a reboot: Linux on $LINUX_MEM, fki.ko loaded with the $CARVE_MB MB carve-out at $CARVE_BASE (rebuild fki-ko before a reboot when it changes)"
