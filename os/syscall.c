#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "fcntl.h" //Q CH6: file open flags such as O_CREATE, O_TRUNC, O_WRONLY, and O_RDWR
#include "file.h" //Q CH6: file structures and helper functions
#include "fs.h"   //Q CH6: Stat structure, DIR and FILE constants, and inode helper functions
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
	struct proc *p = curr_proc(); //Q CH6: current process
	struct file *f; //Q CH6: file attached to this file descriptor
	char str[MAX_STR_LEN]; //Q CH6: temporary buffer for console output
	int size; //Q CH6: number of bytes copied for console output

	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);

	if (fd < 0 || fd >= FD_BUFFER_SIZE) //Q CH6: invalid file descriptor number
		return -1;

	f = p->files[fd]; //Q CH6: get the file from the process file descriptor table
	if (f == 0 || !f->writable) //Q CH6: the file descriptor must exist and allow writing
		return -1;

	if (f->type == FD_STDIO) { //Q CH6: writing to standard output or standard error
		size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN)); //Q CH6: copy the user buffer into a kernel buffer
		if (size < 0)
			return -1;
		for (int i = 0; i < size; ++i)
			console_putchar(str[i]);
		return size;
	}

	if (f->type == FD_INODE) //Q CH6: writing to a normal file
		return inodewrite(f, va, len);

	return -1; //Q CH6: unsupported file type
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	struct proc *p = curr_proc(); //Q CH6: current process
	struct file *f; //Q CH6: file attached to this file descriptor
	char str[MAX_STR_LEN]; //Q CH6: temporary buffer for standard input
	uint64 n; //Q CH6: number of bytes read for the standard input path

	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);

	if (fd < 0 || fd >= FD_BUFFER_SIZE) //Q CH6: invalid file descriptor number
		return -1;
	f = p->files[fd]; //Q CH6: get the file from the process file descriptor table
	if (f == 0 || !f->readable) //Q CH6: the file descriptor must exist and allow reading
		return -1;

	if (f->type == FD_STDIO) { //Q CH6: reading from standard input
		n = MIN(len, MAX_STR_LEN);
		for (uint64 i = 0; i < n; ++i)
			str[i] = consgetc();
		if (copyout(p->pagetable, va, str, n) < 0)
			return -1;
		return n;
	}

	if (f->type == FD_INODE) //Q CH6: reading from a normal file
		return inoderead(f, va, len);

	return -1; //Q CH6: unsupported file type
}

uint64 sys_openat(int dirfd, uint64 pathva, uint64 flags, uint64 mode)
{
	struct proc *p = curr_proc(); //Q CH6: current process
	char path[MAXPATH]; //Q CH6: file name copied from user space

	(void)dirfd; 
	(void)mode; 

	if (copyinstr(p->pagetable, path, pathva, MAXPATH) < 0) //Q CH6: copy the path from user memory
		return -1;

	return fileopen(path, flags); //Q CH6: let the file layer handle open and create
}

uint64 sys_close(int fd)
{
	struct proc *p = curr_proc(); //Q CH6: current process
	struct file *f; //Q CH6: file being closed

	if (fd < 0 || fd >= FD_BUFFER_SIZE) //Q CH6: invalid file descriptor number
		return -1;

	f = p->files[fd]; //Q CH6: get the file from the file descriptor table
	if (f == 0) //Q CH6: nothing is open at this file descriptor
		return -1;

	p->files[fd] = 0; //Q CH6: clear this process's file descriptor slot
	fileclose(f); //Q CH6: decrease the file reference count
	return 0;
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

static int mmap_perm_from_port(uint64 port) //Q CH6: convert user memory map permission bits into page table permission bits
{
	int perm = PTE_U; //Q CH6: all user mappings must be user accessible
	if (port & 0x1)
		perm |= PTE_R; //Q CH6: readable
	if (port & 0x2)
		perm |= PTE_W; //Q CH6: writable
	if (port & 0x4)
		perm |= PTE_X; //Q CH6: executable
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
			return -1; //Q CH6: do not map over pages that already exist
	}

	perm = mmap_perm_from_port(port);

	for (uint64 va = start; va < end; va += PGSIZE) {
		void *mem = kalloc(); //Q CH6: allocate one physical page
		if (mem == 0) {
			uvmunmap(p->pagetable, start, (va - start) / PGSIZE, 1); //Q CH6: undo partial work on failure
			return -1;
		}

		memset(mem, 0, PGSIZE); //Q CH6: start with a blank page

		if (mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) < 0) {
			kfree(mem); //Q CH6: free the current page if mapping failed
			uvmunmap(p->pagetable, start, (va - start) / PGSIZE, 1); //Q CH6: undo earlier mappings
			return -1;
		}
	}

	if (end / PGSIZE > p->max_page)
		p->max_page = end / PGSIZE; //Q CH6: update the highest used user page

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
			return -1; //Q CH6: cannot unmap pages that do not exist
	}

	uvmunmap(p->pagetable, start, npages, 1); //Q CH6: unmap and free these pages
	return 0;
}

uint64 sys_spawn(uint64 va)
{
	struct proc *parent = curr_proc(); 
	struct proc *np; 
	char name[200]; 
	int id; 

	if (copyinstr(parent->pagetable, name, va, 200) < 0)
		return -1; 

	id = get_id_by_name(name); 
	if (id < 0)
		return -1;

	np = allocproc(); 
	if (np == 0)
		return -1;

	np->parent = parent; 
	np->stride = 0; 
	np->priority = 16; 
	np->pass = BIG_STRIDE / 16; 
	np->start_cycle = 0; 
	np->start_cycle_inited = 0;
	memset(np->syscall_times, 0, sizeof(np->syscall_times)); 

	if (loader(id, np) < 0) { 
		freeproc(np);
		return -1;
	}

	np->state = RUNNABLE; 
	add_task(np);
	return np->pid;
}

