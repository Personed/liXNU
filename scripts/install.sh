#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Install or uninstall the xnulinux CLI wrapper on a Linux/macOS PC.
# Usage: install.sh [--prefix DIR] [--uninstall]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${PREFIX:-/usr/local}
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

BIN_DIR="$PREFIX/bin"
TARGET="$BIN_DIR/xnulinux"

if [ "$UNINSTALL" = 1 ]; then
  if [ -e "$TARGET" ] || [ -L "$TARGET" ]; then
    if [ -w "$BIN_DIR" ] || [ "$(id -u)" -eq 0 ]; then
      rm -f "$TARGET"
    else
      sudo rm -f "$TARGET"
    fi
    echo "removed $TARGET"
  else
    echo "$TARGET is not installed"
  fi
  exit 0
fi

if [ -w "$PREFIX" ] || { [ ! -e "$PREFIX" ] && [ -w "$(dirname "$PREFIX")" ]; } || [ "$(id -u)" -eq 0 ]; then
  mkdir -p "$BIN_DIR"
  cat > "$TARGET" <<EOF
#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
export XNULINUX_HOME="$ROOT"
exec "$ROOT/scripts/xnulinux" "\$@"
EOF
  chmod 0755 "$TARGET"
else
  sudo mkdir -p "$BIN_DIR"
  sudo tee "$TARGET" >/dev/null <<EOF
#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
export XNULINUX_HOME="$ROOT"
exec "$ROOT/scripts/xnulinux" "\$@"
EOF
  sudo chmod 0755 "$TARGET"
fi

echo "installed $TARGET -> $ROOT/scripts/xnulinux"
"$TARGET" doctor || true
