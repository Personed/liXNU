// SPDX-License-Identifier: GPL-2.0-only
/* Disposable-VM-only #PF/#GP recovery probe. */
#include <linux/build_bug.h>
#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/irqflags.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <asm/cpufeature.h>
#include <asm/desc.h>
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <asm/processor-flags.h>
#include <asm/segment.h>
#include <asm/special_insns.h>
#include <asm/pgtable_64_types.h>

#include "fki.h"
#include "fki_x86_64_context.h"
#include "fki_x86_64_transaction.h"
#include "exception_recovery_asm.h"
#include "exception_recovery_uapi.h"

#define ER_STACK_ORDER 2
#define ER_STACK_SIZE (PAGE_SIZE << ER_STACK_ORDER)
#define ER_STACK_GUARD_BYTES sizeof(u64)
#define ER_STACK_LOW_CANARY  0x45524c4f5743414eULL
#define ER_STACK_HIGH_CANARY 0x4552484947484341ULL
#define ER_IDT_ENTRIES 256
#define ER_IDT_BYTES (ER_IDT_ENTRIES * 16)
#define ER_IDTR_PROOF_RESTORED (1ULL << 0)
#define ER_IDTR_PROOF_READBACK (1ULL << 1)
#define ER_GP_SELECTOR_LIMIT_VALID(limit) ((limit) < ER_GP_SELECTOR)
/* C snapshots may change arithmetic flags while evaluating the comparison. */
#define ER_RFLAGS_HOST_CONTROL (X86_EFLAGS_IF | X86_EFLAGS_DF | X86_EFLAGS_IOPL | \
			    X86_EFLAGS_NT | X86_EFLAGS_AC | X86_EFLAGS_VIF | \
			    X86_EFLAGS_VIP)
#define ER_RFLAGS_RETURNED ER_RFLAGS_RETURNED_MASK

struct er_idtr_operand {
	unsigned short limit;
	unsigned long base;
} __packed;

struct er_asm_control {
	unsigned long armed;
	unsigned long expected_vector;
	unsigned long expected_rip;
	unsigned long recovery_rip;
	unsigned long envelope_rsp;
	unsigned long saved_rbx;
	unsigned long saved_rbp;
	unsigned long saved_r12;
	unsigned long saved_r13;
	unsigned long saved_r14;
	unsigned long saved_r15;
	unsigned long fault_window_rsp;
	unsigned long fault_stack_base;
	unsigned long fault_stack_top;
	unsigned long recovery_stack_base;
	unsigned long recovery_stack_top;
	unsigned long recovery_count;
	unsigned long captured_vector;
	unsigned long error_code;
	unsigned long cr2;
	unsigned long fault_rip;
	unsigned long fault_cs;
	unsigned long fault_rflags;
	unsigned long fault_rsp;
	unsigned long fault_ss;
	struct er_idtr_operand original_idtr;
	unsigned char original_pad[6];
	struct er_idtr_operand verify_idtr;
	unsigned char verify_pad[6];
	struct er_idtr_operand shadow_idtr;
	unsigned char shadow_pad[6];
	unsigned long idtr_proof;
	unsigned long terminal_code;
	unsigned long result_flags;
	unsigned long fault_address;
	unsigned long no_fault;
	unsigned long result_vector;
	unsigned long result_error_code;
	unsigned long result_cr2;
	unsigned long result_rip;
	unsigned long result_cs;
	unsigned long result_rflags;
	unsigned long result_rsp;
	unsigned long result_ss;
	unsigned long expected_rax;
	unsigned long saved_ds;
	unsigned long envelope_rip;
	unsigned long task_stack_low;
	unsigned long task_stack_end;
};

struct er_host_snapshot {
	int cpu;
	struct task_struct *task;
	unsigned long stack_page;
	unsigned long stack_end;
	unsigned long rsp;
	unsigned long rflags;
	unsigned long cr0;
	unsigned long cr3;
	unsigned long cr4;
	unsigned long gdt_base;
	unsigned long idt_base;
	unsigned int gdt_limit;
	unsigned int idt_limit;
	unsigned short ldt;
	unsigned short tr;
	unsigned short ds;
	unsigned int preempt_count;
	bool irq_disabled;
	u64 efer;
	u64 fs_base;
	u64 gs_base;
	u64 kernel_gs_base;
	u64 xcr0;
};

struct er_idt_gate {
	u16 offset_low;
	u16 selector;
	u8 ist;
	u8 type_attr;
	u16 offset_mid;
	u32 offset_high;
	u32 reserved;
} __packed;

struct er_gpr_scratch {
	unsigned long rax;
	unsigned long rbx;
	unsigned long rcx;
	unsigned long rdx;
	unsigned long rsi;
	unsigned long rdi;
	unsigned long rbp;
	unsigned long r8;
	unsigned long r9;
	unsigned long r10;
	unsigned long r11;
	unsigned long r12;
	unsigned long r13;
	unsigned long r14;
	unsigned long r15;
	unsigned long rflags;
	unsigned long envelope_rsp;
	unsigned long envelope_rip;
	unsigned long task_stack_low;
	unsigned long task_stack_end;
};

struct er_cpu_state {
	struct er_asm_control control;
	void *idt_copy;
	struct page *fault_stack_pages;
	struct page *recovery_stack_pages;
	struct page *xsave_pages;
	struct er_host_snapshot before;
	struct er_host_snapshot after;
	struct fki_x86_context_control_v1 control_model;
	struct fki_x86_host_constraints host_model;
	u64 xsave_bytes;
	u64 xsave_avx_end;
	u64 xsave_supported_xcr0;
	unsigned int xsave_order;
	unsigned int cpu;
};

