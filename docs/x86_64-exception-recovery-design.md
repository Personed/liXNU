# x86_64 controlled exception recovery design

This is a disposable-VM design for the next test-only boundary gate. It does not implement XNU entry, transfer Linux interrupt ownership to XNU, or change the production FKI/XNU ABI. `fki_caps()` and the normal x86_64 XNU probe must remain fail-closed until this mechanism is implemented and validated.

## Requirement

Before foreign kernel execution, a selected vCPU must survive one controlled page fault (`#PF`) and one controlled general-protection fault (`#GP`) raised inside the test-owned shim, one at a time. For each case the test must record the vector, faulting RIP, error code, CR2 for `#PF`, CPU identity, and recovery count; resume exactly once at the documented recovery point; then pass the existing host-state, syscall, unload, and shutdown checks. A nested or unclassified exception is a failed test, even if the disposable VM remains alive.

NMI is intentionally not claimed by this design. NMI can arrive outside the controlled instruction window, uses different completion semantics, and interacts with Linux's NMI-safe entry code and watchdogs. It remains a separate blocker.

## Why the exception table alone is not enough

Linux already has a recoverable-fault mechanism through the exception table. In the kernels relevant to this tree, both the kernel-mode page-fault path and `#GP` path first call `fixup_exception()` for faults in kernel text. That mechanism can redirect execution to a fixup target and is appropriate for ordinary Linux helper code.

It is not the full contract oracle: the fixup target is not given a structured copy of the vector, error code, faulting RIP, or CR2. Separate faulting instructions can identify the intended vector only indirectly. That may be useful as a preliminary implementation reference, but it must not be presented as exact exception-metadata validation.

## Test-owned shadow IDT design

The full test mechanism is deliberately confined to one pinned online vCPU in a disposable VM:

1. The probe enters from the existing `work_on_cpu` path with preemption disabled and local IRQs disabled, verifies the CPU identity, and saves the current IDTR plus the host-state snapshot already required by the boundary probe.
2. It copies the complete current IDT into test-owned memory and replaces only the `#PF` and `#GP` descriptors with test-owned 64-bit interrupt gates. All other descriptors byte-match Linux's table. Interrupt gates keep IF clear on entry.
3. The replacement handlers are small assembly stubs in the test module. They never call Linux C code before state is captured, never allocate memory, never acquire locks, and never re-enable interrupts.
4. The test loads the shadow IDT only on the selected vCPU, executes one injection, restores the original IDTR before any C-level result processing, and serializes the setup/restore path so partial setup cannot persist after a handled fault.
5. Only one injection mode is armed at a time. If the arming state does not match the caught vector/RIP, the recovery stub treats the exception as unclassified and fails the test rather than continuing toward XNU.

Other CPUs keep Linux's original IDT. This is still not a production mechanism: replacing even a per-CPU copy of the IDT is a disposable-VM test operation and must never be used by the normal x86_64 module path.

## Handler and recovery contract

In 64-bit mode the architectural exception frame includes SS, RSP, RFLAGS, CS and RIP, with the `#PF`/`#GP` error code pushed after RIP; this is also true for a same-CPL0 exception. The replacement gates use no IST, so hardware builds that frame on the current test-owned alternate stack. The raw handler entry RSP points at the error-code slot; the error code is not part of the frame consumed by `iretq`. Each handler must:

- save the general-purpose registers it needs in per-vCPU test storage before touching them;
- record the vector selected by the independent handler entry point;
- record the error code and hardware frame fields exactly as delivered;
- read CR2 for `#PF` before any instruction that can take another fault;
- restore every general-purpose register it used before `iretq`, so the recovery path starts with the fault-time register state;
- rewrite the frame exactly once: set frame RIP to the test recovery stub, set frame RSP to the test-owned recovery-stack top, keep CS/RFLAGS/SS unchanged, and account for the error-code slot before executing `iretq`; and
- increment no counter and perform no C call in the raw handler beyond the capture writes.

The stack switch is therefore explicit: `iretq` first consumes the five-slot frame left by the exception on the faulting alternate stack, then loads the rewritten frame-RSP value. Recovery begins on the test-owned recovery stack with fault-time GPRs restored. The exact recovery point must execute once; a second caught exception for the same armed case is a nested-fault failure.

