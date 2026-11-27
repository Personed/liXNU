#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Interactive terminal on XNU's /dev/console.
# Works locally on the Linux host/VM where xnu.ko runs (reading journalctl -k -f or dmesg -w and writing CONSOLE_IN)
# or from a remote host via serial-cap.log + scripts/vm.sh.
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

# Strip CR inside unbuffered sed (-u) rather than piping through `tr`, which
# block-buffers stdout in pipelines and hides short prompts/replies.
SED_FILTER='s/\r//g; s/^\[ *[0-9.]*\] .*fki: darwin console: //p; t; s/^\[ *[0-9.]*\] fki: darwin console: //p; t; s/^fki: darwin console: //p'

if [ "$MODE" = local ]; then
  # Prefer journalctl -k -f when systemd-journald is running and actually
  # collecting kernel messages (not ReadKMsg=no): util-linux dmesg (/dev/kmsg)
  # can stop delivering records after a flood of XNU output while journald keeps
  # receiving them (see scripts/vm-dmesg-shell.sh).
  JOURNAL_HAS_KMSG=0
  if command -v journalctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    if [ -n "$( (sudo journalctl -k -n 1 -q --no-pager 2>/dev/null || journalctl -k -n 1 -q --no-pager 2>/dev/null) | head -c 1)" ]; then
      JOURNAL_HAS_KMSG=1
    fi
  fi
  if [ "$JOURNAL_HAS_KMSG" = 1 ]; then
    (sudo journalctl -k -f -n 0 -q -o short-monotonic --no-pager 2>/dev/null ||
      journalctl -k -f -n 0 -q -o short-monotonic --no-pager) | sed -un "$SED_FILTER" &
  elif command -v dmesg >/dev/null 2>&1; then
    (sudo dmesg -w 2>/dev/null || dmesg -w 2>/dev/null) | sed -un "$SED_FILTER" &
  else
    echo "xnu-console: neither journalctl nor dmesg found" >&2
    exit 1
  fi
  TP=$!; trap 'kill $TP 2>/dev/null' EXIT
  while IFS= read -r line; do
    printf '%s\n' "$line" >> "$CONSOLE_IN"
  done
else
  tail -n 0 -F "$LOG" | sed -un "$SED_FILTER" &
  TP=$!; trap 'kill $TP 2>/dev/null' EXIT
  while IFS= read -r line; do
    printf '%s\n' "$line" | bash "$P/scripts/vm.sh" "cat >> '$CONSOLE_IN'"
  done
fi
