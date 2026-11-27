// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <asm/cpufeature.h>
#include <asm/desc.h>
#include <asm/nmi.h>
#include <asm/ptrace.h>

#include "nmi_boundary_uapi.h"

#define DEVICE_NAME "x86-nmi-boundary"
#define NMI_HANDLER_NAME "x86-nmi-boundary-probe"

#define NMI_PHASE_PREPARE 1
#define NMI_PHASE_ARMED 2
#define NMI_PHASE_FIRED 3
#define NMI_PHASE_DRAIN 4
#define NMI_PHASE_DONE 5

#define NMI_EXPECTED_FLAGS 0x46ULL
#define NMI_FLAGS_MASK 0x24ffffULL

#define NMI_GPR_RAX (1U << 0)
#define NMI_GPR_RBX (1U << 1)
#define NMI_GPR_RBP (1U << 2)
#define NMI_GPR_RCX (1U << 3)
#define NMI_GPR_RDX (1U << 4)
#define NMI_GPR_RDI (1U << 5)
#define NMI_GPR_RSI (1U << 6)
#define NMI_GPR_R8  (1U << 7)
#define NMI_GPR_R9  (1U << 8)
#define NMI_GPR_R10 (1U << 9)
#define NMI_GPR_R11 (1U << 10)
#define NMI_GPR_R12 (1U << 11)
#define NMI_GPR_R13 (1U << 12)
#define NMI_GPR_R14 (1U << 13)
#define NMI_GPR_R15 (1U << 14)

#define NMI_CANARY_RAX 0x4eULL
#define NMI_CANARY_RBX 0x11111111ULL
#define NMI_CANARY_RCX 0ULL
#define NMI_CANARY_RDX 0x22222222ULL
#define NMI_CANARY_RDI 0x44444444ULL
#define NMI_CANARY_RSI 0x33333333ULL
#define NMI_CANARY_R8  0x66666666ULL
#define NMI_CANARY_R9  0x77777777ULL
#define NMI_CANARY_R10 0x12345678ULL
#define NMI_CANARY_R11 0x23456789ULL
#define NMI_CANARY_R12 0x3456789aULL
#define NMI_CANARY_R13 0x456789abULL
#define NMI_CANARY_R14 0x56789abcULL
#define NMI_CANARY_R15 0x6789abcdULL

extern char x86_nmi_probe_window_begin[];
extern char x86_nmi_probe_window_end[];
extern void x86_nmi_probe_wait(void);

/* Shared with the assembly shim; the VM has exactly one vCPU. */
int x86_nmi_probe_phase;
int x86_nmi_probe_done;
unsigned long x86_nmi_probe_expected_rsp;
unsigned long x86_nmi_probe_expected_rbp;
unsigned long x86_nmi_probe_return_rflags;
unsigned int x86_nmi_probe_return_gpr_mask;
unsigned int x86_nmi_probe_return_rsp_ok;
unsigned int x86_nmi_probe_return_rflags_ok;

static DEFINE_MUTEX(probe_mutex);
static atomic_t nmi_count = ATOMIC_INIT(0);
static atomic_t duplicate_count = ATOMIC_INIT(0);
static atomic_t unexpected_count = ATOMIC_INIT(0);
static struct x86_nmi_boundary_result probe_result;
static struct task_struct *probe_task;
static bool handler_registered;
static bool run_started;
static bool run_finalized;

static __always_inline unsigned int nmi_gpr_match_mask(const struct pt_regs *regs)
{
	unsigned int mask = 0;

	if (regs->ax == NMI_CANARY_RAX)
		mask |= NMI_GPR_RAX;
	if (regs->bx == NMI_CANARY_RBX)
		mask |= NMI_GPR_RBX;
	if (regs->bp == READ_ONCE(x86_nmi_probe_expected_rbp))
		mask |= NMI_GPR_RBP;
	if (regs->cx == NMI_CANARY_RCX)
		mask |= NMI_GPR_RCX;
	if (regs->dx == NMI_CANARY_RDX)
		mask |= NMI_GPR_RDX;
	if (regs->di == NMI_CANARY_RDI)
		mask |= NMI_GPR_RDI;
	if (regs->si == NMI_CANARY_RSI)
		mask |= NMI_GPR_RSI;
	if (regs->r8 == NMI_CANARY_R8)
		mask |= NMI_GPR_R8;
	if (regs->r9 == NMI_CANARY_R9)
		mask |= NMI_GPR_R9;
	if (regs->r10 == NMI_CANARY_R10)
		mask |= NMI_GPR_R10;
	if (regs->r11 == NMI_CANARY_R11)
		mask |= NMI_GPR_R11;
	if (regs->r12 == NMI_CANARY_R12)
		mask |= NMI_GPR_R12;
	if (regs->r13 == NMI_CANARY_R13)
		mask |= NMI_GPR_R13;
	if (regs->r14 == NMI_CANARY_R14)
		mask |= NMI_GPR_R14;
	if (regs->r15 == NMI_CANARY_R15)
		mask |= NMI_GPR_R15;

	return mask;
}

