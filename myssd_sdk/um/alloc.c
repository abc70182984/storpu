#include <sys/mman.h>
#include <stddef.h>

#include "types.h"

void* alloc_vmpages(unsigned long nr_pages, int flags)
{
    void* addr;

    addr = mmap(NULL, nr_pages << 12, PROT_READ | PROT_WRITE,
                MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (addr == MAP_FAILED) return NULL;

    return addr;
}

void free_mem(phys_addr_t base, size_t len) { munmap((void*)base, len); }
