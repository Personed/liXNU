<!-- SPDX-License-Identifier: GPL-2.0-only -->
# Physical x86_64 hosted XNU acceptance record

Use one record per physical PC, Linux profile, and run of the native Tahoe 26.7.3 hosted XNU path.
Use the test artifacts and prerequisites from the [native port design](x86_64-native-port-design.md).
Capture the same fields for Intel and AMD in separate records. This form also records incomplete runs.

## Identify the host and inputs

Record the state of the machine before starting XNU. The doctor report can supply Linux facts:

```sh
python3 scripts/xnulinux-doctor.py --json > doctor.json
```

Inspect the JSON even if the command exits with status 1 because QEMU is absent. Its readiness checks
describe the complementary TCG route. Fill in any missing fields in the record below. A CPU vendor or
model string alone cannot establish that Linux is running directly on the physical system.

| Field | Recorded value |
| --- | --- |
| Run identity | UTC start/end time, evidence link, and how the operator confirmed Linux booted directly on the PC |
| Machine | CPU vendor, model, family/model/stepping, relevant CPU flags, microcode, and logical CPUs available and used |
| Linux | Distribution and version, kernel release and package or build identity, base page size, Linux `.config` SHA-256 and source, and relevant boot settings |
| Memory | Installed RAM, Linux-visible total and available memory immediately before entry, and the allocation reserved for XNU, in bytes |
| Build and installation | liXNU commit, build commands and toolchain versions, module install method, module SHA-256 values, and build logs or manifest |
| XNU and boot inputs | Exact Tahoe build and XNU revision, collection and disk SHA-256 values before boot, and the disk preparation method |

Identify a kernel config exported by `/proc/config.gz` as coming from the running kernel. Identify a
matching `/boot/config-<release>` as a file associated with the release, since two kernels can share a
release string. Use `unavailable` for an unreadable or missing config. Retain reviewed logs, test output,
and hashes. Keep system images, firmware, credentials, serial numbers, and private disk contents out of
the repository and shared evidence.

## Record the furthest verified stage

Use `pass`, `fail`, or `not run` for each stage. For each pass or failure, link the corresponding log
lines or other observation. Record the failed prerequisite or error for stages that cannot run.
For host integrity, use the checks in the [entry and return contract](x86_64-entry-return-contract.md)
with evidence from the native XNU session being recorded.

| Stage | Evidence to retain | Status and link |
| --- | --- | --- |
| Reproduce build and install | The stated commands produce and install the architecture-matched test artifacts on the recorded Linux profile. Record any digest difference on a repeat build. | |
| Enter hosted XNU | The native entry trace names the loaded build and records execution past the XNU entry point. | |
| Return and fault recovery | The approved checks record normal return and the outcome of each exercised recovery path, with before/after Linux state comparisons. | |
| Linux host integrity | The host checks report CPU/task, register, extended state, address-space, and interrupt state preservation as applicable. Record Linux syscall and memory checks during hosted operation and after return. | |
| Tahoe desktop and devices | The desktop renders visibly, responds to keyboard and pointer input, and passes both disk I/O and network checks. Record the rendering backend and the outcome of each check. | |
| Guest shutdown and cleanup | Tahoe requests shutdown. Linux logs the XNU stop and module cleanup and stays responsive afterward. Record a host-initiated stop as cleanup. | |
| Unsupported CPU or config refusal | Record the deliberately unsupported profile in a separate run and retain the refusal before XNU entry, plus the subsequent Linux health check. | |

Use a passing desktop record only when the build/install, entry, return/recovery, host-integrity, desktop,
and guest-shutdown stages all have direct evidence for that profile. Link the separate refusal run where
applicable. Attach the record to the relevant PR and summarize it in
[Portability validation](portability-validation.md#native-x86_64-hosted-xnu-work-status).
Record observed resource values for that run. For lower resource settings, record repeated passes and
failures on the same profile and report the tested boundary.