static __always_inline unsigned long read_host_cr3(void)
{
	unsigned long cr3;

	asm volatile("mov %%cr3, %0" : "=r"(cr3) :: "memory");
	return cr3;
}

static notrace int nmi_boundary_handler(unsigned int type, struct pt_regs *regs)
{
	struct desc_ptr idtr;
	unsigned int gpr_mask;
	int old_phase;
	int cpu;

	atomic_inc(&nmi_count);
	old_phase = cmpxchg(&x86_nmi_probe_phase, NMI_PHASE_ARMED,
			    NMI_PHASE_FIRED);
	if (old_phase != NMI_PHASE_ARMED) {
		if (old_phase == NMI_PHASE_FIRED || old_phase == NMI_PHASE_DRAIN)
			atomic_inc(&duplicate_count);
		else
			atomic_inc(&unexpected_count);
		return NMI_HANDLED;
	}

	cpu = raw_smp_processor_id();
	gpr_mask = nmi_gpr_match_mask(regs);
	store_idt(&idtr);
	WRITE_ONCE(probe_result.nmi_type, type);
	WRITE_ONCE(probe_result.cpu_nmi, cpu);
	WRITE_ONCE(probe_result.nmi_ip, regs->ip);
	WRITE_ONCE(probe_result.nmi_rsp, regs->sp);
	WRITE_ONCE(probe_result.nmi_rflags, regs->flags);
	WRITE_ONCE(probe_result.task_pid_nmi, current->pid);
	WRITE_ONCE(probe_result.cr3_nmi, read_host_cr3());
	WRITE_ONCE(probe_result.expected_rsp,
		   READ_ONCE(x86_nmi_probe_expected_rsp));
	WRITE_ONCE(probe_result.callback_gpr_mask, gpr_mask);
	WRITE_ONCE(probe_result.idtr_nmi_base, idtr.address);
	WRITE_ONCE(probe_result.idtr_nmi_limit, idtr.size);

	if (type != NMI_LOCAL)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_TYPE;
	if (cpu != 0 || cpu != READ_ONCE(probe_result.cpu_before))
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_CPU;
	if (regs->ip < (unsigned long)x86_nmi_probe_window_begin ||
	    regs->ip >= (unsigned long)x86_nmi_probe_window_end)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_RIP;
	if (gpr_mask != X86_NMI_BOUNDARY_GPR_ALL)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_GPRS;
	if (regs->sp != READ_ONCE(x86_nmi_probe_expected_rsp))
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_RSP;
	if ((regs->flags & NMI_FLAGS_MASK) != NMI_EXPECTED_FLAGS)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_RFLAGS;
	if (idtr.address != READ_ONCE(probe_result.idtr_before_base) ||
	    idtr.size != READ_ONCE(probe_result.idtr_before_limit))
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_IDTR;
	if (current != READ_ONCE(probe_task) ||
	    current->pid != READ_ONCE(probe_result.task_pid_before) ||
	    probe_result.cr3_nmi != READ_ONCE(probe_result.cr3_before))
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_HOST_STATE;

	smp_wmb();
	WRITE_ONCE(x86_nmi_probe_done, 1);
	return NMI_HANDLED;
}

static void capture_after_state(void)
{
	struct desc_ptr idtr;
	int cpu;

	preempt_disable();
	cpu = raw_smp_processor_id();
	store_idt(&idtr);
	WRITE_ONCE(probe_result.cpu_after, cpu);
	WRITE_ONCE(probe_result.idtr_after_base, idtr.address);
	WRITE_ONCE(probe_result.idtr_after_limit, idtr.size);
	preempt_enable();
}

