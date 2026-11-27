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
  # Prefer journalctl -k -f when systemd-journald is running and actively
  # collecting kernel messages (not ReadKMsg=no): util-linux dmesg (/dev/kmsg)
  # can stop delivering records after a flood of XNU output while journald keeps
  # receiving them (see scripts/vm-dmesg-shell.sh).
  SYSTEMD_RUNTIME_DIR=${SYSTEMD_RUNTIME_DIR:-/run/systemd/system}
  JOURNALD_CONF_DIR=${JOURNALD_CONF_DIR:-/etc/systemd}
  JOURNAL_HAS_KMSG=0
  if command -v journalctl >/dev/null 2>&1 && [ -d "$SYSTEMD_RUNTIME_DIR" ]; then
    READ_KMSG_AWK='
      function finish_file() {
        if (continuation != "") {
          process_line(continuation)
          continuation = ""
        }
        in_journal = 0
      }
      function process_line(line, s, v) {
        if (from_cat_config && line ~ /^# \/.*\/systemd\/journald\.conf(\.d\/[^\/]+\.conf)?$/) {
          in_journal = 0
          return
        }
        if (line ~ /^[[:space:]]*[#;]/ || line ~ /^[[:space:]]*$/) return
        if (line ~ /^[[:space:]]*\[[^]]*\][[:space:]]*$/) {
          s = line
          gsub(/^[[:space:]]*\[|\][[:space:]]*$/, "", s)
          in_journal = (s == "Journal")
          return
        }
        if (in_journal && line ~ /^[[:space:]]*ReadKMsg[[:space:]]*=/) {
          v = line
          sub(/^[[:space:]]*ReadKMsg[[:space:]]*=[[:space:]]*/, "", v)
          sub(/[[:space:]]*$/, "", v)
          v = tolower(v)
          if (v ~ /^(1|yes|y|true|t|on)$/) last = "yes"
          else if (v ~ /^(0|no|n|false|f|off)$/) last = "no"
        }
      }
      FNR == 1 { finish_file() }
      {
        sub(/\r$/, "")
        line = $0
        if (from_cat_config && line ~ /^# \/.*\/systemd\/journald\.conf(\.d\/[^\/]+\.conf)?$/) {
          if (continuation != "") {
            process_line(continuation)
            continuation = ""
          }
          process_line(line)
          next
        }
        if (continuation != "" && line ~ /^[[:space:]]*[#;]/) next
        if (continuation != "") {
          line = continuation line
          continuation = ""
        }
        if (line ~ /\\[[:space:]]*$/) {
          sub(/\\[[:space:]]*$/, " ", line)
          continuation = continuation line
          next
        }
        process_line(line)
      }
      END {
        finish_file()
        print last
      }
    '
    READ_KMSG=$(
      if [ "$JOURNALD_CONF_DIR" = /etc/systemd ] && command -v systemd-analyze >/dev/null 2>&1; then
        (SYSTEMD_COLORS=0 systemd-analyze cat-config systemd/journald.conf 2>/dev/null || true) | awk -v from_cat_config=1 "$READ_KMSG_AWK"
      else
        conf_files=()
        for conf in "$JOURNALD_CONF_DIR/journald.conf" "$JOURNALD_CONF_DIR/journald.conf.d"/*.conf; do
          [ -f "$conf" ] && [ -r "$conf" ] && conf_files+=("$conf")
        done
        if [ "${#conf_files[@]}" -gt 0 ]; then
          awk -v from_cat_config=0 "$READ_KMSG_AWK" "${conf_files[@]}"
        fi
      fi
    )
    case "$READ_KMSG" in
      no) JOURNAL_HAS_KMSG=0 ;;
      *)
        if [ -n "$( (sudo journalctl -k -n 1 -q --no-pager 2>/dev/null || journalctl -k -n 1 -q --no-pager 2>/dev/null) | head -c 1)" ]; then
          JOURNAL_HAS_KMSG=1
        fi ;;
    esac
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
