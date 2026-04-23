#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "vm.h"
#include "queue.h"
#include "timer.h"

struct proc pool[NPROC];
__attribute__((aligned(16))) char kstack[NPROC][PAGE_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][TRAP_PAGE_SIZE];

extern char boot_stack_top[];
struct proc *current_proc;
struct proc idle;
struct queue task_queue;

int threadid()
{
	return curr_proc()->pid;
}

struct proc *curr_proc()
{
	return current_proc;
}

static uint64 calc_pass(int priority)
{
	if (priority < 2)
		priority = 16;
	return BIG_STRIDE / (uint64)priority; //Q Project 3 stride formula
}

void proc_init()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		p->state = UNUSED;
		p->kstack = (uint64)kstack[p - pool];
		p->ustack = 0;
		p->trapframe = (struct trapframe *)trapframe[p - pool];
		p->pagetable = 0;
		p->max_page = 0;
		p->parent = NULL;
		p->exit_code = 0;
		p->start_cycle = 0;
		p->start_cycle_inited = 0;
		memset(p->syscall_times, 0, sizeof(p->syscall_times));
		p->stride = 0; //Q Project 3 initial stride
		p->priority = 16; //Q Project 3 initial priority
		p->pass = calc_pass(p->priority); //Q Project 3 pass value
	}

	idle.kstack = (uint64)boot_stack_top;
	idle.ustack = 0;
	idle.trapframe = 0;
	idle.pagetable = 0;
	idle.max_page = 0;
	idle.parent = NULL;
	idle.exit_code = 0;
	idle.pid = IDLE_PID;
	idle.start_cycle = 0;
	idle.start_cycle_inited = 0;
	memset(idle.syscall_times, 0, sizeof(idle.syscall_times));
	idle.stride = 0;
	idle.priority = 16;
	idle.pass = calc_pass(idle.priority);

	current_proc = &idle;
	init_queue(&task_queue);
}

int allocpid()
{
	static int PID = 1;
	return PID++;
}

struct proc *pop_task()
{
	return 0;
}

void add_task(struct proc *p)
{
	if (p == 0)
		return;
	if (p->state != UNUSED && p->state != ZOMBIE)
		p->state = RUNNABLE;
}

struct proc *allocproc()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		if (p->state == UNUSED)
			goto found;
	}
	return 0;

found:
	p->pid = allocpid();
	p->state = USED;
	p->ustack = 0;
	p->max_page = 0;
	p->parent = NULL;
	p->exit_code = 0;
	p->pagetable = uvmcreate((uint64)p->trapframe);
	memset(&p->context, 0, sizeof(p->context));
	memset((void *)p->kstack, 0, KSTACK_SIZE);
	memset((void *)p->trapframe, 0, TRAP_PAGE_SIZE);
	p->context.ra = (uint64)usertrapret;
	p->context.sp = p->kstack + KSTACK_SIZE;
	p->start_cycle = 0;
	p->start_cycle_inited = 0;
	memset(p->syscall_times, 0, sizeof(p->syscall_times));
	p->stride = 0; //Q Project 3 initial stride
	p->priority = 16; //Q Project 3 initial priority
	p->pass = calc_pass(p->priority); //Q Project 3 pass value
	return p;
}

void scheduler()
{
	struct proc *p;
	struct proc *best;

	for (;;) {
		best = 0;

		for (p = pool; p < &pool[NPROC]; p++) {
			if (p->state != RUNNABLE)
				continue;

			if (best == 0 || p->stride < best->stride ||
			    (p->stride == best->stride && p->pid < best->pid)) {
				best = p; //Q Project 3 choose runnable process with smallest stride
			}
		}

		if (best == 0)
			panic("all app are over!\n");

		if (!best->start_cycle_inited) {
			best->start_cycle = get_cycle();
			best->start_cycle_inited = 1;
		}

		best->state = RUNNING;
		current_proc = best;
		best->stride += best->pass; //Q Project 3 advance stride after scheduling
		swtch(&idle.context, &best->context);
	}
}

