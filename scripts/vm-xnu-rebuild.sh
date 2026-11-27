#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Inside the VM: apply xnulinux's XNU patches to the k27 tree, rebuild the kernel incrementally, and (when the macOS 27.2
# kernelcache is there) relink the kernel collection with Apple's kexts. The kernel's segment slots inside the collection
# (k27/segaddrs.sh) follow the kernel's size: when kc27-layout.py moves them, the kernel is linked again.
set -e -o pipefail
if [ -n "${POC:-}" ]; then
  POC=$(cd -- "$POC" >/dev/null && pwd -L)
else
  POC=$(CDPATH= cd -- "$(dirname "$0")/.." >/dev/null && pwd -L)
fi
DVM=${DVM:-$HOME/w/dvm}
DVM=$(cd -- "$DVM" >/dev/null && pwd -P)
cd "$DVM"
export PATH=$PATH:/usr/lib/llvm-21/bin
T=out/k27/xnu
# Restore every source file touched by last run's patches, then apply the current patch set. Identical files keep their times.
P="$POC/xnu-patches"
FILES=$(cat "$T/.linux64-patched" 2>/dev/null; grep -h '^+++ b/' "$P"/*.patch | sed 's|^+++ b/||')
SNAP=$(mktemp -d)
for f in $FILES; do [ -e "$T/$f" ] && mkdir -p "$SNAP/$(dirname "$f")" && cp -p "$T/$f" "$SNAP/$f"; done
for f in $FILES; do git -C "$T" checkout -q -- "$f" 2>/dev/null || rm -f "$T/$f"; done
for p in "$P"/*.patch; do git -C "$T" apply "$p"; echo "applied $(basename "$p")"; done
for f in $FILES; do [ -e "$SNAP/$f" ] && cmp -s "$SNAP/$f" "$T/$f" && touch -r "$SNAP/$f" "$T/$f"; done
rm -rf "$SNAP"
grep -h '^+++ b/' "$P"/*.patch | sed 's|^+++ b/||' > "$T/.linux64-patched"
# The machine layer's headers (shared with xnu.ko), where every component's sources find them.
mkdir -p "$T/EXTERNAL_HEADERS/linux64"
for h in "$P"/include/*.h; do cmp -s "$h" "$T/EXTERNAL_HEADERS/linux64/$(basename "$h")" || cp "$h" "$T/EXTERNAL_HEADERS/linux64/"; done
# Use xnulinux's corecrypto headers and kernel implementation, not XNU's internal-use-licensed copy.
mirror() { # mirror SRC DST: DST holds exactly SRC's files
  mkdir -p "$2"
  for f in $(ls -A "$2"); do [ -e "$1/$f" ] || rm -rf "$2/$f"; done
  for f in $(ls -A "$1"); do cmp -s "$1/$f" "$2/$f" || cp "$1/$f" "$2/$f"; done
}
rm -rf "$T/osfmk/corecrypto" "$T/osfmk/arm64/corecrypto"
mirror "$P/corecrypto/include/corecrypto" "$T/EXTERNAL_HEADERS/corecrypto"
mirror "$P/corecrypto/kernel" "$T/osfmk/xnulinux_crypto"
mkdir -p out/k27/obj
t=$SECONDS
set +e
JOBS="${JOBS:-12}" bash k27/build-kernel27.sh 2>&1 | tee out/k27/xnulinux-rebuild.log | tail -15
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ]; then
  echo "kernel rebuild failed ($rc); showing the bounded build logs that explain missing compiler recipes"
  for log in out/k27/build-bootstrap.log out/k27/build-raw.log; do
    if [ -f "$log" ]; then
      echo "===== $log (last 120 lines) ====="
      tail -120 "$log"
    fi
  done
  exit "$rc"
fi
echo "kernel: $((SECONDS - t)) s"
APPLE_KC=out/kc/kc27.bin
have_apple_kc=false
if [ -e "$APPLE_KC" ]; then
  have_apple_kc=true
  cp k27/segaddrs.sh /tmp/segaddrs.before
  python3 k27/kc27-layout.py "$APPLE_KC" out/k27/kernel-k27.macho --write k27/segaddrs.sh | tail -2
  if ! cmp -s /tmp/segaddrs.before k27/segaddrs.sh; then
    echo "kernel slots moved: linking the kernel again"
    JOBS="${JOBS:-12}" bash k27/build-kernel27.sh 2>&1 | tee out/k27/xnulinux-relink.log | tail -15
  fi
elif [ "${BUILD_NATIVE_KEXTS:-0}" = 1 ]; then
  # A source-built collection has no Apple kernelcache to define segment slots. Fit them to the kernel's __TEXT_EXEC.
  fit_log=out/k27/kernel-slot-fit.log
  python3 "$POC/scripts/free-kc/fit_kernel_segments.py" out/k27/kernel-k27.macho k27/segaddrs.sh 2>&1 | tee "$fit_log"
  if grep -q '^relink kernel:' "$fit_log"; then
    echo "free collection kernel slots need a relink"
    JOBS="${JOBS:-12}" bash k27/build-kernel27.sh 2>&1 | tee out/k27/xnulinux-relink.log | tail -15
  fi
  python3 "$POC/scripts/free-kc/fit_kernel_segments.py" --check out/k27/kernel-k27.macho k27/segaddrs.sh
fi

NATIVE=()
NETINC="$POC/build/netinc"
NETSRC="$POC/build/IONetworkingFamily-188"
mkdir -p "$NETINC/IOKit"
if "$have_apple_kc"; then
  KEXTS=("$POC"/kexts/*/*.json)
  # XnuEthernet uses private SPI declarations from Apple's open-source IONetworkingFamily.
  [ -e "$NETSRC/IOEthernetController.h" ] || git clone -q --depth 1 --branch IONetworkingFamily-188 https://github.com/apple-oss-distributions/IONetworkingFamily.git "$NETSRC"
  ln -sfn "$NETSRC" "$NETINC/IOKit/network"
