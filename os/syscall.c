#include "syscall.h"
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

#define MAX_MMAP_LEN (1UL << 30) //Q set max length to 1 GiB per requirments

static int copy_to_user_by_useraddr(uint64 dstva, const void *src, uint64 len)
{
	pagetable_t pagetable = curr_proc()->pagetable;
	const char *s = (const char *)src;

	for (uint64 i = 0; i < len; i++) {
		uint64 dst = useraddr(pagetable, dstva + i);
		if (dst == 0)
			return -1;
		*(char *)dst = s[i];
	}
	return 0;
}

uint64 sys_write(int fd, char *str, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, str, len);

	if (fd != STDOUT)
		return -1;

	pagetable_t pagetable = curr_proc()->pagetable;

	for (uint i = 0; i < len; i++) {
		uint64 src = useraddr(pagetable, (uint64)str + i);
		if (src == 0)
			return -1;
		console_putchar(*(char *)src);
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

uint64 sys_gettimeofday(TimeVal *val, int _tz) //Q returns syscall return value
{
	(void)_tz; //Q not used

	TimeVal kv; 
	uint64 cycle = get_cycle();

	kv.sec = cycle / CPU_FREQ; //Q converts cycles into seconds 
	kv.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ; 
	//Q *1000000 converts into micro seconds 

	if (copy_to_user_by_useraddr((uint64)val, &kv, sizeof(kv)) < 0) 
	//Q ^copies kv to val and converts to address value 
		return -1;

	return 0;
}

uint64 sys_getpid()
{
	return (uint64)curr_proc()->pid;
}

typedef enum { //Q defines task states 
	UnInit,
	Ready,
	Running,
	Exited,
} TaskStatus;

typedef struct { //Q info returned to user space 
	TaskStatus status;
	unsigned int syscall_times[MAX_SYSCALL_NUM];
	int time;
} TaskInfo;

static uint64 sys_task_info(TaskInfo *ti)
{
	struct proc *p = curr_proc(); //Q pointer current process
	TaskInfo kt;  //Q local task info in kernal mem
	uint64 now; //Q current cpu count 
	uint64 delta; //Q current cycle count vs start of process cycle count 

	kt.status = Running; //Q makes status Running 

	for (int i = 0; i < MAX_SYSCALL_NUM; i++) //Q copy counters to taskinfo we return
		kt.syscall_times[i] = p->syscall_times[i];
		

	now = get_cycle(); //Q hardware cycle counter and stores it in now
	if (!p->start_cycle_inited) { //Q if start time not initiated initiate then set as initalized  
		p->start_cycle = now;
		p->start_cycle_inited = 1;
	}

	delta = now - p->start_cycle; //Q how many cycles passed sense start 
	kt.time = (int)(delta * 1000 / CPU_FREQ); //Q cycles to milli seconds 
	// cpu_freq is cpu cycles per sec *1000 is to milliseconds 
	if (copy_to_user_by_useraddr((uint64)ti, &kt, sizeof(kt)) < 0) //Q copy kt to user buffer ti
		return -1;

	return 0;
}

static int mmap_perm_from_port(uint64 port)
{
	int perm = PTE_U; //Q Project 2 spec says port bit 0 = read, bit 1 = write, bit 2 = execute.
	if (port & 0x1)
		perm |= PTE_R;
	if (port & 0x2)
		perm |= PTE_W;
	if (port & 0x4)
		perm |= PTE_X;
	return perm;
}

static uint64 sys_mmap(uint64 start, uint64 len, uint64 port, uint64 flag, uint64 fd)
{
	uint64 end;
	int perm;
	struct proc *p = curr_proc();
	(void)flag;//Q
	(void)fd;//Q ignore these 2 as specified 

	if (len == 0) //return good
		return 0;

	if (len > MAX_MMAP_LEN) //reject too big
		return -1;

	if ((start % PGSIZE) != 0) // reject not aligned start address 
		return -1;

	if ((port & ~0x7UL) != 0) //bad permission	
		return -1;

	if ((port & 0x7UL) == 0) //reject no permission
		return -1;

	end = PGROUNDUP(start + len);
	if (end < start)
		return -1;

	for (uint64 va = start; va < end; va += PGSIZE) { 
		//Q^ makes sure requested region not mapped
		if (walkaddr(p->pagetable, va) != 0)
			return -1;
	}

	perm = mmap_perm_from_port(port);
 //Q 171- 185 allocates one page at a time, zeros each page, maps pages table,
 //... roll back if failes 
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
		p->max_page = end / PGSIZE; //Q updates proccesses max page count 

	return 0;
}
//Q 192-220 validates map, checks page region is mapped, unmaps pages if everythings okay
static uint64 sys_munmap(uint64 start, uint64 len)
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

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };//comment
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);

	if (id >= 0 && id < MAX_SYSCALL_NUM)
	//Q added system calling counter for valid syss calls
		curr_proc()->syscall_times[id]++;

	switch (id) {
	//Q Hook the new syscalls into the dispatcher so user programs can call them 238-245
	case SYS_write:
		ret = sys_write(args[0], (char *)args[1], args[2]);
		//Q write len bytes from user buffer to stdout
		break;
	case SYS_exit:
		sys_exit(args[0]);  // terminate the current process with the given exit code
	case SYS_sched_yield:
		ret = sys_sched_yield(); //Q voluntarily give up the CPU
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
		 //Q return current time to user-provided TimeVal
		break;
	case SYS_getpid:
		ret = (int)sys_getpid();
		break;
	case SYS_taskinfo:
		ret = (int)sys_task_info((TaskInfo *)args[0]);
		//Q copy current task status, syscall counts, and runtime to user space
		break;
	case SYS_mmap:
		ret = (int)sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break; //Q map anonymous user memory with requested permissions
	case SYS_munmap:
		ret = (int)sys_munmap(args[0], args[1]);
		break;  // unmap a previously mapped user memory region
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;// place syscall return value in a0 for user mode
	tracef("syscall ret %d", ret); // debug print of syscall return value
}