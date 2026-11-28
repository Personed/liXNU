// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/prctl.h>
#include <linux/seccomp.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef PR_SET_SYSCALL_USER_DISPATCH
#define PR_SET_SYSCALL_USER_DISPATCH 59
#endif
#ifndef PR_SYS_DISPATCH_ON
#define PR_SYS_DISPATCH_ON 1
#endif
#ifndef PR_SYS_DISPATCH_OFF
#define PR_SYS_DISPATCH_OFF 0
#endif
#ifndef SYSCALL_DISPATCH_FILTER_ALLOW
#define SYSCALL_DISPATCH_FILTER_ALLOW 0
#endif
#ifndef SYSCALL_DISPATCH_FILTER_BLOCK
#define SYSCALL_DISPATCH_FILTER_BLOCK 1
#endif
#ifndef SYS_USER_DISPATCH
#define SYS_USER_DISPATCH 2
#endif

#define X86_SYS_getpid 39
#define FOREIGN_BSD_WRITE_NR 0x2000004UL
#define SECCOMP_MAGIC_RESULT 0x516d6564

typedef long (*syscall_page_fn)(void);

static volatile char sud_selector = SYSCALL_DISPATCH_FILTER_BLOCK;
static volatile sig_atomic_t sud_count;
static volatile sig_atomic_t sud_nr;
static volatile sig_atomic_t sud_code;
static volatile uintptr_t sud_addr;
static sigjmp_buf sud_escape;
static volatile long sud_direct_pid;

static void emit_syscall_fn(unsigned char *page, unsigned int nr)
{
	/* mov $nr,%eax ; syscall ; ret */
	page[0] = 0xb8;
	memcpy(page + 1, &nr, sizeof(nr));
	page[5] = 0x0f;
	page[6] = 0x05;
	page[7] = 0xc3;
}

static void sud_handler(int sig, siginfo_t *info, void *uctx)
{
	(void)uctx;
	if (sig != SIGSYS || info->si_code != SYS_USER_DISPATCH)
		_exit(90);
	sud_count++;
	sud_code = info->si_code;
	sud_nr = info->si_syscall;
	sud_addr = (uintptr_t)info->si_call_addr;
	sud_selector = SYSCALL_DISPATCH_FILTER_ALLOW;
	siglongjmp(sud_escape, 1);
}

static int test_syscall_user_dispatch(void)
{
	long page_size = sysconf(_SC_PAGESIZE);
	unsigned char *allowed;
	unsigned char *blocked;
	struct sigaction action;
	long expected_pid = getpid();
	int jumped = 0;

	if (page_size <= 0)
		return 10;
	allowed = mmap(NULL, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	blocked = mmap(NULL, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (allowed == MAP_FAILED || blocked == MAP_FAILED) {
		perror("mmap");
		return 11;
	}
	emit_syscall_fn(allowed, X86_SYS_getpid);
	emit_syscall_fn(blocked, FOREIGN_BSD_WRITE_NR);

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = sud_handler;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSYS, &action, NULL) != 0) {
		perror("sigaction");
		return 12;
	}

	sud_selector = SYSCALL_DISPATCH_FILTER_BLOCK;
	if (prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON,
		  (unsigned long)allowed, (unsigned long)page_size,
		  (void *)&sud_selector, 0) != 0) {
		if (errno == EINVAL || errno == ENOSYS) {
			fprintf(stderr, "SKIP: syscall user dispatch unavailable\n");
			return 77;
		}
		perror("prctl(PR_SET_SYSCALL_USER_DISPATCH)");
		return 13;
	}

	if (sigsetjmp(sud_escape, 0) == 0) {
		sud_direct_pid = ((syscall_page_fn)allowed)();
		if (sud_direct_pid != expected_pid || sud_count != 0)
			return 14;
		(void)((syscall_page_fn)blocked)();
		return 15;
	}
	jumped = 1;

	if (!jumped || sud_count != 1 || sud_code != SYS_USER_DISPATCH)
		return 16;
	if ((unsigned long)sud_nr != FOREIGN_BSD_WRITE_NR) {
		fprintf(stderr, "SUD_NR_MISMATCH got=0x%lx want=0x%lx\n", (unsigned long)sud_nr, FOREIGN_BSD_WRITE_NR);
		return 17;
	}
	if (sud_addr < (uintptr_t)blocked || sud_addr >= (uintptr_t)blocked + page_size)
		return 18;

	sud_selector = SYSCALL_DISPATCH_FILTER_ALLOW;
	if (prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_OFF, 0, 0, 0, 0) != 0)
		return 19;
	printf("SUD_PASS nr=0x%lx direct_pid=%ld\n", (unsigned long)sud_nr, sud_direct_pid);
	return 0;
}

static int install_seccomp_listener(unsigned long foreign_nr)
{
	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, foreign_nr, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {
		.len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
		.filter = filter,
	};

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
		return -1;
	return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
		       SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
}

static long raw_syscall0(unsigned long nr)
{
	long ret;
	register long r10 __asm__("r10") = 0;
	register long r8 __asm__("r8") = 0;
	register long r9 __asm__("r9") = 0;

	__asm__ volatile("syscall"
			 : "=a"(ret)
			 : "a"(nr), "D"(0), "S"(0), "d"(0), "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return ret;
}

static int test_seccomp_user_notification(void)
{
	int listener;
	pid_t child;
	int status;
	struct seccomp_notif request;
	struct seccomp_notif_resp response;

	listener = install_seccomp_listener(FOREIGN_BSD_WRITE_NR);
	if (listener < 0) {
		perror("seccomp listener");
		return 30;
	}

	child = fork();
	if (child < 0)
		return 31;
	if (child == 0) {
		long ret = raw_syscall0(FOREIGN_BSD_WRITE_NR);
		_exit(ret == SECCOMP_MAGIC_RESULT ? 0 : 40);
	}

	memset(&request, 0, sizeof(request));
	if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &request) != 0) {
		perror("SECCOMP_IOCTL_NOTIF_RECV");
		return 32;
	}
	if (request.data.nr != FOREIGN_BSD_WRITE_NR || (pid_t)request.pid != child)
		return 33;

	memset(&response, 0, sizeof(response));
	response.id = request.id;
	response.val = SECCOMP_MAGIC_RESULT;
	if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &response) != 0) {
		perror("SECCOMP_IOCTL_NOTIF_SEND");
		return 34;
	}
	if (waitpid(child, &status, 0) != child)
		return 35;
	close(listener);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return 36;
	printf("SECCOMP_USER_NOTIF_PASS nr=0x%lx supervised_result=0x%x\n",
	       (unsigned long)request.data.nr, SECCOMP_MAGIC_RESULT);
	return 0;
}

int main(void)
{
	int rc;

	rc = test_syscall_user_dispatch();
	if (rc != 0)
		return rc;
	rc = test_seccomp_user_notification();
	if (rc != 0)
		return rc;
	puts("X86_MEDIATION_ENABLERS_PASS");
	return 0;
}