struct er_work {
	struct er_cpu_state *state;
	struct x86_exception_recovery_request request;
};

static struct er_cpu_state **er_cpu_states;
static DEFINE_MUTEX(er_ioctl_lock);
static struct miscdevice er_device;
static void *er_fault_guard_owner;
static unsigned long er_fault_address;

/* Assembly reads this only while the ioctl mutex serializes one active probe. */
struct er_asm_control *er_active_state;
extern struct er_gpr_scratch er_recovery_scratch;

long x86_er_inject(struct er_asm_control *control, unsigned int vector);
void x86_er_pf_entry(void);
void x86_er_gp_entry(void);

#define ER_DECLARE_TERMINAL(n) void x86_er_terminal_##n(void)
ER_DECLARE_TERMINAL(0);  ER_DECLARE_TERMINAL(1);  ER_DECLARE_TERMINAL(3);
ER_DECLARE_TERMINAL(4);  ER_DECLARE_TERMINAL(5);  ER_DECLARE_TERMINAL(6);
ER_DECLARE_TERMINAL(7);  ER_DECLARE_TERMINAL(8);  ER_DECLARE_TERMINAL(9);
ER_DECLARE_TERMINAL(10); ER_DECLARE_TERMINAL(11); ER_DECLARE_TERMINAL(12);
ER_DECLARE_TERMINAL(15); ER_DECLARE_TERMINAL(16); ER_DECLARE_TERMINAL(17);
ER_DECLARE_TERMINAL(18); ER_DECLARE_TERMINAL(19); ER_DECLARE_TERMINAL(20);
ER_DECLARE_TERMINAL(21); ER_DECLARE_TERMINAL(22); ER_DECLARE_TERMINAL(23);
ER_DECLARE_TERMINAL(24); ER_DECLARE_TERMINAL(25); ER_DECLARE_TERMINAL(26);
ER_DECLARE_TERMINAL(27); ER_DECLARE_TERMINAL(28); ER_DECLARE_TERMINAL(29);
ER_DECLARE_TERMINAL(30); ER_DECLARE_TERMINAL(31);
#undef ER_DECLARE_TERMINAL

static void (*const er_terminal_entry[ER_IDT_ENTRIES])(void) = {
	[0] = x86_er_terminal_0,   [1] = x86_er_terminal_1,
	[3] = x86_er_terminal_3,   [4] = x86_er_terminal_4,
	[5] = x86_er_terminal_5,   [6] = x86_er_terminal_6,
	[7] = x86_er_terminal_7,   [8] = x86_er_terminal_8,
	[9] = x86_er_terminal_9,   [10] = x86_er_terminal_10,
	[11] = x86_er_terminal_11, [12] = x86_er_terminal_12,
	[15] = x86_er_terminal_15, [16] = x86_er_terminal_16,
	[17] = x86_er_terminal_17, [18] = x86_er_terminal_18,
	[19] = x86_er_terminal_19, [20] = x86_er_terminal_20,
	[21] = x86_er_terminal_21, [22] = x86_er_terminal_22,
	[23] = x86_er_terminal_23, [24] = x86_er_terminal_24,
	[25] = x86_er_terminal_25, [26] = x86_er_terminal_26,
	[27] = x86_er_terminal_27, [28] = x86_er_terminal_28,
	[29] = x86_er_terminal_29, [30] = x86_er_terminal_30,
	[31] = x86_er_terminal_31,
};

