## Purpose

<!-- State the user outcome and the architecture path this changes. -->

## Related work and coordination

- Related PRs/issues:
- Related PR conversation and review threads read by each participating agent:
- Coordination and result comments posted for other agents, with links:

## Validation

- Commands and results:
- Hardware/artifact acceptance still missing:
- Support claims changed (if any):

## Checklist

- [ ] ARM64 behavior remains covered when shared module code changes.
- [ ] No Apple system images, firmware, or local SMC OSK values are included.
- [ ] TCG evidence is labeled as development-only.
- [ ] The active x86_64 → QEMU → ARM64 Linux → PureDarwin path is distinct from separate experiments.
- [ ] Every related agent has visible scope, interface, and result updates in PR comments.
- [ ] No merge or auto-merge is performed without explicit user authorization; green CI alone is not permission.
