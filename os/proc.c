#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "timer.h"

struct proc pool[NPROC];
char kstack[NPROC][PAGE_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][PAGE_SIZE];
/* char ustack[NPROC][PAGE_SIZE]; */ //Q no prebuilt user stacks in ch4 VM loader

extern char boot_stack_top[];
struct proc *current_proc;
struct proc idle;

int threadid()
{
	return curr_proc()->pid;
}

struct proc *curr_proc()
{
	return current_proc;
}

// initialize the proc table at boot time.
void proc_init(void)
{
	struct proc *p; //Q 30-39 just initallizes all counts to 0
	for (p = pool; p < &pool[NPROC]; p++) {
		p->state = UNUSED;
		p->kstack = (uint64)kstack[p - pool];
		p->ustack = 0; //Q user stack VA is assigned later by the ch4 loader
		p->trapframe = (struct trapframe *)trapframe[p - pool];
		p->pagetable = 0; 
		p->max_page = 0; 
		p->start_cycle = 0;
		p->start_cycle_inited = 0;
		memset(p->syscall_times, 0, sizeof(p->syscall_times)); //Q clears all system counters
	}
	//Q 42-47 clears user memory
	//Q initialize the idle process's kernel stack to the boot stack
	idle.kstack = (uint64)boot_stack_top;

	//Q idle is not a normal user process, so it has no user stack
	idle.ustack = 0;

	//Q idle does not use a user trapframe
	idle.trapframe = 0;

	//Q idle has no user page table
	idle.pagetable = 0;

	//Q idle has no mapped user pages to track
	idle.max_page = 0;

	//Q reserve pid 0 for the idle process
	idle.pid = 0;

	//Q set the currently running process pointer to idle at boot
	current_proc = &idle;
}

int allocpid()
{
	static int PID = 1;
	return PID++;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel.
// If there are no free procs, or a memory allocation fails, return 0.
struct proc *allocproc(void)
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		if (p->state == UNUSED) {
			goto found;
		}
	}
	return 0;

found:
	p->pid = allocpid();
	p->state = USED;
	memset(&p->context, 0, sizeof(p->context));
	memset(p->trapframe, 0, PAGE_SIZE);
	memset((void *)p->kstack, 0, PAGE_SIZE);
	p->context.ra = (uint64)usertrapret; //Q ra is saved kernal address 	
	p->context.sp = p->kstack + PAGE_SIZE; //Q sp is stack pointer 
	p->ustack = 0; //Q assigned by bin_loader in ch4 ustack is user address 
	p->pagetable = 0; //Q no page table yet
	p->max_page = 0; //Q tracks user virtual mem helpful for freeing mem
	p->start_cycle = 0; //Q cycle count 
	p->start_cycle_inited = 0; //Q cycle initiated 
	memset(p->syscall_times, 0, sizeof(p->syscall_times));
	return p;
}

// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void)
{
	struct proc *p;
	for (;;) {
		for (p = pool; p < &pool[NPROC]; p++) {
			if (p->state == RUNNABLE) {
				if (!p->start_cycle_inited) { //Q if a process hasnt been started record start time
					p->start_cycle = get_cycle();
					p->start_cycle_inited = 1;
				}

				p->state = RUNNING;
				current_proc = p;
				swtch(&idle.context, &p->context);
			}
		}
	}
}

// Switch to scheduler.
void sched(void)
{
	struct proc *p = curr_proc();
	if (p->state == RUNNING)
		panic("sched running");
	swtch(&p->context, &idle.context);
}

// Give up the CPU for one scheduling round.
void yield(void)
{
	current_proc->state = RUNNABLE;
	sched();
}

// Exit the current process.
void exit(int code)
{
	struct proc *p = curr_proc();
	infof("proc %d exit with %d", p->pid, code);

	if (p->pagetable != 0) { //Q when process is over free table space 
		uvmfree(p->pagetable, p->max_page); //Q need this because its vm state now q
		p->pagetable = 0; 
		p->max_page = 0; 
	} 

	p->ustack = 0; //Q
	p->state = UNUSED;
	finished();
	sched();
}