static_assert(sizeof(struct er_idtr_operand) == 10);
static_assert(sizeof(struct er_idt_gate) == 16);
static_assert(sizeof(struct er_gpr_scratch) == 160);
static_assert(sizeof(struct x86_exception_recovery_result) == 40);
static_assert(sizeof(struct x86_exception_recovery_request) == 48);
static_assert(offsetof(struct er_asm_control, armed) == ER_OFF_ARMED);
static_assert(offsetof(struct er_asm_control, expected_vector) == ER_OFF_EXPECTED_VECTOR);
static_assert(offsetof(struct er_asm_control, expected_rip) == ER_OFF_EXPECTED_RIP);
static_assert(offsetof(struct er_asm_control, recovery_rip) == ER_OFF_RECOVERY_RIP);
static_assert(offsetof(struct er_asm_control, envelope_rsp) == ER_OFF_ENVELOPE_RSP);
static_assert(offsetof(struct er_asm_control, saved_rbx) == ER_OFF_SAVED_RBX);
static_assert(offsetof(struct er_asm_control, saved_rbp) == ER_OFF_SAVED_RBP);
static_assert(offsetof(struct er_asm_control, saved_r12) == ER_OFF_SAVED_R12);
static_assert(offsetof(struct er_asm_control, saved_r13) == ER_OFF_SAVED_R13);
static_assert(offsetof(struct er_asm_control, saved_r14) == ER_OFF_SAVED_R14);
static_assert(offsetof(struct er_asm_control, saved_r15) == ER_OFF_SAVED_R15);
static_assert(offsetof(struct er_asm_control, fault_window_rsp) == ER_OFF_FAULT_WINDOW_RSP);
static_assert(offsetof(struct er_asm_control, fault_stack_base) == ER_OFF_FAULT_STACK_BASE);
static_assert(offsetof(struct er_asm_control, fault_stack_top) == ER_OFF_FAULT_STACK_TOP);
static_assert(offsetof(struct er_asm_control, recovery_stack_base) == ER_OFF_RECOVERY_STACK_BASE);
static_assert(offsetof(struct er_asm_control, recovery_stack_top) == ER_OFF_RECOVERY_STACK_TOP);
static_assert(offsetof(struct er_asm_control, recovery_count) == ER_OFF_RECOVERY_COUNT);
static_assert(offsetof(struct er_asm_control, captured_vector) == ER_OFF_CAPTURED_VECTOR);
static_assert(offsetof(struct er_asm_control, error_code) == ER_OFF_ERROR_CODE);
static_assert(offsetof(struct er_asm_control, cr2) == ER_OFF_CR2);
static_assert(offsetof(struct er_asm_control, fault_rip) == ER_OFF_FAULT_RIP);
static_assert(offsetof(struct er_asm_control, fault_cs) == ER_OFF_FAULT_CS);
static_assert(offsetof(struct er_asm_control, fault_rflags) == ER_OFF_FAULT_RFLAGS);
static_assert(offsetof(struct er_asm_control, fault_rsp) == ER_OFF_FAULT_RSP);
static_assert(offsetof(struct er_asm_control, fault_ss) == ER_OFF_FAULT_SS);
static_assert(offsetof(struct er_asm_control, original_idtr) == ER_OFF_ORIGINAL_IDTR);
static_assert(offsetof(struct er_asm_control, verify_idtr) == ER_OFF_VERIFY_IDTR);
static_assert(offsetof(struct er_asm_control, shadow_idtr) == ER_OFF_SHADOW_IDTR);
static_assert(offsetof(struct er_asm_control, idtr_proof) == ER_OFF_IDTR_PROOF);
static_assert(offsetof(struct er_asm_control, terminal_code) == ER_OFF_TERMINAL_CODE);
static_assert(offsetof(struct er_asm_control, result_flags) == ER_OFF_RESULT_FLAGS);
static_assert(offsetof(struct er_asm_control, fault_address) == ER_OFF_FAULT_ADDRESS);
static_assert(offsetof(struct er_asm_control, no_fault) == ER_OFF_NO_FAULT);
static_assert(offsetof(struct er_asm_control, result_vector) == ER_OFF_RESULT_VECTOR);
static_assert(offsetof(struct er_asm_control, result_error_code) == ER_OFF_RESULT_ERROR_CODE);
static_assert(offsetof(struct er_asm_control, result_cr2) == ER_OFF_RESULT_CR2);
static_assert(offsetof(struct er_asm_control, result_rip) == ER_OFF_RESULT_RIP);
static_assert(offsetof(struct er_asm_control, result_cs) == ER_OFF_RESULT_CS);
static_assert(offsetof(struct er_asm_control, result_rflags) == ER_OFF_RESULT_RFLAGS);
static_assert(offsetof(struct er_asm_control, result_rsp) == ER_OFF_RESULT_RSP);
static_assert(offsetof(struct er_asm_control, result_ss) == ER_OFF_RESULT_SS);
static_assert(offsetof(struct er_asm_control, expected_rax) == ER_OFF_EXPECTED_RAX);
static_assert(offsetof(struct er_asm_control, saved_ds) == ER_OFF_SAVED_DS);
static_assert(sizeof(struct er_asm_control) == 392);
static_assert(ER_SENTINEL_R11 != 0,
	      "R11 sentinel must differ from the expected #GP CR2 value");
static_assert(offsetof(struct er_gpr_scratch, rax) == ER_SCRATCH_RAX);
static_assert(offsetof(struct er_gpr_scratch, r15) == ER_SCRATCH_R15);
static_assert(offsetof(struct er_gpr_scratch, rflags) == ER_SCRATCH_FLAGS);
static_assert(offsetof(struct er_gpr_scratch, envelope_rsp) == ER_SCRATCH_ENVELOPE_RSP);
static_assert(offsetof(struct er_gpr_scratch, envelope_rip) == ER_SCRATCH_ENVELOPE_RIP);
static_assert(offsetof(struct er_gpr_scratch, task_stack_low) == ER_SCRATCH_TASK_STACK_LOW);
static_assert(offsetof(struct er_gpr_scratch, task_stack_end) == ER_SCRATCH_TASK_STACK_END);
static_assert(offsetof(struct er_asm_control, envelope_rip) == ER_OFF_ENVELOPE_RIP);
static_assert(offsetof(struct er_asm_control, task_stack_low) == ER_OFF_TASK_STACK_LOW);
static_assert(offsetof(struct er_asm_control, task_stack_end) == ER_OFF_TASK_STACK_END);
static_assert(ER_GP_SELECTOR == X86_EXCEPTION_RECOVERY_GP_ERROR);
static_assert(ER_GP_SELECTOR_LIMIT_VALID(ER_GP_SELECTOR - 1));
static_assert(!ER_GP_SELECTOR_LIMIT_VALID(ER_GP_SELECTOR));
static_assert(!ER_GP_SELECTOR_LIMIT_VALID(0xffff));
static_assert(ER_RESULT_GPRS_MATCHED == X86_EXCEPTION_RECOVERY_GPRS_MATCHED);
static_assert(X86_EXCEPTION_RECOVERY_RUN ==
	      _IOWR(0xb8, 1, struct x86_exception_recovery_request));

static int er_read_xcr0(u64 *xcr0)
{
	u32 low, high;

	if (!boot_cpu_has(X86_FEATURE_XSAVE) ||
	    !boot_cpu_has(X86_FEATURE_OSXSAVE) ||
	    !boot_cpu_has(X86_FEATURE_AVX))
		return -EOPNOTSUPP;
	asm volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
	*xcr0 = ((u64)high << 32) | low;
	if ((*xcr0 & 0x6) != 0x6)
		return -EOPNOTSUPP;
	return 0;
}

