#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 sys_write(int fd, char *str, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, str, len);
	if (fd != STDOUT)
		return -1;
	for (int i = 0; i < len; ++i) {
		console_putchar(str[i]);
	}
	return len;
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

uint64 sys_gettimeofday(TimeVal *val, int _tz)
{
	uint64 cycle = get_cycle();
	val->sec = cycle / CPU_FREQ;
	val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	return 0;
}
/*
* LAB1: you may need to define sys_task_info here
*/
uint64 sys_getpid()   // Q: Returns the PID of the current process.
{
	return (uint64)curr_proc()->pid; // Q: from the PCB.
}
#ifndef MAX_SYSCALL_NUM                 
#define MAX_SYSCALL_NUM 500 
#endif           
//Line 49-60 were pulled from user/stddef.h to match layout 
typedef enum {   // Kernel copy of TaskStatus matching user/stddef.h order.
	UnInit,     // Uninitialized.
	Ready,      
	Running,              
	Exited,                             
} TaskStatus; // Status enum for TaskInfo.

typedef struct {   
	TaskStatus status;  
	unsigned int syscall_times[MAX_SYSCALL_NUM]; // Syscall counters cant be negitive.
	int time;    
} TaskInfo;      // Returned by SYS_taskinfo.

static uint64 sys_task_info(TaskInfo *ti) // Q: Fill TaskInfo for the current task.
{ 
	struct proc *p = curr_proc();  // Q: Current process.
	ti->status = Running;   // Q: Current task is running when it calls syscall.

	for (int i = 0; i < MAX_SYSCALL_NUM; i++)   // Q: Copy syscall counters to user struct.
		ti->syscall_times[i] = p->syscall_times[i]; // Q: Mirror count

	uint64 now = get_cycle();   // Q: Current cycle count.
	if (!p->start_cycle_inited) { // Q: if not initialized, initialize here too.
		p->start_cycle = now;  // Q: Set start cycle.
		p->start_cycle_inited = 1;  // Q: Mark initialized.
	}
	uint64 delta = now - p->start_cycle; // Q: count Cycles since first run.
	ti->time = (int)(delta * 1000 / CPU_FREQ); // Q: Convert cycles to milliseconds.

	return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/
	if (id >= 0 && id < MAX_SYSCALL_NUM) // Q: Only count syscall IDs in range.
		curr_proc()->syscall_times[id]++;  // Q: Increment per-task counter (includes SYS_taskinfo itself).

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], (char *)args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday((TimeVal *)args[0], args[1	]);
		break;
	case SYS_getpid:              // Q: Handle syscall 172 so tests stop failing.
		ret = (int)sys_getpid();  // Q: Return pid in a0.
		break;
	/*
	* LAB1: you may need to add SYS_taskinfo case here
	*/
//This is the “router” that connects the syscall number 410 to the kernel code that fills a TaskInfo struct for the current process.
	case SYS_taskinfo: // Q: Handle syscall 410 for task info.
		ret = (int)sys_task_info((TaskInfo *)args[0]); // Q: Fill TaskInfo at user pointer.
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