void sched()
{
	struct proc *p = curr_proc();
	if (p->state == RUNNING)
		panic("sched running");
	swtch(&p->context, &idle.context);
}

void yield()
{
	current_proc->state = RUNNABLE;
	sched();
}

void freepagetable(pagetable_t pagetable, uint64 max_page)
{
	uvmunmap(pagetable, TRAMPOLINE, 1, 0);
	uvmunmap(pagetable, TRAPFRAME, 1, 0);
	uvmfree(pagetable, max_page);
}

void freeproc(struct proc *p)
{
	if (p->pagetable)
		freepagetable(p->pagetable, p->max_page);

	p->pagetable = 0;
	p->ustack = 0;
	p->max_page = 0;
	p->parent = NULL;
	p->exit_code = 0;
	p->start_cycle = 0;
	p->start_cycle_inited = 0;
	memset(p->syscall_times, 0, sizeof(p->syscall_times));
	p->stride = 0; //Q Project 3 reset stride data
	p->priority = 16; //Q Project 3 reset priority
	p->pass = calc_pass(p->priority); //Q Project 3 reset pass
	p->state = UNUSED;
}

int fork()
{
	struct proc *np;
	struct proc *p = curr_proc();

	if ((np = allocproc()) == 0)
		return -1;

	if (uvmcopy(p->pagetable, np->pagetable, p->max_page) < 0) {
		freeproc(np);
		return -1;
	}

	np->max_page = p->max_page;
	*(np->trapframe) = *(p->trapframe);
	np->trapframe->a0 = 0;
	np->parent = p;
	np->stride = 0; //Q Project 3 child starts at stride 0
	np->priority = p->priority; //Q Project 3 child inherits priority
	np->pass = calc_pass(np->priority); //Q Project 3 recompute child pass
	np->state = RUNNABLE;
	add_task(np);
	return np->pid;
}

int exec(char *name)
{
	int id = get_id_by_name(name);
	if (id < 0)
		return -1;

	struct proc *p = curr_proc();
	uvmunmap(p->pagetable, 0, p->max_page, 1);
	p->max_page = 0;
	p->ustack = 0;
	p->start_cycle = 0;
	p->start_cycle_inited = 0;
	memset(p->syscall_times, 0, sizeof(p->syscall_times));
	p->stride = 0; //Q Project 3 reset stride on exec
	p->pass = calc_pass(p->priority); //Q Project 3 recompute pass on exec

	loader(id, p);
	return 0;
}

int wait(int pid, int *code)
{
	struct proc *np;
	int havekids;
	struct proc *p = curr_proc();

	for (;;) {
		havekids = 0;
		for (np = pool; np < &pool[NPROC]; np++) {
			if (np->state != UNUSED && np->parent == p &&
			    (pid <= 0 || np->pid == pid)) {
				havekids = 1;
				if (np->state == ZOMBIE) {
					pid = np->pid;
					*code = np->exit_code;
					freeproc(np);
					return pid;
				}
			}
		}
		if (!havekids)
			return -1;

		p->state = RUNNABLE;
		sched();
	}
}

void exit(int code)
{
	struct proc *p = curr_proc();
	struct proc *np;

	p->exit_code = code;
	debugf("proc %d exit with %d\n", p->pid, code);

	for (np = pool; np < &pool[NPROC]; np++) {
		if (np->parent == p)
			np->parent = NULL;
	}

	if (p->parent != NULL) {
		if (p->pagetable) {
			freepagetable(p->pagetable, p->max_page);
			p->pagetable = 0;
			p->ustack = 0;
			p->max_page = 0;
		}
		p->start_cycle = 0;
		p->start_cycle_inited = 0;
		memset(p->syscall_times, 0, sizeof(p->syscall_times));
		p->stride = 0; //Q Project 3 clear stride data on child exit
		p->priority = 16; //Q Project 3 reset priority on child exit
		p->pass = calc_pass(p->priority); //Q Project 3 reset pass on child exit
		p->state = ZOMBIE;
	} else {
		freeproc(p);
	}

	sched();
}