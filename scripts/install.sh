#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Install or uninstall the xnulinux CLI wrapper on a Linux/macOS PC.
# Non-root users default to ~/.local (no sudo required). System-wide/root installs
# require the checkout itself to be root-owned and non-group/world-writable so
# `sudo xnulinux` never executes user-writable checkout files.
# Usage: install.sh [--prefix DIR] [--uninstall]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
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

# Resolve relative --prefix to an absolute path so the wrapper works from any cwd.
PREFIX=$(python3 -c 'import os, sys; print(os.path.abspath(sys.argv[1]))' "$PREFIX")
BIN_DIR="$PREFIX/bin"
TARGET="$BIN_DIR/xnulinux"

need_sudo() {
  [ "$(id -u)" -ne 0 ] && ! { [ -w "$PREFIX" ] || { [ ! -e "$PREFIX" ] && [ -w "$(dirname "$PREFIX")" ]; }; }
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
  # Refuse to point a root-owned global wrapper at a non-root or group/world-writable checkout.
  if ! python3 - "$ROOT" "$ROOT/scripts" "$ROOT/scripts/xnulinux" <<'PY'
import os, stat, sys
for p in sys.argv[1:]:
    st = os.stat(p)
    if st.st_uid != 0 or (st.st_mode & (stat.S_IWGRP | stat.S_IWOTH)):
        sys.exit(1)
PY
  then
    echo "install.sh: refusing system-wide install into '$PREFIX' pointing at user-writable checkout '$ROOT'." >&2
    echo "Run without sudo to install for your user (defaults to ~/.local/bin/xnulinux), or place a root-owned (go-w) checkout first." >&2
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
