#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FIXTURE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/xnulinux-vm-net-path.XXXXXX")"
trap 'rm -rf -- "$FIXTURE_DIR"' EXIT

mkdir -p -- "$FIXTURE_DIR/bin" "$FIXTURE_DIR/sbin"
export IPTABLES_LOG="$FIXTURE_DIR/iptables.log"
export SUDO_LOG="$FIXTURE_DIR/sudo.log"
export SUDO_SECURE_PATH="$FIXTURE_DIR/sbin:/usr/bin:/bin"

cat >"$FIXTURE_DIR/bin/ip" <<'STUB'
#!/usr/bin/env bash
case "$*" in
  "link show xnu0") exit 0 ;;
  "route show default")
    [[ "${MOCK_NO_DEFAULT_ROUTE:-0}" == 1 ]] || printf 'default via 192.0.2.1 dev eth0\n'
    exit 0
    ;;
esac
exit 0
STUB

cat >"$FIXTURE_DIR/bin/sudo" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$SUDO_LOG"
if [[ "${1:-}" == sh && "${2:-}" == -c ]]; then
  exec env "PATH=$SUDO_SECURE_PATH" /bin/sh -c "$3"
fi
exec "$@"
STUB

cat >"$FIXTURE_DIR/bin/sysctl" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB

cat >"$FIXTURE_DIR/bin/dnsmasq" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB

cat >"$FIXTURE_DIR/bin/pkill" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB

cat >"$FIXTURE_DIR/bin/ethtool" <<'STUB'
#!/usr/bin/env bash
exit 1
STUB

cat >"$FIXTURE_DIR/sbin/iptables" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$IPTABLES_LOG"
[[ " $* " != *" -C "* ]]
STUB

chmod +x "$FIXTURE_DIR/bin/"* "$FIXTURE_DIR/sbin/iptables"

TEST_PATH="$FIXTURE_DIR/bin:/usr/bin:/bin"
if PATH="$TEST_PATH" command -v iptables >/dev/null 2>&1; then
  echo 'fixture error: iptables unexpectedly appears in the service PATH' >&2
  exit 1
fi

PATH="$TEST_PATH" "$ROOT_DIR/scripts/vm-net-up.sh" >"$FIXTURE_DIR/with-route.out" 2>&1
[[ -s "$IPTABLES_LOG" ]] || {
  cat "$FIXTURE_DIR/with-route.out" >&2
  echo 'vm-net-up did not find iptables through the privileged sudo PATH' >&2
  exit 1
}
grep -q -- '-t nat -A POSTROUTING' "$IPTABLES_LOG"
grep -q -- '-I FORWARD' "$IPTABLES_LOG"
grep -q -- '-t mangle -A FORWARD' "$IPTABLES_LOG"

: >"$IPTABLES_LOG"
: >"$SUDO_LOG"
MOCK_NO_DEFAULT_ROUTE=1 PATH="$TEST_PATH" "$ROOT_DIR/scripts/vm-net-up.sh" \
  >"$FIXTURE_DIR/no-route.out" 2>&1
[[ ! -s "$IPTABLES_LOG" ]] || {
  echo 'vm-net-up changed firewall rules without a default route' >&2
  exit 1
}
if grep -q '^sh -c command -v iptables$' "$SUDO_LOG"; then
  echo 'vm-net-up queried the privileged PATH when there was no default route' >&2
  exit 1
fi

echo 'PASS: vm-net-up discovers iptables in sudo secure_path without querying it when NAT is unnecessary'
