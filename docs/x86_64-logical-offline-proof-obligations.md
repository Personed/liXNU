# x86_64 logical-offline proof obligations and evidence

## Status and scope

This is a validation-only table of proof obligations and Linux evidence for a possible logical-offline route. It is **not** a protocol candidate, an approved protocol, a state machine, an executor, or permission to execute foreign code. The helper in `fki-ko/fki_x86_64_logical_offline_obligations.h` exposes the same table to userspace fixtures.

No route is accepted. The documented conclusion is conservative: logical CPU offline plus hotplug blocking is insufficient for production-safe ownership unless new evidence and a complete reviewed protocol prove otherwise. This document does not alter or integrate with the transaction, exception, syscall, or control/memory ownership models; it only references them as separate unresolved design inputs.

## Classification meanings

- **evidence-only primitive**: an exported or documented primitive exists, but the evidence only describes a Linux mechanism and does not imply ownership transfer.
- **insufficient alone**: the primitive or combination may be useful evidence for a future design, but cannot satisfy production-safe ownership by itself.
- **forbidden as ownership claim**: using the primitive or state as a claim of physical CPU, memory, or control-state ownership is forbidden.
- **unresolved/requires stronger protocol**: the obligation is explicitly unresolved and must be answered by a later reviewed design.
- **required but undesigned**: the obligation is necessary for any complete design, but no design exists here.

## Evidence constraints

The following evidence is deliberately narrow. Export observations were checked on the local `/lib/modules/6.8.0-1060-aws/build/Module.symvers`; they are evidence for that kernel build, not a universal promise for every host kernel.

