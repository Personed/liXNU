#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Install or uninstall the xnulinux CLI wrapper on a Linux/macOS PC.
# Non-root users default to ~/.local (no sudo required). System-wide/root installs
# require the checkout itself to be root-owned and non-group/world-writable so
# `sudo xnulinux` never executes user-writable checkout files.
# Usage: install.sh [--prefix DIR] [--uninstall]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
if [ -z "${PREFIX:-}" ]; then
  if [ "$(id -u)" -eq 0 ]; then
    PREFIX=/usr/local
  else
    PREFIX="$HOME/.local"
  fi
fi
UNINSTALL=0

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)
      [ $# -ge 2 ] || { echo "install.sh: --prefix requires a directory" >&2; exit 2; }
      PREFIX=$2
      shift 2 ;;
    --uninstall)
      UNINSTALL=1
      shift ;;
    -h|--help)
      echo "Usage: $0 [--prefix DIR] [--uninstall]"
      exit 0 ;;
    *)
      echo "install.sh: unknown option '$1'" >&2
      exit 2 ;;
  esac
done

# Resolve relative prefixes without Python; keeping '..' components is safe because the
# wrapper receives an absolute path anchored to this invocation's working directory.
case "$PREFIX" in
  /*) ;;
  *) PREFIX="$(pwd -P)/$PREFIX" ;;
esac
BIN_DIR="$PREFIX/bin"
TARGET="$BIN_DIR/xnulinux"

need_sudo() {
  [ "$(id -u)" -ne 0 ] && ! { [ -w "$PREFIX" ] || { [ ! -e "$PREFIX" ] && [ -w "$(dirname "$PREFIX")" ]; }; }
}

stat_uid() {
  local uid
  uid=$(stat -c '%u' "$1" 2>/dev/null) || uid=$(stat -f '%u' "$1" 2>/dev/null) || return 1
  printf '%s' "$uid"
}

stat_mode_decimal() {
  local mode
  mode=$(stat -c '%a' "$1" 2>/dev/null) || mode=$(stat -f '%Lp' "$1" 2>/dev/null) || return 1
  case "$mode" in
    ''|*[!0-7]*) return 1 ;;
  esac
  printf '%s' "$((8#$mode))"
}

safe_global_checkout() {
  local path uid mode
  for path in "$ROOT" "$ROOT/scripts" "$ROOT/scripts/xnulinux"; do
    uid=$(stat_uid "$path") || return 1
    mode=$(stat_mode_decimal "$path") || return 1
    [ "$uid" = 0 ] || return 1
    (( (mode & 022) == 0 )) || return 1
    if [ -d "$path" ]; then
      (( (mode & 001) != 0 )) || return 1
    else
      (( (mode & 005) == 005 )) || return 1
    fi
  done

  # A root-owned checkout can still be replaced or hidden through a private or
  # user-writable parent directory, and ordinary users must be able to traverse it.
  path="$ROOT"
  while :; do
    [ -d "$path" ] || return 1
    uid=$(stat_uid "$path") || return 1
    mode=$(stat_mode_decimal "$path") || return 1
    [ "$uid" = 0 ] || return 1
    (( (mode & 001) != 0 )) || return 1
    if (( (mode & 022) != 0 )) && ! { (( (mode & 01000) != 0 )) && [ "$uid" = 0 ]; }; then
      return 1
    fi
    [ "$path" = / ] && break
    path=$(dirname "$path")
  done
}

if [ "$UNINSTALL" = 1 ]; then
  if need_sudo; then
    sudo rm -f "$TARGET"
  else
    rm -f "$TARGET"
  fi
  echo "removed $TARGET"
  exit 0
fi

if [ "$(id -u)" -eq 0 ] || need_sudo; then
  # Refuse global wrappers whose checkout files are writable or whose path users cannot safely traverse.
  if ! safe_global_checkout; then
    echo "install.sh: refusing system-wide install into '$PREFIX' pointing at checkout that is unsafe or inaccessible to users: '$ROOT'." >&2
    echo "Run without sudo to install for your user (defaults to ~/.local/bin/xnulinux), or place a root-owned checkout in a user-traversable system directory first." >&2
    exit 1
  fi
  SUDO=""
  need_sudo && SUDO="sudo"
  $SUDO mkdir -p "$BIN_DIR"
  $SUDO tee "$TARGET" >/dev/null <<EOF
#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
export XNULINUX_HOME="$ROOT"
exec "$ROOT/scripts/xnulinux" "\$@"
EOF
  $SUDO chmod 0755 "$TARGET"
  $SUDO chown 0:0 "$TARGET"
else
  mkdir -p "$BIN_DIR"
  cat > "$TARGET" <<EOF
#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
export XNULINUX_HOME="$ROOT"
exec "$ROOT/scripts/xnulinux" "\$@"
EOF
  chmod 0755 "$TARGET"
fi

echo "installed $TARGET -> $ROOT/scripts/xnulinux"
"$TARGET" doctor || true
