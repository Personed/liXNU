# x86_64 L4H/seL4 enabler assessment

## Status and scope

Validation-only architectural assessment. This document evaluates whether L4H/seL4 is a plausible enabler for the x86_64 hosted-XNU mediation boundary identified in `docs/x86_64-architecture-synthesis.md` and narrowed by `docs/x86_64-syscall-exception-mediation-enablers.md`.

This is not a selection, production-safety claim, viability claim, implementation design, or authorization. It authorizes no runtime, XNU execution, patch, executable ABI, backend implementation, VM/harness, boot/deploy work, CPU operation, or `#216` work.

Evidence was consulted on 2026-11-28. Upstream L4H evidence is pinned to `losfair/l4h` commit `69c0a8d24d2c0e6d830ff515b79f71c28baac9a9` (`Bump FL4 to afdfbe2757af (#271)`, dated 2026-11-27). References below use that commit. Statements are labeled as upstream facts/claims, repository facts, or inferences.

## Evidence sources

Upstream pinned evidence:

- [`DESIGN.md`](https://github.com/losfair/l4h/blob/69c0a8d24d2c0e6d830ff515b79f71c28baac9a9/DESIGN.md), especially the requirements, syscall trap, memory management, and execution-model sections.
- [`guest/provider/user.c`](https://github.com/losfair/l4h/blob/69c0a8d24d2c0e6d830ff515b79f71c28baac9a9/guest/provider/user.c), the user-context execution implementation.
- [`include/l4h/provider-registers.h`](https://github.com/losfair/l4h/blob/69c0a8d24d2c0e6d830ff515b79f71c28baac9a9/include/l4h/provider-registers.h), the native register-array ABI.

Repository evidence:

- `fki-ko/fki_l4h.c` is explicitly labeled “L4H AArch64 (EL0) backend”. It includes external headers `l4h_sys.h`, `l4h_caps.h`, `l4h_vspace.h`, and `l4h/provider-registers.h`, and is built only under `CONFIG_L4H`.
- The same file implements FKI user address spaces and user execution contexts (`fki_as_*`, `fki_utcb_*`) for AArch64 register/PAC conventions. That is repository evidence that the FKI interface has already been shaped around host-mediated user execution, not evidence that an x86_64 backend exists.
- `xnu-patches/include/linux64_abi.h` is also AArch64-specific in the current tree: it describes `blr linux64_hook`, carries `x[31]`, `pstate`, `tpidr/tpidrro`, NEON state, and PAC/JOP-key operations, and says it is copied into XNU's `osfmk/arm64` tree. Thus an x86 hosted-XNU integration needs a machine ABI redesign, not only a new low-level FKI backend.

## Foreign user tasks

Upstream facts/claims:

- L4H runs the guest Linux kernel as an ordinary seL4 user-level program, while guest user processes run natively in their own seL4 address spaces. It does not emulate guest instructions.
- L4H is x86_64-first. Its design states that a trapping thread causes every `syscall` instruction to raise `seL4_Fault_UnknownSyscall`, carrying fault IP, SP, flags, and argument registers; on x86_64 this includes all GPRs.
- Every Linux thread with a user context has a user-context TCB (`uTCB`) in its process VSpace, with `trapSyscalls` set and a badged fault endpoint. The kernel-context TCB (`kTCB`) receives syscall, VM-fault, user-exception, and interruption events.
- The x86 provider code maps seL4 user context to the native Linux register-array order, including `rip`, `rsp`, `rflags`, all GPRs, `fs_base`, and `gs_base`. It distinguishes syscall fast reply from full register restoration when `rcx`/`r11` cannot be reconstructed through `sysretq` semantics.
- FPU/XSAVE state is accessed through explicit seL4 TCB FPU read/write operations added by L4H kernel extension E2. Upstream uses this for signal frames, `sigreturn`, ptrace, core dumps, and fork.
- Linux `mm_struct` page tables are software-owned by the guest kernel; each process seL4 VSpace is a lazily filled cache of those page tables. User-access checking/copying is explicit because kernel and user memory are not both present in the same process VSpace.

Repository facts:

- `fki_l4h.c` uses the same conceptual split: `fki_utcb_run`/`resume` returns stop kinds for syscall, page fault, exception, and interrupt, while `fki_as_*` manages host-backed user address spaces.

Assessment/inference:

For foreign user tasks, L4H/seL4 is concrete evidence of a complete and architecturally coherent mediation boundary. It is stronger evidence than stock-Linux SUD/seccomp because the boundary includes syscall entry, page faults, exceptions, interruption/preemption, user register state, FPU state, and address spaces under one microkernel-mediated model.

That assessment does not mean XNU user tasks can already run or that the semantics match Mach/BSD/Darwin requirements. It means the user-task half of the boundary has a plausible existing architectural mechanism.

## XNU/Linux kernel-mode mediation

Upstream facts/claims:

- L4H does not make a stock Linux kernel safely mediate a foreign kernel in ring 0. Instead, the guest Linux kernel itself runs in ring 3 as a paravirtualized `ARCH=l4h` kernel.
- The privilege boundary is seL4 underneath the Linux guest. User syscalls and exceptions reach the guest kernel as seL4 fault IPC because seL4 owns the machine boundary.
- L4H uses an seL4 kernel with MCS/SMP plus explicit extensions, including the syscall-trap flag and TCB FPU access. It is not a vanilla unmodified seL4 boundary.

Repository facts:

- The repository's `fki_l4h.c` is a Linux kernel module backend compiled only when the surrounding Linux kernel is an L4H guest (`CONFIG_L4H`). It calls L4H/seL4 provider operations rather than relying on stock Linux ring-0 ownership.
- The x86_64 native FKI backend currently remains fail-closed; no x86 L4H FKI backend exists in this repository.

Assessment/inference:

L4H/seL4 answers the architectural ownership part of the kernel-mode gap only by changing who owns the host/privilege boundary. The blocker documented for stock Linux was that syscall MSRs, IDT, KPTI/CR3/GS, and exception entry architecturally enter Linux first. Under L4H, Linux is not the machine-owner in that sense: seL4 mediates user execution and faults before the paravirtualized Linux guest handles them.

That is not yet evidence that faults or control-flow inside an XNU kernel hosted as a module in the same Linux guest are separately identified and mediated. Distinguishing guest-user faults, Linux-kernel behavior, and XNU-kernel execution requires an explicit XNU/LINUX64/FKI execution-boundary design; L4H supplies the host primitive and architecture, not that XNU-specific mediation contract.

Therefore L4H/seL4 is evidence for a type-1 microkernel/paravirtualized architecture, not evidence for the stock-kernel module path. It does not refute the prior stock-kernel synthesis; it reframes the host architecture.

## Boot, deployment, and host architecture

Upstream facts/claims:

- L4H is a type-1 hypervisor built on seL4 and is packaged around bootable system images. Its design claims x86_64 deployment coverage including QEMU, cloud instances, and bare metal, with no requirement for VT-x or SVM for ordinary Linux guests.
- Hardware virtualization support is a separate optional concern in the upstream design; ordinary L4H guest mediation is paravirtual/microkernel-mediated rather than KVM-based.
- The architecture includes dom0 management, guest-owned memory, SMP vCPUs, scheduling contexts, device pass-through or virtio-style devices, and platform-specific boot machinery.

Assessment/inference:

This is neither a stock Linux kernel module nor the repository's supplemental QEMU/KVM macOS guest. It is a different host architecture: users would boot an L4H/seL4 system and run Linux as a paravirtualized guest, with xnulinux modules inside that guest.

Whether that deployment model satisfies the project goal “native x86_64 Linux PCs” is a major architectural/product decision for the owner. This document does not decide it.

## Open obligations before any x86 backend/design

| Area | Required evidence or design work |
| --- | --- |
| x86 registers | Define the FKI x86 user register layout, including GPRs, `rip/rsp/rflags`, `fs_base/gs_base`, syscall restart semantics, and signal/ptrace interactions. |
| XSAVE/FPU | Define the x86 XSAVE feature set, state sizing, alignment, supervisor/user state split, and how TCB FPU read/write maps to XNU thread state. |
| Syscall/stop taxonomy | Map Darwin BSD/Mach/machine-dependent syscall encodings and XNU exception semantics onto L4H stop kinds without confusing them with Linux syscall numbers. |
| Address spaces | Map XNU pmap/VM objects onto host-managed page-table caches, including eviction, shared/nested pages, execute-only or x86-equivalent protections, wired memory, copyin/copyout, and TLB invalidation. |
| User-memory semantics | Define exact copy semantics, atomicity, faulting, races, and protection checks for XNU kernel access to user memory under separate kernel/user VSpaces. |
| Timers/IPI/SMP | Map XNU timer, preemption, IPI, CPU start/stop, idle, and scheduling expectations onto L4H vCPUs, notifications, deadlines, and scheduling contexts. |
| Interrupts/exceptions | Define host event delivery, interrupt latency, exception classification, machine-check/NMI limits, and rollback behavior. |
| Kernel integration | Design an x86 `fki_l4h` backend and the XNU machine-layer interface; none exists now. |
| Rollback/cleanup | Define exactly-once cleanup for user contexts, address spaces, memory frames, TCBs, endpoints, timers, and XNU shutdown/panic paths. |
| Boot/deploy | Define how users install/boot L4H/seL4, what hardware/firmware is required, how Linux guests and xnulinux modules are provisioned, and how upgrades/rollback work. |
| XNU patch integration | Replace or parallelize the current AArch64 LINUX64 machine ABI (`x[31]`, `pstate`, TPIDR/NEON/PAC concepts) with an x86-specific contract, then determine which EL0 host concepts transfer. |
| Hardware acceptance | Validate Intel and AMD separately on physical hosts; upstream QEMU/cloud claims are not xnulinux acceptance evidence. |
| upstream maintenance/security | Track L4H/seL4 extensions, upstream maintenance, security response, reproducible builds, version pinning, and supply-chain review. |
| Goal compatibility | Decide whether type-1 seL4/L4H deployment is within the project definition of “Linux PCs”; this is explicitly unresolved owner-level scope. |

## Conclusion

L4H/seL4 is concrete evidence of a plausible type-1 microkernel/paravirtualized mediation architecture for foreign user tasks and for relocating the architectural machine-ownership boundary away from stock Linux. It is not yet evidence that XNU kernel-mode faults inside the Linux guest are separately identified and mediated; that remains an XNU/LINUX64/FKI design obligation. It is also not evidence for a stock-kernel module path, because it relocates the machine/privilege boundary from stock Linux to seL4 and runs Linux paravirtualized under it.

The repository already contains an AArch64-specific `CONFIG_L4H` FKI backend shaped around this model, but no x86 backend exists. Any x86 backend, ABI, XNU patch integration, runtime, boot/deploy support, or hardware acceptance claim requires later explicit design and implementation authorization.

This is a partial or major architectural reframing, not a small implementation choice. Whether the project accepts type-1 seL4/L4H as a legitimate interpretation of the x86_64 “Linux PCs” goal remains an owner decision after this assessment.