static noinline int er_snapshot(struct er_host_snapshot *snapshot)
{
	struct desc_ptr gdt, idt;
	unsigned long flags;
	u32 low, high;
	int ret;

	snapshot->cpu = raw_smp_processor_id();
	snapshot->task = current;
	snapshot->stack_page = (unsigned long)task_stack_page(current);
	snapshot->stack_end = snapshot->stack_page + THREAD_SIZE;
	asm volatile("mov %%rsp, %0\n\tpushfq\n\tpopq %1"
		     : "=r"(snapshot->rsp), "=r"(flags) :: "memory");
	snapshot->rflags = flags;
	snapshot->preempt_count = preempt_count();
	snapshot->irq_disabled = irqs_disabled();
	snapshot->cr0 = read_cr0();
	snapshot->cr3 = __read_cr3();
	snapshot->cr4 = __read_cr4();
	native_store_gdt(&gdt);
	store_idt(&idt);
	asm volatile("sldt %0" : "=m"(snapshot->ldt));
	asm volatile("str %0" : "=m"(snapshot->tr));
	asm volatile("mov %%ds, %0" : "=r"(snapshot->ds));
	snapshot->gdt_base = gdt.address;
	snapshot->gdt_limit = gdt.size;
	snapshot->idt_base = idt.address;
	snapshot->idt_limit = idt.size;
	ret = er_read_xcr0(&snapshot->xcr0);
	if (ret)
		return ret;
	if (rdmsr_safe(MSR_FS_BASE, &low, &high))
		return -EIO;
	snapshot->fs_base = ((u64)high << 32) | low;
	if (rdmsr_safe(MSR_GS_BASE, &low, &high))
		return -EIO;
	snapshot->gs_base = ((u64)high << 32) | low;
	if (rdmsr_safe(MSR_KERNEL_GS_BASE, &low, &high))
		return -EIO;
	snapshot->kernel_gs_base = ((u64)high << 32) | low;
	if (rdmsr_safe(MSR_EFER, &low, &high))
		return -EIO;
	snapshot->efer = ((u64)high << 32) | low;
	return 0;
}

static bool er_snapshots_match(const struct er_host_snapshot *before,
			       const struct er_host_snapshot *after)
{
	return before->cpu == after->cpu && before->task == after->task &&
	       before->stack_page == after->stack_page &&
	       before->stack_end == after->stack_end && before->rsp == after->rsp &&
	       before->preempt_count == after->preempt_count &&
	       before->irq_disabled == after->irq_disabled &&
	       before->xcr0 == after->xcr0 && before->cr0 == after->cr0 &&
	       before->cr3 == after->cr3 && before->cr4 == after->cr4 &&
	       before->efer == after->efer &&
	       before->gdt_base == after->gdt_base &&
	       before->gdt_limit == after->gdt_limit &&
	       before->idt_base == after->idt_base &&
	       before->idt_limit == after->idt_limit &&
	       before->ldt == after->ldt && before->tr == after->tr &&
	       before->ds == after->ds &&
	       ((before->rflags ^ after->rflags) & ER_RFLAGS_HOST_CONTROL) == 0 &&
	       before->fs_base == after->fs_base && before->gs_base == after->gs_base &&
	       before->kernel_gs_base == after->kernel_gs_base;
}

static void er_set_gate(struct er_idt_gate *gate, unsigned long handler, u8 ist)
{
	gate->offset_low = handler & 0xffff;
	gate->selector = __KERNEL_CS;
	gate->ist = ist & 7;
	gate->type_attr = 0x8e; /* present, DPL 0, 64-bit interrupt gate */
	gate->offset_mid = (handler >> 16) & 0xffff;
	gate->offset_high = (handler >> 32) & 0xffffffff;
	gate->reserved = 0;
}

static int er_build_shadow_idt(struct er_cpu_state *state,
			       const struct desc_ptr *original)
{
	struct er_idt_gate *gates = state->idt_copy;
	struct er_idt_gate *original_gates = (void *)original->address;
	unsigned long bytes = (unsigned long)original->size + 1;
	unsigned int entries, vector;
	u8 df_ist;

	if (!state->idt_copy || bytes > ER_IDT_BYTES || bytes < 32 * sizeof(*gates) ||
	    bytes % sizeof(*gates))
		return -EOPNOTSUPP;
	memcpy(state->idt_copy, (const void *)original->address, bytes);
	entries = bytes / sizeof(*gates);
	if (entries <= X86_EXCEPTION_RECOVERY_PF ||
	    entries <= X86_EXCEPTION_RECOVERY_GP || entries <= 8)
		return -EOPNOTSUPP;
	df_ist = original_gates[8].ist & 7;
	if (!df_ist)
		return -EOPNOTSUPP;

	for (vector = 0; vector < 32; vector++) {
		if (vector == 2 || vector == X86_EXCEPTION_RECOVERY_PF ||
		    vector == X86_EXCEPTION_RECOVERY_GP)
			continue;
		if (!er_terminal_entry[vector])
			return -EOPNOTSUPP;
		er_set_gate(&gates[vector], (unsigned long)er_terminal_entry[vector],
			    original_gates[vector].ist & 7);
	}
	er_set_gate(&gates[X86_EXCEPTION_RECOVERY_PF],
		    (unsigned long)x86_er_pf_entry, 0);
	er_set_gate(&gates[X86_EXCEPTION_RECOVERY_GP],
		    (unsigned long)x86_er_gp_entry, 0);

	state->control.original_idtr.limit = original->size;
	state->control.original_idtr.base = original->address;
	state->control.shadow_idtr.limit = original->size;
	state->control.shadow_idtr.base = (unsigned long)state->idt_copy;
	return 0;
}

