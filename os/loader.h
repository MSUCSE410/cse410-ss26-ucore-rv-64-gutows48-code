#ifndef LOADER_H
#define LOADER_H

#include "const.h"
#include "types.h"

#define BASE_ADDRESS (0x1000UL) //Q sets page size
#define USTACK_SIZE (PAGE_SIZE)
#define KSTACK_SIZE (PAGE_SIZE)
#define TRAP_PAGE_SIZE (PAGE_SIZE)

int finished(); 
void loader_init();
int run_all_app();

#endif // LOADER_H