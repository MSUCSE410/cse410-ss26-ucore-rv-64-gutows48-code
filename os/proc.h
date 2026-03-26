#ifndef PROC_H
#define PROC_H

#include "types.h"
#include "vm.h" 

#define NPROC (16)
#define MAX_SYSCALL_NUM 500

// Saved registers for kernel context switches.
struct context {
	uint64 ra;
	uint64 sp;

	uint64 s0;
	uint64 s1;
	uint64 s2;
	uint64 s3;
	uint64 s4;
	uint64 s5;
	uint64 s6;
	uint64 s7;
	uint64 s8;
	uint64 s9;
	uint64 s10;
	uint64 s11;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
	enum procstate state;
	int pid;
	uint64 ustack;
	uint64 kstack;
	struct trapframe *trapframe;
	struct context context;

	pagetable_t pagetable; //Q store current process user page table 
	uint64 max_page; //Q tracks page size 

	uint32 syscall_times[MAX_SYSCALL_NUM]; //Q counts system calls
	uint64 start_cycle; //stores cycle count when process starts 
	int start_cycle_inited; //keeps track for kernal if start cycle has been set
};

struct proc *curr_proc();
void exit(int);
void proc_init();
void scheduler() __attribute__((noreturn));
void sched();
void yield();
struct proc *allocproc();
void swtch(struct context *, struct context *);

#endif // PROC_H