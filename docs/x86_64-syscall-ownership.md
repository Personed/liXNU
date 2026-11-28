# x86_64 syscall ownership/routing model

This is a validation-only design oracle for a future hosted-x86_64 syscall boundary. It is not a syscall implementation, XNU userland execution, runtime enablement, or production routing mechanism. The future XNU syscall instruction/vector encoding and ABI remain unspecified contract work; a Linux syscall number or mechanism is never a synonym for an XNU syscall.

The model in `fki-ko/fki_x86_64_exception_ownership.h`'s sibling `fki_x86_64_syscall_ownership.h` is per CPU and uses abstract cookies for the Linux host task and foreign task. Those cookies must remain distinct and unchanged across the handoff. CPU or task movement/mismatch is a consistency failure.

Linux-native tasks are never intercepted. A native Linux syscall event is pass-through evidence only and does not consume or alter a foreign handoff. The model does not presume any global MSR, syscall-table, IDT, or Linux syscall-path replacement. Any future real mechanism requires a separately reviewed production-safe protocol.

A registered foreign syscall creates one handoff. The handoff must be accepted exactly once and completed exactly once either by reply or rejection. A duplicate accept/reply/rejection is invalid. A nested foreign syscall while a handoff is pending, or a syscall event with unknown ownership, is terminal abort. Rejection is an exactly-once routing outcome, not a Linux syscall emulation and not an XNU syscall ABI decision.

The model is separate from both the FKI transaction machine and the synchronous-exception ownership model. It does not add events to, alter, duplicate, or integrate with either one. A future production design may compose the three contracts, but this slice performs no real routing and establishes no runtime ownership.