static bool er_stack_canaries_intact(const struct er_cpu_state *state)
{
	unsigned long fault_base = state->control.fault_stack_base;
	unsigned long recovery_base = state->control.recovery_stack_base;
	unsigned long fault_high = fault_base + ER_STACK_SIZE - ER_STACK_GUARD_BYTES;
	unsigned long recovery_high = recovery_base + ER_STACK_SIZE - ER_STACK_GUARD_BYTES;

	return READ_ONCE(*(u64 *)fault_base) == ER_STACK_LOW_CANARY &&
	       READ_ONCE(*(u64 *)fault_high) == ER_STACK_HIGH_CANARY &&
	       READ_ONCE(*(u64 *)recovery_base) == ER_STACK_LOW_CANARY &&
	       READ_ONCE(*(u64 *)recovery_high) == ER_STACK_HIGH_CANARY;
}

static int er_check_fault_address(unsigned long address)
{
	u64 value;

	if (!address || copy_from_kernel_nofault(&value, (const void *)address,
						 sizeof(value)) != -EFAULT)
		return -EOPNOTSUPP;
	return 0;
}

static int er_xsave_constraints(u64 *bytes, u64 *avx_end, u64 *supported_xcr0)
{
	u32 eax, ebx, ecx, edx;
	u64 enabled_bytes, end;

	cpuid_count(0xd, 0, &eax, &ebx, &ecx, &edx);
	*supported_xcr0 = eax | ((u64)edx << 32);
	enabled_bytes = ebx;
	if (!enabled_bytes || enabled_bytes < FKI_X86_XSAVE_MIN_BYTES)
		return -EOPNOTSUPP;
	cpuid_count(0xd, 2, &eax, &ebx, &ecx, &edx);
	if (!eax || !ebx)
		return -EOPNOTSUPP;
	end = (u64)ebx + eax;
	if (end < FKI_X86_XSAVE_AVX_MIN_BYTES || end > enabled_bytes)
		return -EOPNOTSUPP;
	*bytes = enabled_bytes;
	*avx_end = end;
	return 0;
}

static int er_tx_step(enum fki_x86_transaction_state *state,
		      enum fki_x86_transaction_event event)
{
	int err = fki_x86_transaction_transition(state, event);

	if (err)
		pr_err("x86-exception-recovery: transaction model rejected event %u in state %u: %d\n",
		       event, *state, err);
	return err;
}

static int er_prepare_control_model(struct er_cpu_state *state,
				    unsigned long entry_rsp,
				    unsigned long entry_rip)
{
	const struct er_host_snapshot *snapshot = &state->before;
	u64 control_xcr0 = snapshot->xcr0 & FKI_X86_XCR0_V1_MASK;

	state->host_model = (struct fki_x86_host_constraints) {
		.cr4_allowed = snapshot->cr4,
		.efer_allowed = snapshot->efer,
		.xcr0_supported = state->xsave_supported_xcr0,
		.xsave_max_bytes = state->xsave_bytes,
		.xsave_avx_end = state->xsave_avx_end,
		.physical_bits = boot_cpu_data.x86_phys_bits,
		.flags = boot_cpu_has(X86_FEATURE_PCID) ? FKI_X86_HOST_CPU_HAS_PCID : 0,
	};
	state->control_model = (struct fki_x86_context_control_v1) {
		.abi_version = FKI_X86_CONTEXT_ABI_VERSION,
		.control_version = FKI_X86_CONTEXT_CONTROL_VERSION,
		.state_size = FKI_X86_CONTEXT_V1_BYTES,
		.profile = FKI_X86_CONTEXT_PROFILE_HOST_BOUND_SHIM,
		.operation = FKI_X86_CONTEXT_OP_ENTER,
		.rsp = entry_rsp,
		.rip = entry_rip,
		.rflags = snapshot->rflags,
		.cs = __KERNEL_CS,
		.ss = __KERNEL_DS,
		.ds = __KERNEL_DS,
		.es = __KERNEL_DS,
		.tr = snapshot->tr,
		.fs_base = snapshot->fs_base,
		.gs_base = snapshot->gs_base,
		.kernel_gs_base = snapshot->kernel_gs_base,
		.cr0 = snapshot->cr0,
		.cr3 = snapshot->cr3,
		.cr4 = snapshot->cr4,
		.efer = snapshot->efer,
		.xcr0 = control_xcr0,
		.gdt_base = snapshot->gdt_base,
		.idt_base = snapshot->idt_base,
		.gdt_limit = snapshot->gdt_limit,
		.idt_limit = snapshot->idt_limit,
		.xsave_address = (unsigned long)page_address(state->xsave_pages),
		.xsave_bytes = state->xsave_bytes,
		.xsave_xstate_bv = control_xcr0,
		.xsave_format = FKI_X86_XSAVE_FORMAT_STANDARD,
	};
	if (!control_xcr0)
		return -EOPNOTSUPP;
	return 0;
}

static int er_validate_fault_stop_report(struct er_cpu_state *state,
					 unsigned int vector)
{
	struct er_asm_control *captured = &state->control;
	struct fki_x86_context_control_v1 *report = &state->control_model;

	report->stop_reason = vector == X86_EXCEPTION_RECOVERY_PF ?
		FKI_X86_STOP_PAGE_FAULT : FKI_X86_STOP_GENERAL_PROTECTION;
	report->stop_vector = vector;
	report->stop_error_code = captured->error_code;
	report->stop_cr2 = captured->cr2;
	report->stop_rip = captured->fault_rip;
	report->stop_rsp = captured->fault_rsp;
	return fki_x86_validate_stop_report_v1(report);
}

