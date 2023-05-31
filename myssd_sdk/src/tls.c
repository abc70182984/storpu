#include <config.h>
#include <const.h>
#include "tls.h"
#include "proto.h"
#include <utils.h>
#include <page.h>
#include <memalloc.h>

#define BOOT_TLS_OFFSET 0
size_t __tls_offset[NR_FTL_THREADS] = {
    [0 ... NR_FTL_THREADS - 1] = BOOT_TLS_OFFSET,
};

void tls_init(void)
{
    size_t size;
    char* ptr;
    int tid;
    extern char __tlsdata_start[], __tlsdata_end[];

    size = roundup(__tlsdata_end - __tlsdata_start, ARCH_PG_SIZE);
    ptr = alloc_vmpages((size * NR_FTL_THREADS) >> ARCH_PG_SHIFT, ZONE_PS_DDR);

    for (tid = 0; tid < NR_FTL_THREADS; tid++) {
        tls_offset(tid) = ptr - (char*)__tlsdata_start;
        memcpy(ptr, (void*)__tlsdata_start, __tlsdata_end - __tlsdata_start);

        ptr += size;
    }
}
