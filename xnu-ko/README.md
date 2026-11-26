# XNU module architecture targets

| Target | Build contents | Runtime status |
| --- | --- | --- |
| ARM64 | `xnu_main.c`, `world.S`, Mach-O loader, and ADT builder | Existing hosted-XNU path; keep its behavior covered by the ARM64 module build and runtime checks. |
| x86_64 | `xnu_x86_64_stub.c` only | Build target only. Module initialization returns `-EOPNOTSUPP` until an x86_64 world switch, entry/return path, and fault/context handling are implemented and verified. |

The x86_64 CI job proves that the architecture-specific module target packages against x86_64 Linux headers. It does not prove that XNU can execute on x86_64. The stub deliberately fails module initialization; do not change it to report success before the native execution boundary is ready.