static long er_run_on_cpu(void *argument)
{
	struct er_work *work = argument;
	struct er_cpu_state *state = work->state;
	struct er_asm_control *control = &state->control;
	struct desc_ptr current_idtr;
	unsigned long irq_flags;
	unsigned long injection_call_rsp;
	long ret;
	u32 required_flags;
	struct desc_ptr current_gdt;
	unsigned long required_error_code;
	unsigned long required_asm_flags;
	enum fki_x86_transaction_state tx = FKI_X86_TX_INIT;
	bool after_valid = false;

	preempt_disable();
	local_irq_save(irq_flags);
	if (raw_smp_processor_id() != work->request.cpu) {
		ret = -EXDEV;
		goto out_restore;
	}
	if (fki_caps() != 0) {
		ret = -EOPNOTSUPP;
		goto out_restore;
	}
	ret = er_snapshot(&state->before);
	if (ret)
		goto out_restore;
	if (!state->before.irq_disabled ||
	    state->before.rsp < state->before.stack_page ||
	    state->before.rsp >= state->before.stack_end) {
		ret = -EUCLEAN;
		goto out_restore;
	}
#ifdef CONFIG_PREEMPT_COUNT
	if (!state->before.preempt_count) {
		ret = -EUCLEAN;
		goto out_restore;
	}
#endif

	memset(control, 0, sizeof(*control));
	control->task_stack_low = state->before.stack_page;
	control->task_stack_end = state->before.stack_end;
	control->fault_stack_base = (unsigned long)page_address(state->fault_stack_pages);
	control->fault_stack_top = control->fault_stack_base + ER_STACK_SIZE -
				   2 * ER_STACK_GUARD_BYTES;
	control->recovery_stack_base = (unsigned long)page_address(state->recovery_stack_pages);
	control->recovery_stack_top = control->recovery_stack_base + ER_STACK_SIZE -
				     2 * ER_STACK_GUARD_BYTES;
	control->fault_address = READ_ONCE(er_fault_address);
	control->saved_ds = state->before.ds;
	control->expected_vector = work->request.vector;
	store_idt(&current_idtr);
	ret = er_build_shadow_idt(state, &current_idtr);
	if (ret)
		goto out_restore;
	if (!er_stack_canaries_intact(state)) {
		ret = -EUCLEAN;
		goto out_restore;
	}
	if (fki_caps() != 0) {
		ret = -EOPNOTSUPP;
		goto out_restore;
	}
	ret = er_check_fault_address(control->fault_address);
	if (ret)
		goto out_restore;
	if (work->request.vector == X86_EXCEPTION_RECOVERY_PF &&
	    control->fault_address == ER_SENTINEL_R11) {
		pr_err("x86-exception-recovery: not-run, R11 sentinel matches expected CR2\n");
		ret = -EOPNOTSUPP;
		goto out_restore;
	}
	if (work->request.vector == X86_EXCEPTION_RECOVERY_GP) {
		/* SGDT must prove selector 0xfff8 is beyond this CPU's GDT limit. */
		native_store_gdt(&current_gdt);
		if (!ER_GP_SELECTOR_LIMIT_VALID(current_gdt.size) ||
		    current_gdt.address != state->before.gdt_base ||
		    current_gdt.size != state->before.gdt_limit) {
			ret = -EOPNOTSUPP;
			goto out_restore;
		}
	}
	asm volatile("mov %%rsp, %0" : "=r"(injection_call_rsp));
	if (injection_call_rsp < state->before.stack_page ||
	    injection_call_rsp >= state->before.stack_end ||
	    injection_call_rsp - state->before.stack_page <
		ER_TASK_STACK_FAULT_HEADROOM + 7 * sizeof(unsigned long)) {
		ret = -EUCLEAN;
		goto out_snapshot;
	}
	ret = er_prepare_control_model(state, control->recovery_stack_top,
				       (unsigned long)x86_er_inject);
	if (ret)
		goto out_restore;
	ret = fki_x86_context_validate_host_bound_shim(&state->control_model,
							 &state->host_model);
	if (ret)
		goto out_restore;
	ret = er_tx_step(&tx, FKI_X86_TX_VALIDATE_OK);
	if (ret)
		goto out_restore;
	ret = er_tx_step(&tx, FKI_X86_TX_SNAPSHOT_OK);
	if (ret)
		goto out_restore;
	ret = er_tx_step(&tx, FKI_X86_TX_ENTER);
	if (ret)
		goto out_restore;
	er_active_state = control;
	smp_wmb();
	ret = x86_er_inject(control, work->request.vector);
	smp_rmb();
	if (ret)
		goto out_snapshot;
	required_error_code = work->request.vector == X86_EXCEPTION_RECOVERY_GP ?
		X86_EXCEPTION_RECOVERY_GP_ERROR : 0;
	if (control->captured_vector != work->request.vector ||
	    control->result_vector != control->captured_vector ||
	    control->error_code != required_error_code ||
	    control->result_error_code != control->error_code ||
	    control->fault_rip != control->expected_rip ||
	    control->result_rip != control->fault_rip ||
	    control->fault_rsp != control->fault_window_rsp ||
	    control->result_rsp != control->fault_rsp ||
	    control->fault_cs != __KERNEL_CS || !control->fault_ss ||
	    control->result_cs != control->fault_cs ||
	    control->result_ss != control->fault_ss ||
	    ((control->result_rflags ^ control->fault_rflags) & ER_RFLAGS_RETURNED) ||
	    (control->fault_rflags & X86_EFLAGS_IF) ||
	    control->recovery_count != 1 ||
	    control->idtr_proof != (ER_IDTR_PROOF_RESTORED | ER_IDTR_PROOF_READBACK)) {
		ret = -EUCLEAN;
		goto out_snapshot;
	}
	if (work->request.vector == X86_EXCEPTION_RECOVERY_PF &&
	    (control->cr2 != control->fault_address ||
	     control->result_cr2 != control->cr2)) {
		ret = -EUCLEAN;
		goto out_snapshot;
	}
	if (work->request.vector == X86_EXCEPTION_RECOVERY_GP &&
	    (control->cr2 || control->result_cr2)) {
		ret = -EUCLEAN;
		goto out_snapshot;
	}
	required_asm_flags = X86_EXCEPTION_RECOVERY_RIP_MATCHED |
		X86_EXCEPTION_RECOVERY_GPRS_MATCHED |
		X86_EXCEPTION_RECOVERY_IDTR_RESTORED |
		X86_EXCEPTION_RECOVERY_ONCE |
		X86_EXCEPTION_RECOVERY_STACKS_INTACT |
		X86_EXCEPTION_RECOVERY_CPU_MATCHED |
		ER_RESULT_ENVELOPE_MATCHED;
	if (work->request.vector == X86_EXCEPTION_RECOVERY_PF)
		required_asm_flags |= X86_EXCEPTION_RECOVERY_CR2_MATCHED;
	if (control->result_flags != required_asm_flags) {
		ret = -EUCLEAN;
		goto out_snapshot;
	}
	if (!er_stack_canaries_intact(state)) {
		ret = -EUCLEAN;
		goto out_snapshot;
	}
	ret = er_validate_fault_stop_report(state, work->request.vector);
	if (ret)
		goto out_snapshot;
	ret = er_tx_step(&tx, FKI_X86_TX_FAULT_STOP);
	if (ret)
		goto out_snapshot;
	ret = 0;

out_snapshot:
	if (er_snapshot(&state->after)) {
		ret = -EIO;
	} else {
		after_valid = true;
		if (!er_snapshots_match(&state->before, &state->after))
			ret = -EUCLEAN;
	}
	if (after_valid && (state->after.cpu != work->request.cpu ||
	    state->after.idt_base != state->before.idt_base ||
	    state->after.idt_limit != state->before.idt_limit))
		ret = -EUCLEAN;

	if (!ret) {
		ret = er_tx_step(&tx, FKI_X86_TX_RESTORE_OK);
		if (!ret)
			ret = er_tx_step(&tx, FKI_X86_TX_ABORT);
		if (!ret && tx != FKI_X86_TX_ABORTED)
			ret = -EUCLEAN;
	}

	if (!ret) {
		work->request.result.cpu = work->request.cpu;
		work->request.result.vector = control->captured_vector;
		work->request.result.error_code = control->error_code;
		work->request.result.recovery_count = control->recovery_count;
		work->request.result.fault_rip = control->fault_rip;
		work->request.result.cr2 = control->cr2;
		work->request.result.flags =
			X86_EXCEPTION_RECOVERY_RIP_MATCHED |
			X86_EXCEPTION_RECOVERY_GPRS_MATCHED |
			X86_EXCEPTION_RECOVERY_DS_PRESERVED |
			X86_EXCEPTION_RECOVERY_IDTR_RESTORED |
			X86_EXCEPTION_RECOVERY_ONCE |
			X86_EXCEPTION_RECOVERY_STACKS_INTACT |
			X86_EXCEPTION_RECOVERY_CPU_MATCHED;
		if (work->request.vector == X86_EXCEPTION_RECOVERY_PF)
			work->request.result.flags |= X86_EXCEPTION_RECOVERY_CR2_MATCHED;
	}

	er_active_state = NULL;

out_restore:
	local_irq_restore(irq_flags);
	preempt_enable();
	if (ret)
		pr_err("x86-exception-recovery: cpu=%u vector=%u failed status=%ld\n",
		       work->request.cpu, work->request.vector, ret);
	else {
		required_flags = X86_EXCEPTION_RECOVERY_RIP_MATCHED |
			X86_EXCEPTION_RECOVERY_GPRS_MATCHED |
			X86_EXCEPTION_RECOVERY_DS_PRESERVED |
			X86_EXCEPTION_RECOVERY_IDTR_RESTORED |
			X86_EXCEPTION_RECOVERY_ONCE |
			X86_EXCEPTION_RECOVERY_STACKS_INTACT |
			X86_EXCEPTION_RECOVERY_CPU_MATCHED;
		if (work->request.vector == X86_EXCEPTION_RECOVERY_PF)
			required_flags |= X86_EXCEPTION_RECOVERY_CR2_MATCHED;
		if ((work->request.result.flags & required_flags) != required_flags)
			return -EUCLEAN;
		pr_info("x86-exception-recovery: cpu=%u vector=%u rip=%#lx error=%#llx "
			"cr2=%#lx recovery_count=%u idtr_restored=1 gprs=ok ds=%#x stacks=ok transaction_state=%u\n",
			work->request.result.cpu, work->request.result.vector,
			(unsigned long)work->request.result.fault_rip,
			(unsigned long long)work->request.result.error_code,
			(unsigned long)work->request.result.cr2,
			work->request.result.recovery_count,
			(unsigned int)state->after.ds, tx);
	}
	return ret;
}

