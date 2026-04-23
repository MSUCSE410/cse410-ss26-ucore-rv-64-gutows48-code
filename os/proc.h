#ifndef PROC_H
#define PROC_H

#include "riscv.h"
#include "types.h"
#include "queue.h"

#ifndef MAX_SYSCALL_NUM
#define MAX_SYSCALL_NUM 500 //Q define syscall counter array size before struct proc uses it
#endif

#define NPROC (512)
#define FD_BUFFER_SIZE (16)
#define BIG_STRIDE (0x7fffffffUL) //Q large constant used for stride scheduling: pass = BIG_STRIDE / priority

struct file;

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

struct proc {
	enum procstate state;
	int pid;
	pagetable_t pagetable;
	uint64 ustack;
	uint64 kstack;
	struct trapframe *trapframe;
	struct context context;
	uint64 max_page;
	struct proc *parent;
	uint64 exit_code;
	struct file *files[FD_BUFFER_SIZE];

	uint64 start_cycle; //Q stores the cycle count when the process first begins running
	int start_cycle_inited; //Q tracks whether start_cycle has already been initialized
	uint64 syscall_times[MAX_SYSCALL_NUM]; //Q counts how many times each syscall is used by this process

	uint64 stride; //Q current accumulated stride value used by stride scheduling
	uint64 pass; //Q amount added to stride every time this process is scheduled
	int priority; //Q process priority; project says initial priority should be 16
};

int cpuid();
int threadid(); //Q helper declaration returning current process pid
struct proc *curr_proc();

void exit(int);
void proc_init();
void scheduler() __attribute__((noreturn));
void sched();
void yield();

int fork();
int exec(char *);
int wait(int, int *);

void add_task(struct proc *);
struct proc *pop_task();
struct proc *allocproc();
void freeproc(struct proc *p); //Q declaration so syscall.c can free a failed spawned process
int fdalloc(struct file *);

void swtch(struct context *, struct context *);

#endif