static void evaluate_result(bool check_syscall)
{
	if (atomic_read(&nmi_count) != 1)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_COUNT;
	if (atomic_read(&duplicate_count) != 0)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_DUPLICATE;
	if (atomic_read(&unexpected_count) != 0)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_UNEXPECTED;
	if (probe_result.cpu_before != 0 || probe_result.cpu_nmi != 0 ||
	    probe_result.cpu_after != 0)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_CPU;
	if (probe_result.callback_gpr_mask != X86_NMI_BOUNDARY_GPR_ALL ||
	    READ_ONCE(x86_nmi_probe_return_gpr_mask) != X86_NMI_BOUNDARY_GPR_ALL)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_GPRS;
	if (probe_result.expected_rsp == 0 ||
	    probe_result.nmi_rsp != probe_result.expected_rsp ||
	    !READ_ONCE(x86_nmi_probe_return_rsp_ok))
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_RSP;
	if ((probe_result.nmi_rflags & NMI_FLAGS_MASK) != NMI_EXPECTED_FLAGS ||
	    !READ_ONCE(x86_nmi_probe_return_rflags_ok))
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_RFLAGS;
	if (probe_result.idtr_before_base != probe_result.idtr_nmi_base ||
	    probe_result.idtr_before_base != probe_result.idtr_after_base ||
	    probe_result.idtr_before_limit != probe_result.idtr_nmi_limit ||
	    probe_result.idtr_before_limit != probe_result.idtr_after_limit)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_IDTR;
	if (probe_result.nmi_type != NMI_LOCAL)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_TYPE;
	if (probe_result.task_pid_before <= 0 ||
	    probe_result.task_pid_nmi != probe_result.task_pid_before ||
	    probe_result.task_pid_after != probe_result.task_pid_before ||
	    probe_result.cr3_before != probe_result.cr3_nmi ||
	    probe_result.cr3_before != probe_result.cr3_after)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_HOST_STATE;
	if (check_syscall &&
	    (probe_result.post_syscall_pid <= 0 || probe_result.post_syscall_cpu != 0))
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_SYSCALL;
}

static long run_probe(void __user *arg)
{
	unsigned long irq_flags;
	unsigned long rsp_before;
	unsigned long rsp_after;
	int cpu;
	int ret;

	if (READ_ONCE(run_started))
		return -EALREADY;

	memset(&probe_result, 0, sizeof(probe_result));
	probe_result.size = sizeof(probe_result);
	probe_result.version = X86_NMI_BOUNDARY_VERSION;
	probe_task = current;
	probe_result.task_pid_before = current->pid;
	probe_result.cr3_before = read_host_cr3();
	atomic_set(&nmi_count, 0);
	atomic_set(&duplicate_count, 0);
	atomic_set(&unexpected_count, 0);
	WRITE_ONCE(x86_nmi_probe_phase, NMI_PHASE_PREPARE);
	WRITE_ONCE(x86_nmi_probe_done, 0);
	WRITE_ONCE(x86_nmi_probe_expected_rsp, 0);
	WRITE_ONCE(x86_nmi_probe_expected_rbp, 0);
	WRITE_ONCE(x86_nmi_probe_return_rflags, 0);
	WRITE_ONCE(x86_nmi_probe_return_gpr_mask, 0);
	WRITE_ONCE(x86_nmi_probe_return_rsp_ok, 0);
	WRITE_ONCE(x86_nmi_probe_return_rflags_ok, 0);
	{
		struct desc_ptr idtr;

		store_idt(&idtr);
		probe_result.idtr_before_base = idtr.address;
		probe_result.idtr_before_limit = idtr.size;
	}

	if (cpu_feature_enabled(X86_FEATURE_FRED)) {
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_FRED;
		ret = -EOPNOTSUPP;
		goto copy_result;
	}
	if (!boot_cpu_has(X86_FEATURE_HYPERVISOR)) {
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_ENVIRONMENT;
		ret = -EPERM;
		goto copy_result;
	}

	ret = register_nmi_handler(NMI_LOCAL, nmi_boundary_handler, 0,
				    NMI_HANDLER_NAME);
	if (ret)
		goto copy_result;
	handler_registered = true;
	run_started = true;

	preempt_disable();
	cpu = raw_smp_processor_id();
	probe_result.cpu_before = cpu;
	if (cpu != 0) {
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_CPU;
		preempt_enable();
		ret = -EXDEV;
		goto copy_result;
	}

	local_irq_save(irq_flags);
	asm volatile("mov %%rsp, %0" : "=r"(rsp_before) :: "memory");
	x86_nmi_probe_wait();
	asm volatile("mov %%rsp, %0" : "=r"(rsp_after) :: "memory");
	local_irq_restore(irq_flags);
	probe_result.expected_rsp = READ_ONCE(x86_nmi_probe_expected_rsp);
	if (rsp_before != rsp_after)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_RSP;
	probe_result.task_pid_after = current->pid;
	probe_result.cr3_after = read_host_cr3();
	if (current != READ_ONCE(probe_task))
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_HOST_STATE;
	preempt_enable();

	WRITE_ONCE(x86_nmi_probe_phase, NMI_PHASE_DRAIN);
	msleep(50);
	capture_after_state();
	probe_result.nmi_count = atomic_read(&nmi_count);
	probe_result.duplicate_count = atomic_read(&duplicate_count);
	probe_result.unexpected_count = atomic_read(&unexpected_count);
	probe_result.return_gpr_mask = READ_ONCE(x86_nmi_probe_return_gpr_mask);
	probe_result.return_rsp_ok = READ_ONCE(x86_nmi_probe_return_rsp_ok);
	probe_result.return_rflags_ok = READ_ONCE(x86_nmi_probe_return_rflags_ok);
	evaluate_result(false);
	ret = copy_to_user(arg, &probe_result, sizeof(probe_result)) ? -EFAULT : 0;
	return ret;

copy_result:
	probe_result.nmi_count = atomic_read(&nmi_count);
	ret = copy_to_user(arg, &probe_result, sizeof(probe_result)) ? -EFAULT : ret;
	return ret;
}

