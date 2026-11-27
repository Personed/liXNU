# Native x86_64 code-path audit

Status on 2026-11-27, based on main `7712aac19400a3bc5cc1edf2f2d8c647b43b2978`.
This audit covers only the separate native hosted-XNU experiment. It does not
change the project goal or the ARM64 implementation.

## Current path and gates

| Layer | Current code | Evidence boundary |
| --- | --- | --- |
| XNU module selection | `xnu-ko/Makefile` links only `xnu_x86_64_stub.o` for `CONFIG_X86_64`; ARM64 still links `xnu_main.o`, `macho.o`, `adt.o`, and `world.o`. | Normal x86 `xnu.ko` cannot call ARM64 loader/world-switch code. |
| XNU module init | `xnu-ko/xnu_x86_64_stub.c` checks the documented CPU feature floor, then returns `-EOPNOTSUPP` even when the features exist. | The feature check does not enable execution. The x86 module load test remains a refusal test in a disposable VM. |
| FKI x86 backend | `fki-ko/fki_x86_64.c` supplies CPU ID, monotonic counter, alternate Linux stacks, and an FPU save boundary. `fki_caps()` returns zero. | There is no x86 implementation of `fki_enter/leave`, PAC swap, address-space mapping, user contexts, or exception delivery. The shared `fki.h` state/capability API still includes ARM concepts; it must not be reinterpreted as an x86 ABI. |
| Mach-O inspection | `xnu-ko/macho_parse_x86_64.c` and its fixtures inspect a bounded synthetic `MH_EXECUTE` subset. The x86 branch in `xnu-ko/macho.c` parses and then returns `-EOPNOTSUPP`. | The parser does not load, relocate, apply fixups, allocate executable pages, or enter XNU. Its object is compiled for validation but is not linked into the normal x86 `xnu.ko`. |
| ARM64 loader and handoff | `xnu-ko/xnu_main.c`, `world.S`, and `world.h` use `_arm_init`, `linux64_abi.h`, PAC, AArch64 system registers, and ARM page-table setup. | These are not an x86 boot path. `tests/x86_64/build-xnu-main.sh` is only a diagnostic compile attempt and is not in the required x86 workflow; its Kbuild invocation also needs an explicit architecture before it can be treated as x86 evidence. |
| Host boundary probe | Merged PR #97 strengthens the disposable Linux-to-Linux call/return probe for normal return and software abort. | It does not switch CR3/GS, route XNU syscalls or faults, validate XNU boot arguments, or enter a kernel image. The #87 contract keeps exception-ownership design and real-artifact ABI pinning as separate gates. |

## Artifact blocker

PR #56 is pinned to public XNU source `xnu-12377.121.6` (26.5) while the
runtime target is Tahoe 26.6.2. Its current Intel-native run failed closed
before kernel compilation because the official Xcode SDK scan found no
`TrustCache/API.h` v4; the Apple Silicon cross-build was skipped without a
matching KDK. No `kernel.development` artifact exists. The missing official
TrustCache/KDK input blocks real-image inspection and source-to-artifact ABI
confirmation. It does not block isolated checks derived from the pinned public
source, but those checks cannot establish 26.6.2 compatibility.

## Safe slice in this change

This change adds a source-pinned x86 `boot_args` v2/revision-0 layout and a
core-field fixture builder based on
`pexpert/pexpert/i386/boot.h` in the pinned XNU commit. It asserts the 4096-byte
structure and key field offsets, validates EFI memory-map descriptor extents,
requires 64-bit EFI mode, and rejects any physical range that cannot be
represented by the source structure's 32-bit address fields. Synthetic tests
cover valid core fields, malformed lengths, unsupported versions/modes,
unterminated command lines, and low-address boundary overflow.

The builder initializes only a core subset. EFI system-table, runtime-service,
video, security, and target-specific entry fields remain unset. The helper is
compiled as a validation-only object and tested on the host; it is not included
in the x86 `xnu.ko` object list or called by the ARM64 module. A passing fixture
means only that this source-pinned data layout and range checker behave as
specified. It does not produce bootable arguments or prove the real artifact's
handoff ABI.

## Next step

After #56 produces an official Intel-native kernel artifact, run the existing
read-only inspector against that exact file and compare its source/configuration
manifest with the boot-argument version and entry protocol. If the artifact
confirms this layout, extend the isolated fixture to the artifact-required
fields and memory-map rules. Keep `fki_caps()==0` and the normal x86 refusal
stub until the loader, context transaction, exception/syscall ownership, and
return paths have their own reviewed evidence.
