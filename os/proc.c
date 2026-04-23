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

int cpuid()
{
	return 0; //Q CH6: current labs run with a single CPU/hart, so hart id is always 0
}

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
	return BIG_STRIDE / (uint64)priority; 
}

int fdalloc(struct file *f)
{
	struct proc *p = curr_proc(); //Q CH6: install a file into the current process fd table

	for (int i = 3; i < FD_BUFFER_SIZE; ++i) { //Q CH6: reserve 0,1,2 for stdio
		if (p->files[i] == 0) { //Q CH6: first empty descriptor slot wins
			p->files[i] = f; //Q CH6: bind file discriptor to system-level file object
			return i; //Q CH6: return allocated file descriptor number
		}
	}
	return -1; //Q CH6: no free fd slot available in this process
}

static void init_stdio(struct proc *p) //Q CH6: every fresh process needs stdin/stdout/stderr open
{
	p->files[STDIN] = stdio_init(STDIN); //Q CH6: attach console-backed stdin
	p->files[STDOUT] = stdio_init(STDOUT); //Q CH6: attach console-backed stdout
	p->files[STDERR] = stdio_init(STDERR); //Q CH6: attach console-backed stderr
}

static void close_all_files(struct proc *p) //Q CH6: helper to properly drop all file refs held by a process
{
	for (int i = 0; i < FD_BUFFER_SIZE; ++i) {
		if (p->files[i]) {
			fileclose(p->files[i]); //Q CH6: release file object reference instead of leaking it
			p->files[i] = 0; //Q CH6: clear process file discriptor slot after close
		}
	}
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
		for (int i = 0; i < FD_BUFFER_SIZE; ++i) //Q CH6: clear per-process file discriptor table
			p->files[i] = 0;
		p->start_cycle = 0; 
		p->start_cycle_inited = 0; 
		memset(p->syscall_times, 0, sizeof(p->syscall_times)); 
		p->stride = 0; 
		p->priority = 16; 
		p->pass = calc_pass(p->priority); 
	}

	idle.kstack = (uint64)boot_stack_top;
	idle.ustack = 0;
	idle.trapframe = 0;
	idle.pagetable = 0;
	idle.max_page = 0;
	idle.parent = NULL; 
	idle.exit_code = 0; 
	for (int i = 0; i < FD_BUFFER_SIZE; ++i) //Q CH6: idle process has no open files
		idle.files[i] = 0;
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
	for (int i = 0; i < FD_BUFFER_SIZE; ++i) //Q CH6: start with no open files before stdio init
		p->files[i] = 0;
	init_stdio(p); //Q 
	p->pagetable = uvmcreate((uint64)p->trapframe); 
	memset(&p->context, 0, sizeof(p->context));
	memset((void *)p->kstack, 0, KSTACK_SIZE);
	memset((void *)p->trapframe, 0, TRAP_PAGE_SIZE);
	p->context.ra = (uint64)usertrapret;
	p->context.sp = p->kstack + KSTACK_SIZE;
	p->start_cycle = 0; 
	p->start_cycle_inited = 0; 
	memset(p->syscall_times, 0, sizeof(p->syscall_times));
	p->stride = 0; 
	p->priority = 16; 
	p->pass = calc_pass(p->priority); 
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
				best = p; 
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
		best->stride += best->pass; 
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
	close_all_files(p); //Q CH6: close all descriptors instead of silently dropping them
	p->start_cycle = 0; 
	p->start_cycle_inited = 0; 
	memset(p->syscall_times, 0, sizeof(p->syscall_times)); 
	p->stride = 0; 
	p->priority = 16; 
	p->pass = calc_pass(p->priority); 
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

	close_all_files(np); //Q CH6: discard default stdio refs before inheriting parent's actual descriptor table
	for (int i = 0; i < FD_BUFFER_SIZE; ++i) { //Q CH6: fork should inherit all open file descriptors
		np->files[i] = p->files[i];
		if (np->files[i])
			np->files[i]->ref++; //Q CH6: inherited descriptors share the same system file object
	}

	np->stride = 0; 
	np->priority = p->priority; 
	np->pass = calc_pass(np->priority); 
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
	p->stride = 0; 
	p->pass = calc_pass(p->priority); 

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
		close_all_files(p); //Q CH6: release all open files when process exits
		p->start_cycle = 0; 
		p->start_cycle_inited = 0; 
		memset(p->syscall_times, 0, sizeof(p->syscall_times)); 
		p->stride = 0; 
		p->priority = 16; 
		p->pass = calc_pass(p->priority); 
		p->state = ZOMBIE; 
	} else {
		freeproc(p);
	}

	sched();
}