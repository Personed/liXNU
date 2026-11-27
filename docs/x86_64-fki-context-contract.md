# x86-64 FKI context-state contract draft

This document is the proposed architecture-specific register and transaction contract for the native x86-64 FKI/XNU port. It is a design boundary for implementation work, not evidence that the boundary is implemented or safe.

The contract exists because the ARM64 `fki.h` interface cannot be copied mechanically to x86-64. ARM64 hosting relies on exception-level separation, TTBR state, PSTATE, PAC keys, and Linux's ARM64 exception model. The x86-64 port instead has to account for long-mode versus protected-mode startup, segmentation state, control registers, descriptor tables, PCIDs, XSAVE state, and the fact that the architectural XNU entry protocol begins in 32-bit protected mode. `docs/x86_64-entry-abi-source-pin.md` records that source-level entry protocol; this document defines how a host-owned CPU context must describe, validate, enter, stop, and restore such a foreign context without exposing an unimplemented capability.

No part of this document authorizes setting an x86-64 `FKI_CAP_*` bit or entering XNU. While exception/memory ownership remains unresolved, x86-64 `fki_caps()` must remain zero and XNU module loading must remain fail-closed.

## Design goals

The contract must be able to:

1. snapshot enough host state to prove that Linux survives entry, normal return, fault recovery, shutdown, and abort paths;
2. describe the exact architectural state that will be active when foreign x86-64 code starts or resumes;
3. reject invalid or unsupported state before any foreign instruction executes;
4. preserve enough foreign and host state at every stop to diagnose and resume or abort deterministically;
5. support Intel and AMD without pretending that their feature sets are interchangeable;
6. separate experiment-only synthetic entry from any future authentic XNU entry profile.

The contract must not:

1. treat a Mach-O `LC_UNIXTHREAD` register record as the XNU kernel entry protocol;
2. expose host kernel pointers or Linux-specific structures to the foreign module;
3. require userspace ABI layout decisions before the native kernel-entry boundary exists;
4. claim that XSAVE state can be ignored because early synthetic tests happen not to use AVX;
5. imply that QEMU/KVM or QEMU TCG results validate the native x86-64 module path.

## Execution profiles

There are three distinct profiles. They are not interchangeable and must never share acceptance evidence.

### `HOST_BOUND_SHIM`

The CPU remains in Linux long mode with Linux-owned paging, descriptor tables, interrupt state, and control registers. The shim may save host state, call a bounded synthetic function, and restore host state. This is the only profile supported by the current boundary-probe work.

This profile validates the host-preservation oracle. It is not authentic XNU execution and must never be used as native-port acceptance.

### `XNU_PREBOOT_LONG`

The host or loader performs the machine setup normally done by XNU's protected-mode bootstrap, constructs the long-mode state expected at a precisely identified later XNU entry point, and enters that anchor directly. This profile avoids a live long-mode to protected-mode transition, but it is only valid if the skipped bootstrap work is enumerated and reproduced exactly for the selected artifact/source configuration.

This profile still gives the foreign kernel architectural control over GDT/IDT, page tables, exceptions, syscalls, MSRs, and possibly PCIDs. It cannot be enabled until the exception-forwarding and memory-ownership design below is implemented and validated.

### `XNU_COLD_BOOT`

The host recreates the source-pinned protected-mode environment and transfers control to XNU's architectural `_start`/`pstart` entry. This is artifact-faithful but much more invasive: the CPU must leave the host's normal long-mode execution environment, and Linux cannot execute on that CPU while paging and host control state are unavailable.

This profile is reserved for research only. It requires CPU quiescence, exact control-register and descriptor-state ownership, an abort/re-entry path, physical-address layout validation, and a demonstrated way to recover the CPU on faults, NMI, machine check, and unexpected halt or shutdown. No production acceptance may use it until those mechanisms exist and are validated.

## Versioning and shape

The x86-64 extension must carry an explicit ABI version and fixed-size control block version independently of the architecture-neutral FKI version. A caller supplies the highest version it understands; the host rejects zero, unknown, truncated, or misaligned state. Fields added in later versions must be appended without changing the meaning of earlier fields, and the host must reject nonzero bytes in reserved space.

All integer fields use fixed-width little-endian x86-64 layout. No Linux structure, host pointer, file descriptor, or architecture-neutral ARM64 state crosses the interface. Opaque host handles remain opaque integers or pointers meaningful only to the host implementation.

A state object is never implicitly valid because it was copied from a register dump. It becomes usable only after validation succeeds and the host has taken its own restoration snapshot.

## Register state

