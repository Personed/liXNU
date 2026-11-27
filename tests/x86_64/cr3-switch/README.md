# Disposable Linux CR3 switch probe

The probe exercises a real CR3 change and exact restoration inside QEMU TCG.
The positive guest has one possible CPU, four-level paging, NX and plain
physical addresses. PCID is disabled, so a CR3 load invalidates non-global
translations on this CPU. The module checks the profile before allocating
pages or entering the assembly routine. Kernel
page-table isolation is also disabled to avoid relying on the paired roots of
its entry code.

The module copies the active Linux PML4 into a page it owns while preemption
and maskable interrupts are disabled. It leaves the original Linux tree and
all existing mappings intact. An unused lower-half PML4 slot leads to three
test-owned table pages and a read-only, supervisor-only, non-global NX
sentinel page. Linux enables CR4.PGE during early boot. The copied root
preserves identical translations for every Linux global mapping.
[Linux early CR4 setup](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/head_64.S#L192-L223)
All code and data needed by Linux, including an NMI handler if an unexpected
NMI arrives, retain the same mappings. The VM disables the NMI watchdog and
has no injected asynchronous event during this test.

The assembly invalidates any local translation for the alias with
`INVLPG`, which also invalidates a global entry for that address. It then
loads and reads back the private CR3, reads the sentinel through that root,
and restores CR3 before touching the result buffer or returning.
The module checks the original CR3 both in the result and live after each
of the normal-return and cooperative-abort paths. The VM checks that a
Linux syscall still works, the module unloads, and the guest powers off.
A second guest with two possible CPUs must refuse before the switch.

Run with matching x86_64 Linux headers, an unsigned or local Linux kernel
image and a static BusyBox:

```sh
bash tests/x86_64/test-cr3-switch-vm.sh KDIR KERNEL_IMAGE BUSYBOX_STATIC
```

This exercise keeps Linux mappings live. A foreign XNU root still needs
its own entry and return mappings, table ownership, exception and NMI
handling, and PCID/TLB policy. The abort is a deliberate return path in
the shim. Fault recovery, concurrent page-table edits, and physical
Intel or AMD support require their own validation.