Before arming, the test control block records the armed vector/mode, exact injection RIP, post-injection recovery target, original envelope RSP, original IDTR image, and recovery-stack canaries. The injection window is straight-line assembly with no push, pop, call, or other RSP-changing instruction between recording that RSP and the post-injection label, so the saved envelope RSP is the exact stack state required after the injection is skipped. After entry on the recovery stack, the recovery stub remains assembly and follows this exact sequence:

1. store any scratch GPRs it needs in a dedicated recovery save area before using them;
2. validate the arm token, vector, faulting RIP, one-shot recovery count, and recovery-stack canaries; any mismatch takes the terminal path below;
3. copy the already captured architectural record into the ordinary result buffer and disarm the injection with a single test-state write;
4. restore the original IDTR from the saved image, read it back with `sidt`, compare base and limit, and record the proof bits for exactly-one recovery and byte-identical IDTR restoration;
5. restore the scratch GPRs, load the original envelope RSP saved before arming, and jump to the post-injection label inside the same shim text.

Only then does the shim use the existing normal/software-abort envelope. Because the original envelope RSP and GPRs are restored before that jump, the shim's ordinary return path remains defined. No C result processing, logging, locking, allocation, or Linux helper runs between the recovery-stack switch and that handoff. The first C-level work after the shim returns is the existing after-snapshot/host-state comparison; the proof bits recorded in assembly are inputs to that comparison, not a substitute for it.

A fault while the shadow IDT is installed but outside the exact expected instruction is unclassified. A nested fault, mismatched vector/RIP, failed IDTR readback, corrupted recovery canary, or second recovery entry takes the same terminal path: the assembly writes a terminal code for the failure class to per-vCPU test storage and to the disposable VM's explicit QEMU `isa-debug-exit` port, then enters a `cli`/`hlt` loop. It must not `iretq` through an unknown frame, call Linux C, or attempt a best-effort restore whose integrity cannot be proven. If the I/O write has no effect because the device is absent, the loop still prevents onward execution. The VM harness must include the debug-exit device for this test and treat its terminal exit status, missing completion marker, or timeout as a failed run; a terminal failure must not be converted into a pass or retried transparently.

## Controlled injections

The first `#PF` case should use a deliberately selected unmapped canonical kernel address, preferably a fixed low address such as zero in the disposable kernel image. The test verifies that CR2 equals the selected address and that the error code is architecturally consistent with the attempted access. It must not depend on Linux keeping address zero unmapped in every configuration; the implementation probes the environment and reports a not-run configuration error rather than weakening the check.

The first `#GP` case should use a read-only invalid MSR access such as `rdmsr` from a reserved MSR index. A read is preferred over `wrmsr` because a mistaken index must not mutate machine state. The expected error code is zero, and the faulting RIP is the exact instruction in the injection window.

Both instructions must be out-of-line, individually labeled, and reachable only in the armed test mode. Compiler-generated memory accesses in the injection window are not accepted as controlled injections.

## Rollback and validation gates

The original IDTR is restored before preemption or interrupts are re-enabled. The after snapshot must match the existing boundary baseline for CPU/task identity, stack bounds, RSP, relevant RFLAGS, CR0/CR3/CR4, EFER, FS/GS/KERNEL_GS, XCR0, GDTR/IDTR, LDTR/TR, and preemption/IRQ state. In particular, IDTR base and limit after the test must equal the values captured before shadow-IDT installation.

A pass requires all of the following for both `#PF` and `#GP`:

- exact vector, RIP, error code, CPU, and recovery count metadata;
- CR2 equality and access-type consistency for `#PF`;
- exactly one recovery entry and no nested/unclassified exception;
- normal Linux syscalls after return;
- byte-for-byte restoration of IDTR state before cleanup;
- successful module unload and guest shutdown in the disposable VM.

Failure of any element blocks XNU entry work. Passing these tests is still only host-boundary evidence: it does not assign Linux's production exception ownership to XNU, validate XNU page tables, or prove that foreign kernel exceptions can be serviced safely.
