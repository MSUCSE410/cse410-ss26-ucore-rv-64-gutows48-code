#include "loader.h"
#include "defs.h"
#include "file.h" //Q CH6: need struct inode definition for filesystem-backed loading
#include "fs.h" //Q CH6: need MAXPATH, T_FILE, namei, readi, iput
#include "trap.h"
#include "proc.h"
#include "vm.h"
#include "riscv.h"

#ifndef MAX_APP_SIZE
#define MAX_APP_SIZE 0x20000 //Q CH6: reserve a fixed app region so code/data/bss all have room
#endif

static char pending_name[MAXPATH]; //Q CH6: remember the filename requested by get_id_by_name

extern char INIT_PROC[];

void loader_init()
{
	pending_name[0] = '\0'; //Q CH6: initialize remembered filename buffer
}

int get_id_by_name(char *name)
{
	struct inode *ip; //Q CH6: verify that the named file exists in the root directory

	if (name == 0)
		return -1;

	ip = namei(name); //Q CH6: look up executable by filename in the filesystem
	if (ip == 0) {
		warnf("Cannot find such app %s", name);
		return -1;
	}

	iput(ip); //Q CH6: only checking existence here, actual load happens later
	strncpy(pending_name, name, MAXPATH - 1); //Q CH6: remember the requested program name
	pending_name[MAXPATH - 1] = '\0'; //Q CH6: force null-termination
	return 0; //Q CH6: compatibility return value for existing caller code
}

int bin_loader(struct inode *ip, struct proc *p)
{
	uint64 off; //Q CH6: current file offset while reading program bytes
	uint64 va_start; //Q CH6: first user virtual address of program image
	uint64 va_end; //Q CH6: end of reserved user program region
	uint64 file_size; //Q CH6: executable size in bytes from inode metadata

	if (p == NULL || p->state == UNUSED)
		panic("bin_loader: invalid proc");

	if (p->trapframe == 0)
		panic("bin_loader: missing trapframe");

	if (ip == 0)
		return -1; //Q CH6: executable inode must exist

	ivalid(ip); //Q CH6: make sure inode metadata is loaded
	if (ip->type != T_FILE)
		return -1; //Q CH6: only regular files can be executed in this simple loader

	if (p->pagetable == 0) {
		p->pagetable = uvmcreate((uint64)p->trapframe); //Q CH6: create user page table if needed
		if (p->pagetable == 0)
			panic("bin_loader: uvmcreate failed");
	}

	file_size = ip->size; //Q CH6: executable length comes from inode size
	if (file_size > MAX_APP_SIZE) //Q CH6: refuse executables larger than reserved app region
		panic("bin_loader: app too large");

	va_start = BASE_ADDRESS; //Q CH6: user programs begin at BASE_ADDRESS
	va_end = va_start + MAX_APP_SIZE; //Q CH6: reserve a full region for code/data/bss like earlier flat-binary loaders

	for (uint64 va = va_start; va < va_end; va += PGSIZE) { //Q CH6: pre-map and zero the full app region
		//Q CH6^:Reserve the whole app region, make it all real memory, and start it all at zero
		char *page = (char *)kalloc(); //Q CH6: backing physical page for program region
		if (page == 0)
			panic("bin_loader: kalloc failed");
		memset(page, 0, PGSIZE); //Q CH6: zero-fill so bss/uninitialized data works
		if (mappages(p->pagetable, va, PGSIZE, (uint64)page,
			     PTE_U | PTE_R | PTE_W | PTE_X) != 0) //Q CH6: map app region as user RWX like earlier labs
			panic("bin_loader: map app page failed");
	}

	for (off = 0; off < file_size; off += PGSIZE) { //Q CH6: copy executable bytes into already-mapped user pages
		uint64 va = va_start + off; //Q CH6: user VA corresponding to this file chunk
		uint64 n = MIN((uint64)PGSIZE, file_size - off); //Q CH6: bytes to copy this iteration
		uint64 pa = walkaddr(p->pagetable, va); //Q CH6: resolve mapped user VA to backing physical page

		if (pa == 0)
			panic("bin_loader: walkaddr failed");

		if (readi(ip, 0, pa, off, n) != (int)n) //Q CH6: read executable bytes directly into mapped physical page
			panic("bin_loader: readi failed");
	}

	p->ustack = va_end; //Q CH6: place stack immediately after app region with no guard page
	for (uint64 va = p->ustack; va < p->ustack + USTACK_SIZE;
	     va += PGSIZE) {
		char *page = (char *)kalloc(); //Q CH6: allocate user stack page
		if (page == 0)
			panic("bin_loader: kalloc stack failed");

		memset(page, 0, PGSIZE); //Q CH6: zero-fill user stack page
		if (mappages(p->pagetable, va, PGSIZE, (uint64)page,
			     PTE_U | PTE_R | PTE_W) != 0) //Q CH6: map writable stack page
			panic("bin_loader: map stack failed");
	}

	p->trapframe->sp = p->ustack + USTACK_SIZE; //Q CH6: initial user stack pointer at top of stack
	p->trapframe->epc = va_start; //Q CH6: initial PC begins at executable start
	p->max_page = PGROUNDUP(p->ustack + USTACK_SIZE - 1) / PGSIZE; //Q CH6: track highest mapped user page
	p->state = RUNNABLE; //Q CH6: loaded process can now be scheduled

	return 0;
}

int loader(int app_id, struct proc *p)
{
	struct inode *ip; //Q CH6: look up the program file remembered by get_id_by_name
	int ret; //Q CH6: propagate bin_loader result back to caller

	(void)app_id; //Q CH6: filesystem-backed loader ignores old linked-app numeric ids

	if (pending_name[0] == '\0')
		return -1; //Q CH6: nothing selected to load

	ip = namei(pending_name); //Q CH6: open executable file by name
	if (ip == 0)
		return -1;

	ret = bin_loader(ip, p); //Q CH6: load executable bytes from file into process address space
	iput(ip); //Q CH6: release inode reference after loading
	return ret;
}

// load init proc from the filesystem image.
int load_init_app()
{
	struct proc *p; //Q CH6: first runnable user process

	if (get_id_by_name(INIT_PROC) < 0)
		panic("Cannot find INIT_PROC %s", INIT_PROC);

	p = allocproc();
	if (p == NULL)
		panic("allocproc failed");

	debugf("load init proc %s", INIT_PROC);
	if (loader(0, p) < 0) //Q CH6: load filesystem-backed init process
		panic("load_init_app: loader failed");
	add_task(p);
	return 0;
}