static long er_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct x86_exception_recovery_request request;
	struct er_work work;
	long ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!boot_cpu_has(X86_FEATURE_HYPERVISOR))
		return -EOPNOTSUPP;
	if (command != X86_EXCEPTION_RECOVERY_RUN)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
		return -EFAULT;
	if (request.vector != X86_EXCEPTION_RECOVERY_PF &&
	    request.vector != X86_EXCEPTION_RECOVERY_GP)
		return -EINVAL;
	if (request.cpu >= nr_cpu_ids || !cpu_online(request.cpu))
		return -EINVAL;
	if (fki_caps() != 0)
		return -EOPNOTSUPP;
	if (!er_fault_guard_owner || !er_fault_address)
		return -EOPNOTSUPP;
	ret = er_check_fault_address(READ_ONCE(er_fault_address));
	if (ret) {
		pr_err("x86-exception-recovery: not-run, canonical fault address is mapped\n");
		return ret;
	}

	memset(&request.result, 0, sizeof(request.result));
	work.request = request;
	work.state = er_cpu_states[request.cpu];
	if (!work.state)
		return -EOPNOTSUPP;
	mutex_lock(&er_ioctl_lock);
	ret = work_on_cpu(request.cpu, er_run_on_cpu, &work);
	mutex_unlock(&er_ioctl_lock);
	if (ret)
		return ret == -EAGAIN ? -EOPNOTSUPP : ret;
	if (copy_to_user((void __user *)argument, &work.request, sizeof(work.request)))
		return -EFAULT;
	return 0;
}