uint64 sys_set_priority(long long prio)
{
	struct proc *p = curr_proc();

	if (prio < 2)
		return -1; //Q CH6: bad priority

	p->priority = (int)prio; //Q CH6: save the new priority
	p->pass = BIG_STRIDE / (uint64)p->priority; //Q CH6: recompute stride pass value
	return prio;
}

uint64 sys_linkat(int olddirfd, uint64 oldpathva, int newdirfd, uint64 newpathva, uint64 flags)
{
	struct proc *p = curr_proc(); //Q CH6: current process
	char oldpath[MAXPATH]; //Q CH6: existing file name
	char newpath[MAXPATH]; //Q CH6: new hard link name
	struct inode *dp; //Q CH6: root directory inode
	struct inode *ip; //Q CH6: file being linked

	(void)olddirfd; //Q CH6: ignored in this project
	(void)newdirfd; //Q CH6: ignored in this project
	(void)flags; //Q CH6: ignored in this project

	if (copyinstr(p->pagetable, oldpath, oldpathva, MAXPATH) < 0) //Q CH6: copy the old path from user memory
		return -1;
	if (copyinstr(p->pagetable, newpath, newpathva, MAXPATH) < 0) //Q CH6: copy the new path from user memory
		return -1;

	if (strncmp(oldpath, newpath, MAXPATH) == 0) //Q CH6: linking a file to the same name is an error
		return -1;

	ip = namei(oldpath); //Q CH6: find the old file
	if (ip == 0)
		return -1;

	ivalid(ip);
	if (ip->type != T_FILE) { //Q CH6: only regular files matter here
		iput(ip);
		return -1;
	}

	dp = root_dir(); //Q CH6: there is only one directory level in this project
	ivalid(dp);

	if (dirlink(dp, newpath, ip->inum) < 0) { //Q CH6: add a new directory entry that points to the same inode
		iput(dp);
		iput(ip);
		return -1;
	}

	ip->nlink++; //Q CH6: one more hard link now points to this file
	iupdate(ip); //Q CH6: save the new link count
	iput(dp);
	iput(ip);
	return 0;
}

uint64 sys_unlinkat(int dirfd, uint64 pathva, uint64 flags)
{
	struct proc *p = curr_proc(); //Q CH6: current process
	char path[MAXPATH]; //Q CH6: file name to unlink
	struct inode *dp; //Q CH6: root directory inode
	struct inode *ip; //Q CH6: file being unlinked

	(void)dirfd; //Q CH6: ignored in this project
	(void)flags; //Q CH6: ignored in this project

	if (copyinstr(p->pagetable, path, pathva, MAXPATH) < 0) //Q CH6: copy the path from user memory
		return -1;

	ip = namei(path); //Q CH6: find the file
	if (ip == 0)
		return -1;

	ivalid(ip);
	dp = root_dir();
	ivalid(dp);

	if (dirunlink(dp, path) < 0) { //Q CH6: remove this name from the directory
		iput(dp);
		iput(ip);
		return -1;
	}

	if (ip->nlink > 0)
		ip->nlink--; //Q CH6: one fewer hard link points to this inode
	iupdate(ip); //Q CH6: save the new link count
	iput(dp);
	iput(ip); //Q CH6: if the count reached zero, iput may fully delete the file
	return 0;
}

uint64 sys_fstat(int fd, uint64 stva)
{
	struct proc *p = curr_proc(); //Q CH6: current process
	struct file *f; //Q CH6: opened file for this file descriptor
	struct Stat st; //Q CH6: file information copied back to user memory

	if (fd < 0 || fd >= FD_BUFFER_SIZE) //Q CH6: invalid file descriptor
		return -1;

	f = p->files[fd]; //Q CH6: get the file from the file descriptor table
	if (f == 0 || f->type != FD_INODE || f->ip == 0) //Q CH6: must be a real inode-backed file
		return -1;

	ivalid(f->ip);

	memset(&st, 0, sizeof(st)); //Q CH6: clear all fields first
	st.dev = 0; //Q CH6: project says device should be zero
	st.ino = f->ip->inum; //Q CH6: inode number
	st.nlink = f->ip->nlink; //Q CH6: hard link count
	st.mode = (f->ip->type == T_DIR) ? DIR : FILE; //Q CH6: report directory or regular filech6_usertest

	if (copyout(p->pagetable, stva, (char *)&st, sizeof(st)) < 0) //Q CH6: copy the Stat structure to user memory
		return -1;

	return 0;
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
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2], args[3]); //Q CH6: open or create a file
		break;
	case SYS_close:
		ret = sys_close(args[0]); //Q CH6: close a file descriptor
		break;
	case SYS_unlinkat:
		ret = sys_unlinkat(args[0], args[1], args[2]); //Q CH6: remove one hard link
		break;
	case SYS_linkat:
		ret = sys_linkat(args[0], args[1], args[2], args[3], args[4]); //Q CH6: create a hard link
		break;
	case SYS_fstat:
		ret = sys_fstat(args[0], args[1]); //Q CH6: get file information
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
		ret = sys_spawn(args[0]);
		break;
	case SYS_setpriority:
		ret = sys_set_priority(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}