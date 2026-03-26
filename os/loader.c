#include "loader.h"
#include "defs.h"
#include "trap.h"
#include "proc.h"
#include "vm.h" //Q VM support for page tables, mappings, and address-space setup
#include "riscv.h"

static uint64 app_num;
static uint64 *app_info_ptr;
extern char _app_num[];
extern char trampoline[]; //Q trampoline symbol used with ch4 virtual memory/trap setup

int finished()
{
	static int fin = 0;
	if (++fin >= app_num)
		panic("all apps over");
	return 0;
}

void loader_init()
{
	app_info_ptr = (uint64 *)_app_num;
	app_num = *app_info_ptr;
	app_info_ptr++;
}

static pagetable_t bin_loader(uint64 start, uint64 end, struct proc *p) 
//Q^ build a fresh user address space and load one app into it
{
	pagetable_t pg; //Q this process's new user page table
	uint64 length; //Q app size rounded up to whole pages
	uint64 app_bytes; //Q exact app size before page rounding
	uint64 ustack_bottom_vaddr; //Q virtual address where the user stack starts
	uint64 stack_pa; //Q physical address of the allocated stack page

	pg = uvmcreate(); //Q create an empty user page table for this process
	if (pg == 0) {
		errorf("bin_loader: uvmcreate error");
		return 0;
	}

	if (mappages(pg, TRAPFRAME, PGSIZE, (uint64)p->trapframe, PTE_R | PTE_W) < 0) 
	//Q^ map the proc's trapframe into its page table so trap handling works
		panic("bin_loader: map trapframe fail");
		//Q trap frame saves cpu state of a user process when a swap to kernal occurs
	if (!PGALIGNED(start)) //Q ensure the app image starts on a page boundary
		panic("user program not aligned, start = %p", start);

	app_bytes = end - start; //Q number of bytes in the program image
	end = PGROUNDUP(end); //Q round app end up so we map full pages
	length = end - start; //Q total byte length to allocate/map for the app

	//Q Allocate fresh pages for the app image so uvmfree()/kfree() are valid later.
	for (uint64 off = 0; off < length; off += PGSIZE) { //Q iterate through one page at a time
		uint64 va = BASE_ADDRESS + off; //Q user virtual address where this app page will live
		char *mem = (char *)kalloc(); //Q allocate one physical page for this piece of the app
		if (mem == 0)
			panic("bin_loader: kalloc app page fail");

		memset(mem, 0, PGSIZE); //Q clear the page before copying program bytes into it

		uint64 remaining = (off < app_bytes) ? (app_bytes - off) : 0; 
		//Q^ bytes of real program data still left to copy
		uint64 n = remaining > PGSIZE ? PGSIZE : remaining; 
		//Q^ copy either one full page or the remaining partial page
		if (n > 0)
			memmove(mem, (void *)(start + off), n); 
			//Q^ copy app bytes from the linked image into the new page

		if (mappages(pg, va, PGSIZE, (uint64)mem, PTE_U | PTE_R | PTE_W | PTE_X) != 0) 
		//Q^ map the new page into user space with user R/W/X permissions
			panic("bin_loader: map app page fail");
	}

	ustack_bottom_vaddr = BASE_ADDRESS + length + PGSIZE;
	 //Q^ place the user stack after the app with a one-page gap
	stack_pa = (uint64)kalloc(); //Q allocate one physical page for the user stack
	if (stack_pa == 0)
		panic("bin_loader: kalloc stack fail");
	memset((void *)stack_pa, 0, PGSIZE); //Q clear the stack page before use

	if (mappages(pg, ustack_bottom_vaddr, USTACK_SIZE, stack_pa, PTE_U | PTE_R | PTE_W | PTE_X) != 0) 
	//Q ^ map the stack into the process page table
		panic("bin_loader: map stack fail");

	p->pagetable = pg; //Q save the page table in the proc struct for later traps/syscalls
	p->ustack = ustack_bottom_vaddr; //Q record the stack's base virtual address
	p->trapframe->epc = BASE_ADDRESS; //Q first user instruction will execute at BASE_ADDRESS
	p->trapframe->sp = p->ustack + USTACK_SIZE; 
	//Q^ initial user stack pointer starts at the top of the stack
	p->max_page = PGROUNDUP(p->ustack + USTACK_SIZE - 1) / PGSIZE; 
	//Q ^track highest mapped user page so exit can free the address space correctly

	return pg; //Q return page table
}

int run_all_app()
{
	for (int i = 0; i < app_num; ++i) {
		struct proc *p = allocproc();

		if (bin_loader(app_info_ptr[i], app_info_ptr[i + 1], p) == 0) 
		//Q^ load each app into its own freshly built address space
			panic("run_all_app: bin_loader fail");

		memset(p->syscall_times, 0, sizeof(p->syscall_times));
		p->start_cycle = 0;
		p->start_cycle_inited = 0;
		p->state = RUNNABLE;
	}
	return 0;
}