else
  KEXTS=("$POC/kexts/XnuCPU/XnuCPU.json" "$POC/kexts/XnuDisk/XnuDisk.json" "$POC/kexts/XnuDrm/XnuDrm.json")
fi

# XnuDisk needs IOStorageFamily headers; the macOS collection also uses HID headers.
for sdk in "$PWD"/toolchains/sdks/*.sdk; do
  headers="$sdk/System/Library/Frameworks/Kernel.framework/Headers"
  [ -f "$headers/IOKit/storage/IOBlockStorageDevice.h" ] || continue
  if "$have_apple_kc" && [ ! -f "$headers/IOKit/hid/IOHIDDevice.h" ]; then
    continue
  fi
  for family in storage hid hidsystem graphics; do
    ln -sfn "$headers/IOKit/$family" "$NETINC/IOKit/$family"
  done
  ln -sfn "$sdk/usr/include/TargetConditionals.h" "$NETINC/TargetConditionals.h"
  ln -sfn "$headers/HIDDriverKit" "$NETINC/HIDDriverKit"
  break
done

for j in "${KEXTS[@]}"; do
  n=$(basename "$j" .json)
  if ! python3 scripts/build-native-kext.py "$n" "$(dirname "$j")/$n.cpp" -I "$NETINC" --obj "${KEXT_OBJ:-k27-obj}"; then
    if ! "$have_apple_kc"; then
      echo "kext $n is required for the PureDarwin collection" >&2
      exit 1
    fi
    echo "kext $n failed to build: left out"
    continue
  fi
  NATIVE+=("$j")
done

sudo install -d -m 0755 /lib/firmware/xnu
if "$have_apple_kc"; then
  printf '%s\n' "${NATIVE[@]}" > out/k27/natives.txt
  t=$SECONDS
  python3 "$POC/scripts/kclink/kclink.py" "$APPLE_KC" out/k27/kernel-k27.macho out/k27/kc-k27.bin \
    --keep $(grep -v '^#' "$POC/scripts/apple-kc/keep.txt") --native "${NATIVE[@]}" \
    --rebind $(grep -v '^#' "$POC/scripts/apple-kc/rebind.txt") > out/k27/kclink.log 2>&1 ||
    { tail -5 out/k27/kclink.log; echo "kclink failed: out/k27/kclink.log"; exit 1; }
  grep -E '^wrote|vtable check' out/k27/kclink.log
  echo "collection: $((SECONDS - t)) s"
  python3 "$POC/scripts/kc-amfi-platform.py" out/k27/kc-k27.bin "${AMFI_PLATFORM:-26}"
  sudo cp out/k27/kc-k27.bin /lib/firmware/xnu/kc.bin
fi
# natives.txt describes kexts to package with an Apple kernelcache; the free collection consumes object manifests instead.
if [ ! -e "$APPLE_KC" ]; then rm -f out/k27/natives.txt; fi
ls -la out/k27/kernel-k27.macho
sudo cp out/k27/kernel-k27.macho /lib/firmware/xnu/kernel.macho
sync