- `stop_machine()` is documented as a temporary heavyweight operation that schedules a thread on every CPU with interrupts disabled. It does not transfer CPU, memory, or control-state ownership and does not establish NMI or machine-check ownership. Reference: [`include/linux/stop_machine.h` lines 99-113](https://github.com/torvalds/linux/blob/v6.8/include/linux/stop_machine.h#L99-L113). The local kernel exports it as GPL-only.
- Linux v6.8 documents that `cpu_online_mask` is cleared by `__cpu_disable()` after services including interrupts are migrated away, and the offline case migrates processes, interrupts, and timers before architecture cleanup. References: [`cpu_online_mask` lines 64-69](https://github.com/torvalds/linux/blob/v6.8/Documentation/core-api/cpu_hotplug.rst#L64-L69) and [offline case lines 133-145](https://github.com/torvalds/linux/blob/v6.8/Documentation/core-api/cpu_hotplug.rst#L133-L145).
- `cpu_device_down()` is documented for the CPU device core only; other subsystems are directed to `remove_cpu()`. `remove_cpu()` serializes `device_offline()` through the device hotplug lock and is exported GPL in v6.8. Reference: [`kernel/cpu.c` lines 1553-1578](https://github.com/torvalds/linux/blob/v6.8/kernel/cpu.c#L1553-L1578).
- `add_cpu()` serializes `device_online()` through the device hotplug lock and is exported GPL in v6.8. Reference: [`kernel/cpu.c` lines 1785-1795](https://github.com/torvalds/linux/blob/v6.8/kernel/cpu.c#L1785-L1795).
- `cpu_hotplug_disable()` waits for current hotplug operations and disables future `cpu_up`/`cpu_down` operations, but it is a global nested kernel-wide mechanism rather than ownership of one physical CPU. Reference: [`kernel/cpu.c` lines 546-574](https://github.com/torvalds/linux/blob/v6.8/kernel/cpu.c#L546-L574).
- `cpus_read_lock()` and cpuhp state setup/removal have exported forms in v6.8, but they are coordination/notification primitives, not foreign-execution ownership mechanisms. References: [`cpus_read_lock` export lines 487-503](https://github.com/torvalds/linux/blob/v6.8/kernel/cpu.c#L487-L503) and [cpuhp export lines 2558-2668](https://github.com/torvalds/linux/blob/v6.8/kernel/cpu.c#L2558-L2668).
- No exported `native_cpu_up`, `start_cpu`, `wakeup_secondary_cpu`, or `do_boot_cpu` symbol was observed in the local kernel `Module.symvers`. This absence is evidence against assuming a module-accessible wake/reset mechanism; it is not proof that no future protocol could exist.

## Primitive evidence table

| Primitive or misuse | Classification | Conservative meaning |
| --- | --- | --- |
| `stop_machine` primitive | insufficient alone | Exported temporary rendezvous evidence; no ownership transfer and no NMI/machine-check ownership. |
| `remove_cpu` primitive | insufficient alone | Exported logical-offline mechanism; can migrate Linux services away but does not possess the physical CPU. |
| `add_cpu` primitive | insufficient alone | Exported logical re-online mechanism; does not define rollback ownership or authorize foreign state beforehand. |
| `cpu_hotplug_disable/enable` primitive | insufficient alone | Exported global hotplug blocker; can prevent later Linux hotplug operations but has global side effects and no per-CPU physical ownership semantics. |
| cpuhp callbacks | evidence-only primitive | Coordination/notification hooks only; they do not transfer ownership. |
| `cpus_read_lock` | evidence-only primitive | Hotplug reader serialization only; temporary and not ownership. |
| Claiming `stop_machine` is ownership transfer | forbidden as ownership claim | Temporary stopping is not transfer. |
| Claiming logical offline is physical CPU ownership | forbidden as ownership claim | Linux logical state is not possession of the physical CPU or control of reset/wake. |
| Claiming hotplug blocking is ownership of a CPU | forbidden as ownership claim | Blocking future Linux hotplug does not isolate hardware or prove rollback. |
| Combined logical-offline plus hotplug blocking | insufficient alone | Even together these primitives do not satisfy memory/control ownership, physical possession, wake/reset control, asynchronous-event policy, reverse rollback, or safe re-online. |

## Required proof obligations

| Obligation | Classification | Required treatment |
| --- | --- | --- |
| CPU selection and physical possession | required but undesigned | A future design must define which CPU, how possession is established, and how Linux/hardware ownership conflicts are prevented. |
| Wake/reset control | unresolved/requires stronger protocol | No module-accessible ownership mechanism is established here. |
| Re-online and rollback control | unresolved/requires stronger protocol | `add_cpu()` alone does not prove reverse-order rollback or ownership during failure. |
| Memory ownership | unresolved/requires stronger protocol | The separate ownership matrix remains unresolved for production composition. |
| CR3/control-state transfer | unresolved/requires stronger protocol | This table neither permits nor decides transfer on any CPU. |
| Reverse-order rollback | required but undesigned | Pre-entry and post-entry side effects must have explicit exactly-once cleanup before any future experiment. |
| NMI invariant | unresolved/requires stronger protocol | Unsupported; no recovery or routing claim. |
| External IRQ invariant | unresolved/requires stronger protocol | Linux IRQ migration is not foreign interrupt ownership. |
| Machine-check invariant | unresolved/requires stronger protocol | Unsupported; no recovery claim. |
| XNU entry | unresolved/requires stronger protocol | No entry path is authorized by this document. |
| Exactly-once executor | unresolved/requires stronger protocol | A future design requirement only; no events, transitions, or executor exist here. |

## Explicit non-decisions and limits

- No protected-mode reproduction, patched long-mode handoff, direct long-mode handoff, or all-entry rejection is selected.
- `XNU_PREBOOT_LONG` remains rejected and unmodeled in ABI v1.
- `_start+0x5c` remains only a structural anchor, not an authorized entry point.
- No live-CPU CR3/control-state transfer is presumed safe.
- No real CPU hotplug call, wake/reset operation, ownership transfer, runtime, XNU execution, loader, patch, entry, MSR/IDT access, userland, transaction change, ABI/layout change, VM harness change, or `#216` work is authorized.

The only supported conclusion is that the inventoried Linux primitives are insufficient evidence for a production-safe ownership claim. A complete reviewed design must resolve the proof obligations above before any executable experiment can even be proposed.
