# XNU module architecture targets

| Target | Build contents | Runtime status |
| --- | --- | --- |
| ARM64 | `xnu_main.c`, `world.S`, Mach-O loader, and ADT builder | Existing hosted-XNU path; keep its behavior covered by the ARM64 module build and runtime checks. |
| x86_64 | `xnu_x86_64_stub.c` only | Build target only. Module initialization rejects CPUs missing SSE4.2, AVX, FMA, MOVBE, POPCNT, LZCNT, BMI1, AVX2, or BMI2 with a message listing the missing features, then returns `-EOPNOTSUPP` until an x86_64 world switch, entry/return path, and fault/context handling are implemented and verified. |

The x86_64 CI job builds the module and exercises its initializer inside disposable x86_64 Linux VMs. One feature-complete KVM run asserts that initialization reaches the world-switch refusal; one feature-limited TCG run asserts that missing CPU features are listed before that refusal. Both assert that `insmod` reports `Operation not supported`, `/sys/module/xnu` remains absent, and the proprietary-module taint bit is set. This does not prove that XNU can execute on x86_64. The stub deliberately fails module initialization; do not change it to report success before the native execution boundary is ready.

Loading a `Proprietary` module taints the kernel before the module initializer runs. Returning `-EOPNOTSUPP` does not clear that taint, so the refusal is not side-effect-free. Do not attempt to load this module on a shared host; use a disposable VM for the rejection check.