The initial fixed control block must represent the following state explicitly. Names are descriptive; exact C layout is implementation work after this contract is accepted.

| Group | Required state |
| --- | --- |
| transaction metadata | ABI version, state size, profile, entry/resume operation, flags, stop mask, feature bitmask, reserved fields |
| GPRs | RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP, R8-R15, RIP |
| flags | RFLAGS, including fixed bit 1, interrupt flag policy, IOPL policy, and reserved-bit validation |
| selectors | CS, SS, DS, ES, FS, GS, TR, LDTR, with mode-appropriate validation |
| segment bases | FS base, GS base, KERNEL_GS base |
| control state | CR0, CR2 for stop reporting, CR3 including PCID low bits, CR4, EFER, XCR0 |
| descriptor state | GDTR base/limit, IDTR base/limit |
| XSAVE state | out-of-line standard-format XSAVE area pointer, byte length, feature mask, and explicit compaction/supervisor-state policy |
| boot payload | abstract boot-argument physical or linear address plus size/identity information when the profile uses the XNU boot protocol |
| stop/reporting | stop reason, vector, error code, CR2, instruction pointer, stack pointer, and machine-readable auxiliary data |

The exact representation of CR3 must distinguish the page-table root physical address from PCID low bits. The implementation must reject a nonzero PCID when PCIDE is disabled or unsupported, reserved control-register bits, non-canonical long-mode addresses, invalid fixed RFLAGS bits, malformed XSAVE metadata, and combinations inconsistent with the selected profile.

### XSAVE policy

The XSAVE area is out-of-line and variable length. Version 1 accepts only the architecturally defined standard format; compacted and supervisor-only formats are rejected until their use is designed. The recorded `xstate_bv` must be a subset of XCR0, XCR0 must be supported by the host CPU, and unsupported extended features must not be enabled merely because a hypervisor or CPUID leaf exposes them.

The host must snapshot and restore enough floating-point/vector state even in `HOST_BOUND_SHIM`; otherwise a synthetic call that uses SSE/AVX can silently corrupt Linux or another task. The boundary probe's XCR0 and sentinel checks are the host-side oracle, not a substitute for transaction state validation.

## Transaction lifecycle

The transaction states are:

`INIT -> VALIDATED -> ARMED -> FOREIGN -> STOPPED -> RESTORED`

with `ABORTED` reachable from `ARMED`, `FOREIGN`, or `STOPPED`.

1. `INIT`: the caller fills architectural state and metadata.
2. `VALIDATED`: the host checks version, shape, alignment, profile, feature support, control-state consistency, memory references, and stop-mask policy. No foreign state is loaded.
3. `ARMED`: the host snapshots its own complete restoration state and prepares the entry trampoline. The CPU is pinned and preemption/interrupt ownership is explicit.
4. `FOREIGN`: control is in the foreign or synthetic context. Linux does not run normally on that CPU in this state.
5. `STOPPED`: control returned to the host trampoline, foreign architectural state was captured, and the stop reason is stable.
6. `RESTORED`: host control, descriptor, paging, interrupt, FPU/vector, and task state were restored and compared against the host oracle.
7. `ABORTED`: restoration failed or a stop cannot be represented. The module must fail closed, poison the transaction, and prevent reuse.

Partial entry is possible. Every setup step must have reverse-order teardown, and a failure after any host state is changed must still pass through the same restoration oracle. A caller must not resume a transaction after `ABORTED`.

## Stop reasons

The machine-readable stop taxonomy must distinguish at least:

- normal cooperative return or host call;
- timer/yield/request to let Linux run;
- user or foreign syscall boundary event;
- page fault or other translation fault;
- synchronous architectural exception;
- external interrupt, NMI, or machine check;
- shutdown, halt, triple-fault equivalent, or unrecoverable foreign state;
- host invariant violation or restoration failure.

A page-fault stop records the vector, error code, CR2, foreign RIP/RSP/RFLAGS, active CR3/PCID, and the translation-ownership class if known. A syscall stop must not be conflated with a Linux syscall merely because the same architectural instruction is used; XNU syscall semantics are a separate contract.

## Linux calls that can sleep

The x86_64 branch of `fki_arch_linux_runs()` can switch to a single alternate stack for the current CPU. The common FKI calls include `fki_yield()` and `fki_idle_until()`, which enable preemption, and file and service calls that can block. A per-CPU stack can still hold a suspended callback when another task runs on that CPU. Pinning the task to that CPU does not prevent the second task from running. That stack also lies outside the task stack Linux uses for normal unwinding.

