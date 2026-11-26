#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# On the Ubuntu build machine: the DroidVM Linux toolchain (native compile, no Darling: the kernel build does not need
# it) and the xnu-12377 kernel from source.
set -ex
POC=$(cd "$(dirname "$0")/.." && pwd)
export DEBIAN_FRONTEND=noninteractive
if [ ! -x /usr/lib/llvm-21/bin/clang ]; then
  . /etc/os-release
  LLVM_CODENAME=${LLVM_CODENAME:-${VERSION_CODENAME:-}}
  [ -n "$LLVM_CODENAME" ] || { echo "Set LLVM_CODENAME to the apt.llvm.org distribution codename" >&2; exit 1; }
  wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc >/dev/null
  echo "deb http://apt.llvm.org/$LLVM_CODENAME/ llvm-toolchain-$LLVM_CODENAME-21 main" | sudo tee /etc/apt/sources.list.d/llvm21.list
  sudo apt-get update -qq
fi
sudo -E apt-get install -y -qq build-essential autoconf automake libtool pkg-config git python3 python3-yaml python3-packaging \
  cmake ninja-build meson bison flex m4 unifdef tcsh pax libbsd-dev uuid-dev libssl-dev zlib1g-dev cpio xz-utils zstd llvm-21 clang-21 lld-21 \
  libmd-dev linux-headers-$(uname -r) 7zip p7zip-full >/dev/null
mkdir -p ~/w && cd ~/w
[ -d dvm ] || git clone -q https://github.com/Droid-VM/macOS-DroidVM dvm
cd dvm
HOST_MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || printf '%s-linux-gnu' "$(uname -m)")
python3 - "$HOST_MULTIARCH" <<'PY'
import sys
p='scripts/setup-env.py'; s=open(p).read()
a=s.find("    apple = linux / 'appleclang'"); b=s.find("    # bin26: the kernel's adapters")
if a>0 and b>a:
    s=s[:a]+"    # (xnupoc) no Darling/AppleClang: native_compile builds the kernel with host LLVM\n"+s[b:]
    open(p,'w').write(s)
s=open(p).read()
# Linux libbsd declares strlcat in bsd/string.h; XNU host tools use it while the generated fragment includes only stdlib/unistd.
needle="    text = text.replace(DEV_ROOT + '/toolchains/linux/host-deps', '').replace(DEV_ROOT, str(ROOT))\n"
insert=needle+"    if '-include bsd/string.h' not in text:\n        text = text.replace('-include bsd/stdlib.h -include bsd/unistd.h', '-include bsd/stdlib.h -include bsd/unistd.h -include bsd/string.h')\n"
if needle in s and insert not in s:
    s=s.replace(needle, insert)
    open(p,'w').write(s)
s=open(p).read()
# The committed gunyah-linux.mk names x86_64 multiarch paths; non-x86 build hosts substitute their own.
multiarch=sys.argv[1]
k="    (X / 'gunyah-linux.mk').write_text('\\n'.join(lines) + '\\n')"
patch=k.replace("'\\n'.join(lines)", f"'\\n'.join(lines).replace('x86_64-linux-gnu', '{multiarch}')")
if k in s and multiarch != 'x86_64-linux-gnu' and patch not in s:
    s=s.replace(k, patch)
    open(p,'w').write(s)
PY
# cctools' foreign mach/machine headers know x86 only; host tools (kextsymboltool, ...) only need LP64 types.
# Patched once the toolchain step has fetched them.
patch_cctools() {
  local d f T
  for d in $(find toolchains -type d -path '*/cctools/include/foreign' 2>/dev/null); do
    for f in $(grep -rl -E 'defined *\(__x86_64__\)' "$d"); do
      grep -q __aarch64__ "$f" || sed -i 's/defined *(__x86_64__)/defined(__x86_64__) || defined(__aarch64__)/g' "$f"
    done
    T=$d/mach/thread_status.h
    grep -q 'xnupoc' "$T" || sed -i 's|^typedef\tnatural_t\tthread_state_data_t\[THREAD_STATE_MAX\];|#ifndef THREAD_STATE_MAX /* xnupoc: aarch64 host */\n#define THREAD_STATE_MAX 144\n#endif\n&|' "$T"
    grep -n -B3 'thread_state_data_t\[' "$T"
  done
}
python3 scripts/setup-env.py toolchain --jobs 12
patch_cctools
# libclang_rt.cc_kext comes from the CLTools package (unpacked only; Darling is not needed for it)
A=toolchains/linux/appleclang; mkdir -p $A
python3 compat/xnu-linux/scripts/setup-appleclang.py fetch --directory $A
[ -d $A/apple-tools ] || python3 compat/xnu-linux/scripts/setup-appleclang.py extract --directory $A
ls $A/apple-tools/Library/Developer/CommandLineTools/usr/lib/clang/*/lib/darwin/ | grep cc_kext
# the k27 tree first (setup-env's x12377 step makes it and runs its installhdrs at once), for decomment: it keeps getc()
# in a char, so EOF never matches where char is unsigned (aarch64) and it spins forever
python3 -c "import importlib.util as u; s = u.spec_from_file_location('setup_env', 'scripts/setup-env.py'); m = u.module_from_spec(s); s.loader.exec_module(m); m.kernel_tree()"
D=out/k27/xnu/SETUP/decomment/decomment.c
sed -i -E 's/^([[:space:]]*)char([[:space:]]+bufchar;)/\1int\2/' $D
grep -qE '^[[:space:]]*int[[:space:]]+bufchar;' $D || { echo "decomment.c: no int bufchar after the patch" >&2; exit 1; }
rm -f out/x12377/build/xnu/hdrs-obj/SETUP/decomment/decomment
# unifdef 2.10 loses conditional nesting after pfvar.h's bare #error; give it the diagnostic text it requires.
P=out/k27/xnu/bsd/net/pfvar.h
sed -i 's|^#error$|#error "PF_MD5_DIGEST_LENGTH != MD5_DIGEST_LENGTH"|' $P
grep -q '^#error "PF_MD5_DIGEST_LENGTH != MD5_DIGEST_LENGTH"' $P || { echo "pfvar.h: no diagnostic #error after the patch" >&2; exit 1; }
# The same unifdef rejects control lines whose trailing comment continues onto the next line.
python3 - <<'PYEOF'
p = 'out/k27/xnu/libkern/libkern/c++/OSEndianTypes.h'
s = open(p).read()
for name in ('__cplusplus', '! _OS_OSENDIANHELPER_H'):
    s = s.replace(f'#endif /* {name}\n        */', f'#endif /* {name} */')
open(p, 'w').write(s)
if '#endif /* __cplusplus */' not in s or '#endif /* ! _OS_OSENDIANHELPER_H */' not in s:
    raise SystemExit('OSEndianTypes.h: multiline directive comments remain')
PYEOF
python3 scripts/setup-env.py x12377 --jobs 12
# the first kernel build is the rebuild's: xnu-patches and xnulinux's corecrypto go into the tree before anything of it
# is compiled (XNU's own corecrypto never is)
DVM="$PWD" bash "$POC"/scripts/vm-xnu-rebuild.sh
