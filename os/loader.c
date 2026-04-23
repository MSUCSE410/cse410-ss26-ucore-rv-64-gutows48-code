#include "loader.h"
#include "defs.h"
#include "trap.h"
#include "proc.h"
#include "vm.h"
#include "riscv.h"

static int app_num;
static uint64 *app_info_ptr;
extern char _app_num[], _app_names[], INIT_PROC[];
char names[MAX_APP_NUM][MAX_STR_LEN];

// Get user progs' information through pre-defined symbol in `link_app.S`
void loader_init()
{
	char *s;
	app_info_ptr = (uint64 *)_app_num;
	app_num = *app_info_ptr;
	app_info_ptr++;
	s = _app_names;
	printf("app list:\n");
	for (int i = 0; i < app_num; ++i) {
		int len = strlen(s);
		strncpy(names[i], (const char *)s, len);
		names[i][len] = '\0';
		s += len + 1;
		printf("%s\n", names[i]);
	}
}

int get_id_by_name(char *name)
{
	for (int i = 0; i < app_num; ++i) {
		if (strncmp(name, names[i], 100) == 0)
			return i;
	}
	warnf("Cannot find such app %s", name);
	return -1;
}

int bin_loader(uint64 start, uint64 end, struct proc *p)
{
	if (p == NULL || p->state == UNUSED)
		panic("bin_loader: invalid proc");

	if (p->trapframe == 0)
		panic("bin_loader: missing trapframe");

	if (p->pagetable == 0) {
		p->pagetable = uvmcreate((uint64)p->trapframe); //Q pass trapframe into uvmcreate because current VM code maps it there
		if (p->pagetable == 0)
			panic("bin_loader: uvmcreate failed");
	}

	uint64 pa_start = PGROUNDDOWN(start);
	uint64 pa_end = PGROUNDUP(end);
	uint64 length = pa_end - pa_start;
	uint64 va_start = BASE_ADDRESS;
	uint64 va_end = BASE_ADDRESS + length;

	for (uint64 va = va_start, pa = pa_start; pa < pa_end;
	     va += PGSIZE, pa += PGSIZE) {
		char *page = (char *)kalloc();
		if (page == 0)
			panic("bin_loader: kalloc failed");

		memset(page, 0, PGSIZE);
		memmove(page, (const void *)pa, PGSIZE);

		if (pa < start)
			memset(page, 0, start - pa);
		if (pa + PGSIZE > end)
			memset(page + (end - pa), 0, PGSIZE - (end - pa));

		if (mappages(p->pagetable, va, PGSIZE, (uint64)page,
			     PTE_U | PTE_R | PTE_W | PTE_X) != 0)
			panic("bin_loader: map app page failed");
	}

	p->ustack = va_end + PGSIZE;
	for (uint64 va = p->ustack; va < p->ustack + USTACK_SIZE;
	     va += PGSIZE) {
		char *page = (char *)kalloc();
		if (page == 0)
			panic("bin_loader: kalloc stack failed");

		memset(page, 0, PGSIZE);
		if (mappages(p->pagetable, va, PGSIZE, (uint64)page,
			     PTE_U | PTE_R | PTE_W) != 0)
			panic("bin_loader: map stack failed");
	}

	p->trapframe->sp = p->ustack + USTACK_SIZE;
	p->trapframe->epc = va_start;
	p->max_page = PGROUNDUP(p->ustack + USTACK_SIZE - 1) / PGSIZE;
	p->state = RUNNABLE;

	return 0;
}

int loader(int app_id, struct proc *p)
{
	return bin_loader(app_info_ptr[app_id], app_info_ptr[app_id + 1], p);
}

// load all apps and init the corresponding `proc` structure.
int load_init_app()
{
	int id = get_id_by_name(INIT_PROC);
	if (id < 0)
		panic("Cannot find INIT_PROC %s", INIT_PROC);

	struct proc *p = allocproc();
	if (p == NULL)
		panic("allocproc failed");

	debugf("load init proc %s", INIT_PROC);
	loader(id, p);
	add_task(p);
	return 0;
}