static long finalize_probe(void __user *arg)
{
	struct x86_nmi_boundary_result user_result;
	int ret = 0;

	if (copy_from_user(&user_result, arg, sizeof(user_result)))
		return -EFAULT;
	if (!run_started || !handler_registered || run_finalized)
		return -EINVAL;

	probe_result.post_syscall_pid = user_result.post_syscall_pid;
	probe_result.post_syscall_cpu = user_result.post_syscall_cpu;
	if (probe_result.post_syscall_pid <= 0 || probe_result.post_syscall_cpu != 0)
		probe_result.failure_flags |= X86_NMI_BOUNDARY_FAIL_SYSCALL;

	msleep(50);
	WRITE_ONCE(x86_nmi_probe_phase, NMI_PHASE_DONE);
	unregister_nmi_handler(NMI_LOCAL, NMI_HANDLER_NAME);
	handler_registered = false;
	run_finalized = true;
	capture_after_state();
	probe_result.nmi_count = atomic_read(&nmi_count);
	probe_result.duplicate_count = atomic_read(&duplicate_count);
	probe_result.unexpected_count = atomic_read(&unexpected_count);
	evaluate_result(true);
	if (copy_to_user(arg, &probe_result, sizeof(probe_result)))
		ret = -EFAULT;
	else if (probe_result.failure_flags)
		ret = -EIO;
	return ret;
}

static long nmi_boundary_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	void __user *user_arg = (void __user *)arg;
	long ret;

	if (_IOC_TYPE(cmd) != X86_NMI_BOUNDARY_IOC_MAGIC)
		return -ENOTTY;
	if (mutex_lock_interruptible(&probe_mutex))
		return -ERESTARTSYS;
	if (cmd == X86_NMI_BOUNDARY_IOC_RUN)
		ret = run_probe(user_arg);
	else if (cmd == X86_NMI_BOUNDARY_IOC_FINALIZE)
		ret = finalize_probe(user_arg);
	else
		ret = -ENOTTY;
	mutex_unlock(&probe_mutex);
	return ret;
}

static const struct file_operations nmi_boundary_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = nmi_boundary_ioctl,
};

static struct miscdevice nmi_boundary_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &nmi_boundary_fops,
	.mode = 0600,
};

static int __init nmi_boundary_init(void)
{
	return misc_register(&nmi_boundary_device);
}

static void __exit nmi_boundary_exit(void)
{
	misc_deregister(&nmi_boundary_device);
	if (handler_registered) {
		WRITE_ONCE(x86_nmi_probe_phase, NMI_PHASE_DONE);
		unregister_nmi_handler(NMI_LOCAL, NMI_HANDLER_NAME);
		handler_registered = false;
	}
}

module_init(nmi_boundary_init);
module_exit(nmi_boundary_exit);

MODULE_DESCRIPTION("Disposable Linux x86_64 NMI boundary probe");
MODULE_LICENSE("GPL");
