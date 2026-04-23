#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"
#include "vm.h"
#include "riscv.h"

#ifndef MAX_SYSCALL_NUM
#define MAX_SYSCALL_NUM 500
#endif

#define MAX_MMAP_LEN (1UL << 30)

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	if (size < 0)
		return -1;
	for (int i = 0; i < size; ++i)
		console_putchar(str[i]);
	return size;
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDIN)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	uint64 n = MIN(len, MAX_STR_LEN);
	for (uint64 i = 0; i < n; ++i)
		str[i] = consgetc();
	if (copyout(p->pagetable, va, str, n) < 0)
		return -1;
	return n;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	(void)_tz;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	if (copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal)) < 0)
		return -1;
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	return fork();
}

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	if (copyinstr(p->pagetable, name, va, 200) < 0)
		return -1;
	return exec(name);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	if (code == 0)
		return -1;
	return wait(pid, code);
}

static int mmap_perm_from_port(uint64 port)
{
	int perm = PTE_U;
	if (port & 0x1)
		perm |= PTE_R;
	if (port & 0x2)
		perm |= PTE_W;
	if (port & 0x4)
		perm |= PTE_X;
	return perm;
}

uint64 sys_mmap(uint64 start, uint64 len, uint64 port, uint64 flag, uint64 fd)
{
	uint64 end;
	int perm;
	struct proc *p = curr_proc();
	(void)flag;
	(void)fd;

	if (len == 0)
		return 0;

	if (len > MAX_MMAP_LEN)
		return -1;

	if ((start % PGSIZE) != 0)
		return -1;

	if ((port & ~0x7UL) != 0)
		return -1;

	if ((port & 0x7UL) == 0)
		return -1;

	end = PGROUNDUP(start + len);
	if (end < start)
		return -1;

	for (uint64 va = start; va < end; va += PGSIZE) {
		if (walkaddr(p->pagetable, va) != 0)
			return -1;
	}

	perm = mmap_perm_from_port(port);

	for (uint64 va = start; va < end; va += PGSIZE) {
		void *mem = kalloc();
		if (mem == 0) {
			uvmunmap(p->pagetable, start, (va - start) / PGSIZE, 1);
			return -1;
		}

		memset(mem, 0, PGSIZE);

		if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) < 0) {
			kfree(mem);
			uvmunmap(p->pagetable, start, (va - start) / PGSIZE, 1);
			return -1;
		}
	}

	if (end / PGSIZE > p->max_page)
		p->max_page = end / PGSIZE;

	return 0;
}

uint64 sys_munmap(uint64 start, uint64 len)
{
	uint64 end, npages;
	struct proc *p = curr_proc();

	if (len == 0)
		return 0;

	if (len > MAX_MMAP_LEN)
		return -1;

	if ((start % PGSIZE) != 0)
		return -1;

	end = PGROUNDUP(start + len);
	if (end < start)
		return -1;

	npages = (end - start) / PGSIZE;

	for (uint64 va = start; va < end; va += PGSIZE) {
		if (walkaddr(p->pagetable, va) == 0)
			return -1;
	}

	uvmunmap(p->pagetable, start, npages, 1);
	return 0;
}

uint64 sys_spawn(uint64 va)
{
	struct proc *parent = curr_proc(); //Q Project 3 parent process for spawned child
	struct proc *np; //Q Project 3 spawned child process
	char name[200]; //Q Project 3 target program name buffer
	int id; //Q Project 3 app id

	if (copyinstr(parent->pagetable, name, va, 200) < 0)
		return -1; //Q Project 3 invalid filename pointer

	id = get_id_by_name(name); //Q Project 3 resolve target program
	if (id < 0)
		return -1; //Q Project 3 invalid filename

	np = allocproc(); //Q Project 3 allocate child process
	if (np == 0)
		return -1; //Q Project 3 allocation failure

	np->parent = parent; //Q Project 3 parent-child relationship
	np->stride = 0; //Q Project 3 initial stride
	np->priority = 16; //Q Project 3 initial priority
	np->pass = BIG_STRIDE / 16; //Q Project 3 initial pass value
	np->start_cycle = 0;
	np->start_cycle_inited = 0;
	memset(np->syscall_times, 0, sizeof(np->syscall_times));

	if (loader(id, np) < 0) { //Q Project 3 load target program into child
		freeproc(np); //Q Project 3 cleanup on spawn failure
		return -1;
	}

	np->state = RUNNABLE; //Q Project 3 spawned child becomes runnable
	add_task(np); //Q Project 3 admit spawned child to scheduler
	return np->pid; //Q Project 3 return child pid on success
}

uint64 sys_set_priority(long long prio)
{
	struct proc *p = curr_proc();

	if (prio < 2)
		return -1; //Q Project 3 reject invalid priority

	p->priority = (int)prio; //Q Project 3 set current process priority
	p->pass = BIG_STRIDE / (uint64)p->priority; //Q Project 3 recompute pass from priority
	return prio; //Q Project 3 return prio on success
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = {
		trapframe->a0, trapframe->a1, trapframe->a2,
		trapframe->a3, trapframe->a4, trapframe->a5
	};

	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);

	if (id >= 0 && id < MAX_SYSCALL_NUM)
		curr_proc()->syscall_times[id]++;

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone:
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	
	case SYS_spawn:
		ret = sys_spawn(args[0]); //Q Project 3 spawn syscall handler
		break;
	case SYS_setpriority:
		ret = sys_set_priority(args[0]); //Q Project 3 set_priority syscall handler
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}