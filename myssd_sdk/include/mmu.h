#ifndef _MMU_H_
#define _MMU_H_

static inline void flush_tlb(void)
{
    dsb(nshst);
    asm("tlbi vmalle1\n" ::);
    dsb(nsh);
    isb();
}

#endif
