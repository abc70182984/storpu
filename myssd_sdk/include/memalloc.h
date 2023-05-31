#ifndef _MEMALLOC_H_
#define _MEMALLOC_H_

#include <types.h>

void mem_init(int mem_zone, phys_addr_t mem_start, size_t free_mem_size);
phys_addr_t alloc_mem(size_t memsize, size_t alignment, int flags);
phys_addr_t alloc_pages(unsigned long nr_pages, int flags);
void* alloc_vmpages(unsigned long nr_pages, int flags);
void free_mem(phys_addr_t base, size_t len);

#endif
