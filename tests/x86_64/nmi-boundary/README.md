# Linux x86_64 NMI boundary probe

This test loads a test-only module into a disposable one-vCPU Linux VM under QEMU TCG. The module registers one
`NMI_LOCAL` callback through Linux's `register_nmi_handler()` API. A QMP controller sends exactly one `inject-nmi`
after the assembly shim emits its ready byte and enters the fixed RIP probe window. The instruction range is bounded,
but the in-guest spin has no deadline; it waits for the callback to release it. The host controller terminates QEMU if
the ready handshake or guest completion exceeds its 300-second timeout.
The module refuses to run unless Linux reports a hypervisor CPU feature and refuses a FRED entry mode; the checked-in
runner selects QEMU TCG explicitly.

The kernel headers must provide the x86 `register_nmi_handler()` API and export both `__register_nmi_handler` and
`unregister_nmi_handler` in `Module.symvers`. The runner checks these requirements before Kbuild and fails with a
specific message when they are missing. For example, the Ubuntu 6.8.0-1060 headers lack the required NMI handler exports;
the Ubuntu 7.0.0-44 headers selected by the current CI package glob provide them. The FRED runtime check is compiled
only when the headers define `X86_FEATURE_FRED`; older kernels without FRED use the legacy IDT entry path.

The test leaves Linux's IDT installed. It compares the IDTR base and limit before, inside, and after the callback; checks
the callback's Linux NMI type, CPU, RIP window, saved GPRs, RSP and RFLAGS; checks the shim's post-`iretq` GPRs, RSP and
RFLAGS, task identity and CR3; executes post-return syscalls; unregisters the handler through Linux and checks normal module unload and guest
power-off. The callback does not log, allocate, sleep, or take locks. `unregister_nmi_handler()` provides the synchronized
cleanup before the test result is accepted.

Linux's NMI callback API provides the NMI type and `pt_regs`, but not a vector-number field. The architectural vector 2
is inferred from QMP's real NMI injection and Linux's NMI entry path; the test does not claim to read vector 2 from the
callback. It records no custom IDT entry and does not replace Linux's IDT.

This is Linux `HOST_BOUND_SHIM` evidence in a virtual machine only. It does not deliver an NMI to XNU, test exception
ownership while foreign XNU code runs, or establish NMI behavior on physical Intel/AMD hardware.