static const struct file_operations er_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = er_ioctl,
};

static void er_free_states(void)
{
	int cpu;

	if (!er_cpu_states)
		return;
	for_each_possible_cpu(cpu) {
		struct er_cpu_state *state = er_cpu_states[cpu];

		if (!state)
			continue;
		if (state->idt_copy)
			free_page((unsigned long)state->idt_copy);
		if (state->fault_stack_pages)
			__free_pages(state->fault_stack_pages, ER_STACK_ORDER);
		if (state->recovery_stack_pages)
			__free_pages(state->recovery_stack_pages, ER_STACK_ORDER);
		if (state->xsave_pages)
			__free_pages(state->xsave_pages, state->xsave_order);
		kfree(state);
	}
	kfree(er_cpu_states);
	er_cpu_states = NULL;
}

static int __init er_init(void)
{
	u64 xcr0, xsave_bytes, xsave_avx_end, supported_xcr0;
	unsigned int xsave_order;
	int cpu, ret;

	if (!boot_cpu_has(X86_FEATURE_HYPERVISOR)) {
		pr_err("x86-exception-recovery: refusing to load outside a detected hypervisor\n");
		return -EPERM;
	}
	if (fki_caps() != 0)
		return -EOPNOTSUPP;
	ret = er_read_xcr0(&xcr0);
	if (ret)
		return ret;
	ret = er_xsave_constraints(&xsave_bytes, &xsave_avx_end, &supported_xcr0);
	if (ret)
		return ret;
	xsave_order = get_order(xsave_bytes);
	er_cpu_states = kcalloc(nr_cpu_ids, sizeof(*er_cpu_states), GFP_KERNEL);
	if (!er_cpu_states)
		return -ENOMEM;
	for_each_possible_cpu(cpu) {
		struct er_cpu_state *state = kzalloc(sizeof(*state), GFP_KERNEL);
		u64 *fault_low, *fault_high, *recovery_low, *recovery_high;

		if (!state) {
			ret = -ENOMEM;
			goto fail;
		}
		er_cpu_states[cpu] = state;
		state->cpu = cpu;
		state->idt_copy = (void *)get_zeroed_page(GFP_KERNEL);
		state->fault_stack_pages = alloc_pages(GFP_KERNEL | __GFP_ZERO,
							      ER_STACK_ORDER);
		state->recovery_stack_pages = alloc_pages(GFP_KERNEL | __GFP_ZERO,
								  ER_STACK_ORDER);
		state->xsave_pages = alloc_pages(GFP_KERNEL | __GFP_ZERO,
						       xsave_order);
		state->xsave_bytes = xsave_bytes;
		state->xsave_avx_end = xsave_avx_end;
		state->xsave_supported_xcr0 = supported_xcr0;
		state->xsave_order = xsave_order;
		if (!state->idt_copy || !state->fault_stack_pages ||
		    !state->recovery_stack_pages || !state->xsave_pages) {
			ret = -ENOMEM;
			goto fail;
		}
		fault_low = page_address(state->fault_stack_pages);
		fault_high = (void *)((unsigned long)fault_low + ER_STACK_SIZE -
				      ER_STACK_GUARD_BYTES);
		recovery_low = page_address(state->recovery_stack_pages);
		recovery_high = (void *)((unsigned long)recovery_low + ER_STACK_SIZE -
					 ER_STACK_GUARD_BYTES);
		WRITE_ONCE(*fault_low, ER_STACK_LOW_CANARY);
		WRITE_ONCE(*fault_high, ER_STACK_HIGH_CANARY);
		WRITE_ONCE(*recovery_low, ER_STACK_LOW_CANARY);
		WRITE_ONCE(*recovery_high, ER_STACK_HIGH_CANARY);
	}

	/* Keep this vmalloc allocation alive; its following guard page is the #PF target. */
	er_fault_guard_owner = vmalloc(PAGE_SIZE);
	if (!er_fault_guard_owner) {
		ret = -ENOMEM;
		goto fail;
	}
	er_fault_address = (unsigned long)er_fault_guard_owner + PAGE_SIZE;
	ret = er_check_fault_address(er_fault_address);
	if (ret)
		goto fail;

	er_device.minor = MISC_DYNAMIC_MINOR;
	er_device.name = "x86-exception-recovery";
	er_device.fops = &er_fops;
	ret = misc_register(&er_device);
	if (ret)
		goto fail;
	pr_info("x86-exception-recovery: test-only; fki_caps=0 xcr0=%#llx\n",
		(unsigned long long)xcr0);
	return 0;

fail:
	er_free_states();
	if (er_fault_guard_owner) {
		vfree(er_fault_guard_owner);
		er_fault_guard_owner = NULL;
		er_fault_address = 0;
	}
	return ret;
}

static void __exit er_exit(void)
{
	misc_deregister(&er_device);
	er_free_states();
	if (er_fault_guard_owner) {
		vfree(er_fault_guard_owner);
		er_fault_guard_owner = NULL;
		er_fault_address = 0;
	}
}

module_init(er_init);
module_exit(er_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Disposable-VM-only x86 #PF/#GP exception recovery probe");
