#ifndef _BARRIER_H_
#define _BARRIER_H_

#include "xpseudo_asm.h"

#define cmb() __asm__ __volatile__("" ::: "memory")

#define sev()  asm volatile("sev" : : : "memory")
#define sevl() asm volatile("sevl" : : : "memory")
#define wfe()  asm volatile("wfe" : : : "memory")
#define wfi()  asm volatile("wfi" : : : "memory")

#undef isb
#undef dmb
#undef dsb

#define isb()    asm volatile("isb" : : : "memory")
#define dmb(opt) asm volatile("dmb " #opt : : : "memory")
#define dsb(opt) asm volatile("dsb " #opt : : : "memory")

#define smp_mb()  dmb(ish)
#define smp_rmb() dmb(ishld)
#define smp_wmb() dmb(ishst)

#endif
