# Owned physical memory in the x86 idle page tables

The experimental patch in
[`scripts/x86_64/experimental-owned-pmap.patch`](../scripts/x86_64/experimental-owned-pmap.patch)
targets public XNU commit `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`. Its
guard is `XNU_LINUX_HOSTED_PMAP`. Any future build that defines it must use
the non-KASAN bootstrap and a loader that supplies version 2 revision 0
`boot_args`. The existing build script leaves the patch unapplied.

The loader supplies the bounds of one Linux-owned physical reservation in
`bootMemStart` and `bootMemSize`. The entire range consists of 2 MiB
leaves below 4 GiB. The loader must hold it until final XNU shutdown and
place the image, boot arguments, EFI map and ADT before the first
bootstrap allocation. It must validate the EFI allocator boundary and the
initial `BootPTD` separately. The patch checks these addresses at the
start of `vstart()`, before the page table allocator runs.

`ALLOCPAGES()` refuses writes outside the reservation or allocations
that would leave the next free pointer at 4 GiB. The initial kernel alias
starts at the reservation's physical base and stops at the end of the
fixed page table allocations. The physmap leaves all 2 MiB entries outside
the reservation absent while preserving physical offsets. The `__HIB`
double map must fit inside the same reservation and its L2 table.

The remaining integration gate is to build this variant and inspect the
actual `IdlePML4` tree before loading it. Verify the virtual addresses
used by entry, boot metadata, stacks, and the return path. Platform work
must also supply owned ACPI data and a policy for EFI services, AP startup,
console and device access. An early panic is a failed boot and still
requires a working host recovery path.

The CI test applies and reverses the patch against the pinned source
file, checks its digest and compiles fixtures against the applied range
helper. This test covers the address arithmetic. Compiling the complete
XNU variant and booting it are separate gates.
