# x86_64 CPU-quiescence transfer-or-rejection decision record

## Status and scope

This is a validation-only decision record for a possible future native x86_64 XNU entry. The helper in `fki-ko/fki_x86_64_cpu_quiescence_decision.h` exposes the same decisions to userspace fixtures. It is not a runtime mechanism, CPU-hotplug implementation, ownership protocol, state machine, or authorization to execute foreign code.

No route in this record is accepted. A route may be a potential prerequisite and still be insufficient for production. The record does not presume that CR3 or other control state can be transferred on a live Linux CPU, and it does not alter or integrate with the ownership matrix in `docs/x86_64-control-memory-ownership.md`.

## Classification meanings

- **forbidden for production**: the route cannot be treated as a production-safe control-state transfer mechanism.
- **unresolved/requires stronger protocol**: the evidence is insufficient; a future reviewed protocol must decide the exact behavior before any experiment.
- **potentially necessary but insufficient**: the mechanism may be required as one prerequisite in a future protocol, but it is not sufficient and must not be treated as ownership transfer.

NMI, external IRQ, and machine-check ownership remain unsupported invariants for every route. Disabling ordinary local interrupts or migrating Linux IRQ delivery does not establish ownership of asynchronous exceptions or physical CPU behavior.

## Evidence constraints

- Linux `stop_machine()` documentation for v6.8 says it schedules a thread on every CPU, each disabling interrupts, so no CPU is holding a spinlock or in another preempt-disabled region while the callback runs; it is a temporary heavyweight lock-like operation and protects against CPU hotplug. It does not say that NMI or machine-check delivery is excluded, and it does not transfer CPU, memory, or control-register ownership. Reference: [`include/linux/stop_machine.h` lines 99-113](https://github.com/torvalds/linux/blob/v6.8/include/linux/stop_machine.h#L99-L113).
- Linux v6.8 CPU-hotplug documentation says `cpu_online_mask` is cleared when a CPU is brought down and that OS services including interrupts are migrated away. The offline case invokes teardown callbacks, migrates processes, interrupts, and timers, and then invokes architecture cleanup. References: [`cpu_online_mask` lines 64-69](https://github.com/torvalds/linux/blob/v6.8/Documentation/core-api/cpu_hotplug.rst#L64-L69) and [offline case lines 133-145](https://github.com/torvalds/linux/blob/v6.8/Documentation/core-api/cpu_hotplug.rst#L133-L145).
- In Linux v6.8, `take_cpu_down()` explicitly calls `__cpu_disable()` so the outgoing CPU does not handle more interrupts, then runs dying callbacks, hands over timekeeping, removes timer broadcasting, and parks the stopper thread. Reference: [`kernel/cpu.c` lines 1304-1333](https://github.com/torvalds/linux/blob/v6.8/kernel/cpu.c#L1304-L1333).
- Neither source above authorizes foreign execution on the physical CPU, transfers ownership of reset/wake/re-online control, or prevents platform firmware/hardware asynchronous events. Logical offline is a Linux scheduler/IRQ state, not ownership of the physical CPU.

## Decision matrix

| Route or component | Decision | Conservative rationale |
| --- | --- | --- |
| Control-state transfer on a live Linux CPU | forbidden for production | Linux retains ownership of scheduling, exceptions, interrupts, MSRs, control registers, and memory. No documented mechanism makes CR3/control-state transfer safe while the CPU remains a live Linux CPU. NMI/IRQ/machine-check remain unsupported. |
| `stop_machine` rendezvous | potentially necessary but insufficient | It can temporarily stop ordinary Linux execution with interrupts disabled and may be useful in a future protocol, but it is temporary, does not exclude NMI/machine-check, does not transfer CPU/control/memory ownership, and does not authorize foreign execution. |
| CPU logical-offline/hotplug prerequisite | potentially necessary but insufficient | Logical offline can remove scheduling and Linux IRQ delivery from a CPU, making it a possible prerequisite. It still does not authorize foreign execution, own the physical CPU, control later wake/reset/re-online, or resolve control-state/memory ownership and rollback. |
| Rollback and re-online path | unresolved/requires stronger protocol | Exact reverse-order rollback, re-online ownership, wake/reset control, failure behavior, and exactly-once cleanup are undecided. The route cannot be inferred from hotplug availability. |

## Necessary versus sufficient

Logical CPU offline and a `stop_machine`-style rendezvous may be necessary in some future design, but neither is sufficient. A production-safe design would still need, at minimum:

1. explicit memory and control-state ownership or rejection;
2. CPU identity and concurrency control;
3. blocking or safely rejecting wake, reset, and re-online while foreign state exists;
4. rollback of pre-memory_guard side effects in reverse order;
5. a policy for unsupported NMI, IRQ, and machine-check events;
6. preservation and restoration of Linux state with exactly-once cleanup.

Those requirements remain unresolved. This record does not choose protected-mode reproduction, patched long-mode handoff, direct long-mode handoff, or rejection of all future entry paths. It only rejects treating any single Linux primitive above as sufficient.

## Relation to the ownership matrix

The matrix in `docs/x86_64-control-memory-ownership.md` classifies resources and invariants. This decision record evaluates candidate CPU-quiescence routes. The two documents are deliberately separate: this record does not change the matrix classifications, duplicate the transaction/exception/syscall models, or add events, transitions, or an executor.

## Explicit non-decisions and limits

- No live-CPU CR3/control-state transfer is presumed safe.
- No foreign execution, XNU execution, loader, patch, entry, real MSR/IDT access, userland, runtime, or ownership mechanism is authorized.
- No CPU hotplug operation is performed by this validation artifact.
- No VM harness or ABI/layout v1 is changed.
- `#216` remains untouched and frozen.