Treat a request that can run Linux code as a cooperative stop. Save the foreign continuation in host-owned transaction storage and restore the Linux execution context, including the saved task stack pointer and its return frame, before running the operation in C. At that point Linux must own GS, paging, exception and syscall entry, and floating-point state. Run a blocking operation only when the restored Linux context can sleep. Balance the stop path's interrupt and preemption masking and use a Linux task frame that the configured kernel unwinder understands. Keep the request data and reply buffer in memory accessible under Linux page tables for the lifetime of the operation.

Tie each pending request to one task and one transaction generation. A second task on the same CPU uses its own task stack. Reserve any per-CPU foreign context slot that the pending request depends on until the continuation is consumed or invalidated. The reservation must allow Linux scheduling and must survive a blocking call without holding a spinlock. The first implementation should bind the owner task to an online CPU and prevent CPU offline for the transaction. After handling the request, verify task/CPU ownership and the current stack bounds, then capture fresh Linux restoration state before re-entry. Consume the continuation once. Invalidate it on cancellation, timeout, or an ownership failure, and keep outstanding request buffers until completion or cancellation is acknowledged.

The first test can use `HOST_BOUND_SHIM` in a disposable Linux VM. Have the shim return an identified request to its Linux task, block that task with a timeout, and run another task on the same CPU while the request is pending. Require an observed context switch. Check that the handler runs within the requesting task's stack, obtains the expected reply after waking, and resumes the corresponding synthetic continuation exactly once. Check that cancellation or timeout leaves a stale continuation unusable. Apply the existing host state, syscall, unload and guest power-off checks to each result.

## Exception and interrupt ownership

This is the central unresolved x86-64 problem. Linux and XNU both expect architectural control of IDT entries, exceptions, syscalls, interrupt delivery, and low-level CPU state. A contract that only saves and restores registers is insufficient.

Before `XNU_PREBOOT_LONG` or `XNU_COLD_BOOT` may expose any capability, the implementation must define and validate one explicit ownership model:

- which descriptor tables are active while foreign code runs;
- how synchronous exceptions are captured without corrupting Linux;
- how NMI and machine check are handled;
- how external interrupts are held, redirected, or delivered;
- how the CPU returns to a host trampoline before Linux state is needed;
- how nested faults and faults in the trampoline itself are contained;
- how other CPUs are prevented from depending on the borrowed CPU.

If the answer requires XNU cooperation or patches, those patches are part of the trusted boundary and must be tied to the exact source revision/configuration. If safe ownership cannot be implemented without virtualization or a stronger hardware mechanism, the port must say so rather than entering foreign code anyway.

## Memory and paging ownership

Memory ownership is outside the register structure but mandatory for validation. Every mapping referenced by foreign state must be classified before entry:

- host-owned Linux memory that foreign code must not write;
- host-managed backing memory assigned to the foreign kernel;
- foreign-owned page-table memory and the physical address space it describes;
- shared user memory, if a future user-execution design supports it;
- memory-mapped I/O or firmware regions, which version 1 rejects unless explicitly designed.

CR3 validation requires knowing who owns the referenced page tables, whether Linux can reclaim them, whether the foreign kernel may write them, and how TLB invalidation is performed. PCID use requires an invalidation and migration policy. A CPU must not migrate while using foreign CR3/PCID state unless the transaction explicitly supports migration.

## Intel and AMD separation

CPUID feature bits are necessary but not sufficient. Validation records vendor, family/model/stepping, required baseline features, PCID/INVPCID support, XSAVE/XCR0 support, NX, paging width, speculative-control MSRs when relevant, and any vendor-specific errata or mitigation constraints. Intel evidence must not be presented as AMD evidence or vice versa.

AMD use additionally requires matching user-provided kernel patches and a separate artifact/configuration audit. The contract does not encode those patches and does not authorize them.

## Validation gates

The implementation must add automated fixtures before exposing a capability:

1. fixed-layout and reserved-byte tests for every versioned structure;
2. invalid-state rejection fixtures for control registers, descriptor state, RFLAGS, XSAVE metadata, PCID, and inconsistent mode combinations;
3. transaction-state machine tests covering setup failure, normal return, resume, and abort;
4. disposable-VM host-preservation tests extending the boundary probe to entry, faults, shutdown, and abort;
5. Intel/AMD feature-matrix tests with explicit unsupported-feature failures;
6. artifact-specific source/configuration binding before any authentic XNU entry profile is enabled.

Passing those tests proves only the tested boundary mechanics. Native hosted-XNU support additionally requires an architecture-matched Tahoe 26 artifact, memory/exception/syscall ownership, complete boot evidence, shutdown evidence, and separate physical Intel and AMD validation.
