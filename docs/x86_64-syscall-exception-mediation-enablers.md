# x86_64 syscall/exception mediation enabler assessment

## Status

Validation-only and test-only assessment. This document evaluates whether existing stock-kernel Linux mechanisms provide any part of the mediation boundary required for a future x86_64 deep-paravirtualization architecture. It does not authorize or implement XNU execution, a kernel module, a patch, an ABI, entry, runtime, ownership, MSR/IDT access, CPU/hotplug operation, transaction behavior, a VM harness change, or `#216` work.

This assessment supplements `docs/x86_64-architecture-synthesis.md`. It does not reopen or duplicate the proof-obligation tables in #240-#246.

## Boundary separation

There are two distinct boundaries:

1. **Foreign user tasks**: user threads/processes executing Darwin/XNU-like code under Linux-visible user mode. Linux can observe or intercept their syscall instructions before the Linux syscall implementation consumes them.
2. **XNU kernel-mode execution**: foreign kernel code running in kernel context. Its exceptions, interrupts, entry state, control registers, GS/CR3 assumptions, LAPIC interactions, and rollback are not covered by user-mode syscall interception.

These boundaries must not be conflated. Evidence for the first does not resolve the second.

## Tested stock-kernel mechanisms

`tests/x86_64/test-x86-mediation-enablers.c` exercises only safe synthetic userspace probes. It does not execute a real Darwin syscall service or XNU code. The probe uses `0x02000004`, the Darwin-style BSD write encoding formed by `SYSCALL_CLASS_UNIX << 24 | 4`, because it is safely outside the native Linux x86_64 syscall namespace used by the probe.

Observed local result:

```text
SUD_PASS nr=0x2000004 direct_pid=<pid>
SECCOMP_USER_NOTIF_PASS nr=0x2000004 supervised_result=0x516d6564
X86_MEDIATION_ENABLERS_PASS
```

The test establishes two narrow facts:

- Syscall User Dispatch allowed a syscall issued from the configured direct-dispatch region while the selector blocked other regions, then intercepted the synthetic foreign-encoded syscall from another executable region and reported `SYS_USER_DISPATCH` with the expected syscall number and call-site address.
- seccomp user notification intercepted the same synthetic foreign-encoded syscall number, delivered it to a userspace supervisor, and completed it with a synthetic supervisor-selected return value.

These are functional userspace mediation results only. They are not evidence of production-safe performance, security, compatibility, memory semantics, exception semantics, or a complete XNU userspace runtime.

## Syscall User Dispatch evidence and limits

Linux v6.8 documents Syscall User Dispatch as a mechanism for compatibility layers such as Wine. It filters by syscall dispatcher address and a userspace selector rather than by trusting the foreign syscall ABI. That makes it relevant to a future foreign-user-task runtime.

Documented limits remain decisive:

- It is explicitly not a security boundary. Foreign code can attempt to jump to an allowed dispatcher region or manipulate the selector unless a separate mechanism constrains it.
- fork and exec reset the mechanism to off.
- vDSO and signal trampoline handling have special rules; a production design must account for them exactly.
- Signal delivery has a cost and the signal handler/supervisor must correctly rewrite or emulate the user context. The probe intentionally validates interception semantics only; it does not implement a syscall service.
- The selector/allowed-region fast path is a compatibility-layer optimization, not proof that arbitrary Darwin userland is mediated safely.

Therefore SUD is at most a **partial enabler** for foreign user tasks. It cannot by itself define a production-safe boundary.

## seccomp user-notification evidence and limits

Linux v6.8 seccomp user notification is designed to let a userspace supervisor handle selected syscalls. The probe confirms that a stock kernel can route a synthetic foreign-encoded syscall number to a supervisor and return a supervised result.

Important limits remain:

- A supervisor process/thread is mandatory; this is not transparent kernel-level routing to XNU.
- The notification carries register arguments, not pointed-to memory contents. A design needing memory arguments must define safe access through `/proc/pid/mem`, ptrace-like mechanisms, shared memory, or another explicit protocol.
- Linux documentation requires care around time-of-check/time-of-use behavior: memory arguments must be copied before policy decisions if they affect security or correctness.
- Preemption, cancellation, killable waits, notification IDs, and supervisor failure must be designed explicitly.
- seccomp filtering happens in Linux syscall entry and cannot distinguish a valid Darwin ABI from malicious code except through the policy and process architecture built around it.

seccomp user notification is also a **partial enabler** for foreign user tasks. It can provide a correctness-oriented mediation path, potentially complemented by SUD or other dispatch mechanisms, but it does not itself establish production safety or performance.

## User-task boundary answer

Under current evidence, **a minimum stock-kernel functional boundary exists for mediating foreign userspace tasks**. SUD and seccomp user notification demonstrate two upstream mechanisms capable of intercepting foreign-encoded syscall instructions and involving userspace mediation logic.

This answer is intentionally narrow. It means only that deep paravirtualization research does not need kernel cooperation merely to prove that a foreign user task's syscall instruction can be intercepted. It does not mean that the complete user-task boundary is production-safe. Signal semantics, memory access, process lifecycle, security containment, performance, Mach/BSD syscall emulation, exception delivery, thread state, and integration with an XNU kernel all remain required-but-undesigned.

## XNU kernel-mode boundary answer

Under current evidence, **there is no demonstrated minimum stock-kernel boundary for XNU kernel-mode mediation**. Neither SUD nor seccomp user notification mediates kernel-mode execution. If XNU kernel code executes in kernel context:

- synchronous exceptions enter Linux's IDT/page-fault/general-protection path first;
- syscall MSRs still belong to the architectural Linux entry path unless user syscalls are intercepted before entering that path;
- KPTI/CR3/GS/entry-stack/TSS/IST state must already be valid for Linux before any module-level forwarding logic can run;
- LAPIC, interrupt, preemption, rollback, cleanup, and exactly-once behavior remain unresolved;
- kprobes, die notifiers, and tracepoints are observation/debug mechanisms, not demonstrated production-safe ownership or routing boundaries.

The architecture synthesis therefore remains correct for kernel-mode mediation: patching XNU alone cannot create the missing interposition. The new userspace evidence narrows, but does not overturn, that assessment.

## Architectural implications

A future deep-paravirtualization design could treat foreign user tasks as Linux-visible user processes/threads and use stock-kernel userspace mediation for part of their syscall boundary. That would be a much deeper architecture than a simple XNU patch: user memory, thread state, process lifecycle, Mach/BSD syscall service, exception delivery, and host communication would all need explicit design.

Even with that userspace boundary, the decisive kernel-mode question remains unresolved. If XNU kernel-mode execution cannot be constrained so Linux entry state remains valid and foreign kernel faults/services are mediated without unstable hooks, then deep paravirtualization alone is insufficient and the research choice moves to kernel cooperation or a hypervisor/virtualization architecture.

This document selects none of those architectures. It records only that the stock-kernel userspace boundary is partially enabled, while the stock-kernel kernel-mode boundary remains unproven.

## Validation

Local probe command:

```sh
bash tests/x86_64/test-x86-mediation-enablers.sh
```

Result: pass. No kernel module was built or loaded, no XNU code ran, and no foreign kernel state was touched.
