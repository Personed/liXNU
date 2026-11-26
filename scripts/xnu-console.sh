#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Interactive terminal on XNU's /dev/console.
# Works locally on the Linux host/VM where xnu.ko runs (reading dmesg/journalctl and writing CONSOLE_IN) or from a
# remote host via serial-cap.log + scripts/vm.sh.
# Usage: xnu-console.sh [--local | --remote]
set -u
P=$(cd "$(dirname "$0")/.." && pwd)
CONSOLE_IN=${CONSOLE_IN:-/var/tmp/console-in}
LOG=${LOG:-$P/../serial-cap.log}
MODE=auto
case "${1:-}" in
  --local) MODE=local ;;
  --remote) MODE=remote ;;
  -h|--help)
    echo "Usage: $0 [--local | --remote]"
    echo "Environment: CONSOLE_IN=$CONSOLE_IN LOG=$LOG"
    exit 0 ;;
esac

if [ "$MODE" = auto ]; then
  if [ -d /sys/module/xnu ] || [ ! -e "$LOG" ]; then
    MODE=local
  else
    MODE=remote
  fi
fi

if [ "$MODE" = local ]; then
  if command -v dmesg >/dev/null 2>&1; then
    (sudo dmesg -w 2>/dev/null || dmesg -w 2>/dev/null) | tr -d '\r' |
      sed -un 's/^\[ *[0-9.]*\] fki: darwin console: //p; s/^fki: darwin console: //p' &
  elif command -v journalctl >/dev/null 2>&1; then
    (sudo journalctl -k -f -n 0 -o short-monotonic --no-pager 2>/dev/null || journalctl -k -f -n 0 -o short-monotonic --no-pager) |
      tr -d '\r' | sed -un 's/^\[ *[0-9.]*\] .*fki: darwin console: //p' &
  else
    echo "xnu-console: neither dmesg nor journalctl found" >&2
    exit 1
  fi
  TP=$!; trap 'kill $TP 2>/dev/null' EXIT
  while IFS= read -r line; do
    printf '%s\n' "$line" >> "$CONSOLE_IN"
  done
else
  tail -n 0 -F "$LOG" | tr -d '\r' | sed -un 's/^\[ *[0-9.]*\] fki: darwin console: //p' &
  TP=$!; trap 'kill $TP 2>/dev/null' EXIT
  while IFS= read -r line; do
    printf '%s\n' "$line" | bash "$P/scripts/vm.sh" "cat >> '$CONSOLE_IN'"
  done
fi
