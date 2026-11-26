#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Install or uninstall the xnulinux CLI on a Linux/macOS PC.
# Non-root users default to ~/.local (no sudo required); root installs a root-owned
# copy under $PREFIX/lib/xnulinux so `sudo xnulinux` never executes user-writable checkout files.
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

BIN_DIR="$PREFIX/bin"
LIB_DIR="$PREFIX/lib/xnulinux"
TARGET="$BIN_DIR/xnulinux"

need_sudo() {
  [ "$(id -u)" -ne 0 ] && ! { [ -w "$PREFIX" ] || { [ ! -e "$PREFIX" ] && [ -w "$(dirname "$PREFIX")" ]; }; }
}

if [ "$UNINSTALL" = 1 ]; then
  if need_sudo; then
    sudo rm -rf "$TARGET" "$LIB_DIR"
  else
    rm -rf "$TARGET" "$LIB_DIR"
  fi
  echo "removed $TARGET"
  exit 0
fi

# When installing to a system/root-owned prefix, copy the runtime tree into a
# root-owned read-only directory ($PREFIX/lib/xnulinux) instead of pointing back
# at a user-writable checkout.
if [ "$(id -u)" -eq 0 ] || need_sudo; then
  SUDO=""
  need_sudo && SUDO="sudo"
  $SUDO mkdir -p "$BIN_DIR"
  $SUDO rm -rf "$LIB_DIR"
  $SUDO mkdir -p "$LIB_DIR"
  for sub in scripts tools fki-ko xnu-ko tests; do
    [ -d "$ROOT/$sub" ] && $SUDO cp -R "$ROOT/$sub" "$LIB_DIR/$sub"
  done
  $SUDO chown -R 0:0 "$LIB_DIR"
  $SUDO chmod -R go-w "$LIB_DIR"
  $SUDO tee "$TARGET" >/dev/null <<EOF
#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
export XNULINUX_HOME="$LIB_DIR"
exec "$LIB_DIR/scripts/xnulinux" "\$@"
EOF
  $SUDO chmod 0755 "$TARGET"
  $SUDO chown 0:0 "$TARGET"
  echo "installed $TARGET -> $LIB_DIR/scripts/xnulinux (root-owned copy)"
else
  mkdir -p "$BIN_DIR"
  cat > "$TARGET" <<EOF
#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
export XNULINUX_HOME="$ROOT"
exec "$ROOT/scripts/xnulinux" "\$@"
EOF
  chmod 0755 "$TARGET"
  echo "installed $TARGET -> $ROOT/scripts/xnulinux"
fi

"$TARGET" doctor || true
