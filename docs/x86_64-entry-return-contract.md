# x86_64 test-only entry/return contract

## Status and scope

This document defines the evidence required before a test-only x86_64 entry/return implementation may be connected to XNU. It does not add an entry path, change the FKI ABI, or enable execution. The normal x86_64 `xnu.ko` must continue to return `-EOPNOTSUPP`, and the x86_64 FKI backend must continue to report `fki_caps() == 0`.

The disposable-VM boundary probe (PR #71 and its test-only contract extension) validates a Linux-to-Linux call/return boundary. It checks callee-saved GPRs, host and alternate stack bounds/alignment/canaries, selected RFLAGS, FS/GS/KERNEL_GS, CR3 root and PCID, preemption/IRQ state, and XCR0 before and after a synthetic assembly call. The ioctl also exercises `kernel_fpu_begin/end`; userspace captures and compares its complete standard-format XSAVE image for all XCR0-enabled components around the raw ioctl syscall, as well as explicit YMM patterns. It checks Linux syscalls before and after and exercises normal/software-abort returns on each requested online vCPU. The XSAVE comparison describes the calling task's enabled user state, not supervisor XSS state or a foreign XNU vector context. This probe does not enter XNU, switch page tables or GS, route exceptions, or validate XNU boot arguments. Its success is necessary host-ABI evidence, not an XNU entry proof.

The contract extension strengthens the existing #71 probe in place with explicit stack, preemption/IRQ, XCR0 and complete enabled-user-XSAVE oracles. It remains a disposable-VM test artifact and does not add a production FKI interface or an XNU transition.

## Non-negotiable safety rules

- The first entry-envelope implementation, when its prerequisites are met, must be a separately named test artifact loaded only in a disposable x86_64 Linux VM. It must not be packaged, autoloaded, or enabled by a runtime parameter on normal artifacts.
- The test artifact must assert that `fki_caps() == 0`. Do not define or expose an x86 execution capability until its ABI and every advertised operation have passed the tests below.
- Never treat the parser's `LC_UNIXTHREAD` RIP as an XNU handoff address without confirming the pinned Tahoe entry protocol. Do not map, rebase, apply fixups, allocate executable XNU pages, or jump into an image during this contract's synthetic-entry phase.
- Do not replace Linux's IDT, page-fault, general-protection, NMI, or syscall path as part of the synthetic-entry phase. A future exception-routing experiment needs an independently reviewed per-CPU ownership and rollback design before fault injection is attempted.
- Once a test switches to foreign state, no Linux C callback may run until the Linux stack, flags, task/CPU identity, address-space state, and enabled extended state have been restored. Any failure to prove restoration must abort before a later transition.
- Keep the existing x86_64 `xnu.ko` stub unchanged until its architecture-specific gates pass. This x86 test contract does not edit the ARM64 implementation; its existing behavior and required regression coverage remain in force.

## Test contract

Each row is a separate gate. The harness records the kernel release/configuration, CPU vendor/model/features, vCPU count, artifact hashes, and the furthest passing gate. A failure aborts the sequence and must leave Linux usable; a later gate must not hide an earlier failure.

| Gate | Test-only operation | Required oracle |
| --- | --- | --- |
| Artifact preflight | Read the successful Intel-native Tahoe build artifact without loading it. Run the production parse-only inspector and retain its JSON and SHA-256. | File type, CPU type/subtype, segments, entry-state record, and fixup metadata are reported. Unsupported structure returns an error before any executable-image allocation or mapping; the inspector may allocate an ordinary read-only input buffer. This is structural validation only; the parser does not load or interpret fixups. |
| ABI pinning | Compare the exact artifact and build configuration with the pinned Tahoe source, symbols, entry assembly, boot-argument definitions, and fixup/linker rules. | A reviewed record names the actual XNU handoff symbol/protocol, register and stack inputs, required memory map, and artifact-specific fixup requirements. `LC_UNIXTHREAD` alone is not accepted as proof. |
| Linux boundary baseline | Run the existing #71 probe in a disposable VM before adding new transitions. | The saved host snapshot and its explicit comparisons pass for normal return and software abort; Linux syscalls work before and after; test modules unload; the guest powers off. This baseline does not claim XNU entry. |
| Synthetic entry envelope | Call a test-owned assembly shim on a selected online CPU via `work_on_cpu`, using Linux's current address space and no XNU image. Exercise a normal return and an explicit software-abort return. | Before/after snapshots match for CPU/task identity, task-stack bounds, RSP, relevant RFLAGS, FS/GS/KERNEL_GS, CR3 root and PCID, preemption/IRQ state, and XCR0. At the shim entry, `RSP % 16 == 8` under the test ABI, and the alternate stack bounds/canaries are valid. Canary GPRs and the calling task's complete standard XSAVE image for enabled XCR0 user components match; the shim's return status is exact; Linux syscalls still work afterward. No C callback executes while the test-owned stack/register state is active. |
| Exception isolation design | Before any #PF/#GP/NMI injection, review a per-CPU recovery mechanism and its interaction with Linux vector ownership, nested faults, and unwind. | The design identifies the exact handler ownership, entry stack, saved frame, recovery target, and rollback for partial setup. Until this is reviewed and implemented, exception injection is a **not-run blocker**, not an assumed pass. |
| Exception recovery test | In a disposable VM, inject one controlled #PF, one #GP, and an NMI into the test-owned shim, one at a time, after the prior case has restored Linux. | The test records the expected vector and fault metadata, returns to the documented recovery point exactly once, and passes the same host snapshot/syscall/unload/shutdown checks. A nested or unclassified fault fails closed and must not continue into XNU. |
| Shutdown and cleanup | Exercise normal return, software abort, and each supported recovery path; then unload test modules and power off the guest. | All temporary mappings, stacks, registrations, and per-CPU state are removed. Linux remains responsive until guest shutdown. No case uses the normal service or host boot path. |

The synthetic entry-envelope gate is intentionally narrower than an XNU run. It proves that a future test-only assembly boundary can enter and return from a controlled shim without losing the selected Linux host state. It does not prove the Tahoe ABI, memory isolation, fixups, syscall routing for XNU tasks, exception delivery, or macOS boot.

## Evidence currently missing

These inputs block adapting a test boundary to the XNU image/entry, changing the FKI execution ABI, or making an XNU handoff. The strengthened Linux-only shim supplies additional host-state oracles without the Tahoe artifact, but it does not advance or claim native XNU execution. The concrete handoff prerequisites are:

1. **A successful Intel-native artifact.** The current #56 diagnostics contain no `kernel.development`; there is therefore no artifact hash, file type, image layout, or successful Intel build to inspect. A cross-build on Apple Silicon is useful separately but does not replace this input.
2. **The artifact's real entry ABI.** Pin the source-level Tahoe entry symbol and its expected register/stack state, boot-argument structure/version, memory map, and CPU setup to the exact source revision/configuration that produced the artifact. Do not infer these from the ARM64 `xnu_main.c` handoff or the Mach-O thread-state record.
3. **Artifact-specific fixup/load requirements.** The production parser only validates a bounded subset of Mach-O and chained-fixup metadata. It neither interprets nor applies fixups, and it rejects kernel collections (`MH_FILESET`). Confirm the actual artifact/container format, every required pointer format and page size, and the loader/linker semantics before defining executable mappings.
4. **An x86_64 context contract for FKI.** The execution/state portions of `fki.h` still describe ARM concepts such as PAC keys, AArch64 user registers, and EL0/high-window capabilities. There is no versioned x86 state or transaction ABI for RIP/RSP/RFLAGS, GPRs, FS/GS bases, CR3/PCID, XSAVE, or exception stops. Do not reinterpret ARM fields or add a capability bit to stand in for this contract.
5. **A reviewed exception and syscall ownership design.** The current #71 probe deliberately leaves Linux IDT, #PF/#GP/NMI, and syscall routing untouched. It supplies no safe recovery path for an exception during foreign execution and does not route a hosted XNU task's syscall.

Until these inputs are available and the test-only gates pass, the supported behavior is the existing fail-closed refusal. A successful parse or compile does not change that state.

## Current evidence references

- `tests/x86_64/test-boundary-probe-vm.sh` runs the call/return probe only in a disposable Linux VM; it never loads XNU. The extension compares the full enabled user XSAVE image around each ioctl, but does not inspect supervisor XSAVE state.
- `tests/x86_64/test-xnu-stub-vm.sh` tests the XNU module's refusal in disposable VMs. Loading this proprietary module taints that guest before its initializer returns.
- `tools/dev/x86_64/xnu-macho-inspect` inspects a candidate artifact read-only; its accepted result does not prove loadability or execution.
- `docs/x86_64-native-port-design.md` describes the broader build, memory, exception, and hardware acceptance gates.
