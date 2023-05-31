#ifndef _CONST_H_
#define _CONST_H_

/* max() & min() */
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

#define roundup(x, align) \
    (((x) % align == 0) ? (x) : (((x) + align) - ((x) % align)))
#define rounddown(x, align) ((x) - ((x) % align))

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#ifdef __ASSEMBLY__
#define _AC(X, Y) X
#define _AT(T, X) X
#else
#define __AC(X, Y) (X##Y)
#define _AC(X, Y)  __AC(X, Y)
#define _AT(T, X)  ((T)(X))
#endif

#define _UL(x)  (_AC(x, UL))
#define _ULL(x) (_AC(x, ULL))

#define _BITUL(x)  (_UL(1) << (x))
#define _BITULL(x) (_ULL(1) << (x))

#define UL(x)  (_UL(x))
#define ULL(x) (_ULL(x))

#define BIT(nr) (UL(1) << (nr))

/* Memory zones */
#define MEMZONE_PS_DDR_HIGH 0
#define MEMZONE_PS_DDR_LOW  1
#define MEMZONE_PL_DDR      2
#define MEMZONE_MAX         3

/* Zone flags */
#define ZONE_PS_DDR_LOW  (1 << MEMZONE_PS_DDR_LOW)
#define ZONE_PS_DDR_HIGH (1 << MEMZONE_PS_DDR_HIGH)
#define ZONE_PL_DDR      (1 << MEMZONE_PL_DDR)
#define ZONE_DMA         (ZONE_PS_DDR_LOW | ZONE_PL_DDR)
#define ZONE_PS_DDR      (ZONE_PS_DDR_LOW | ZONE_PS_DDR_HIGH)
#define ZONE_ALL         (ZONE_PS_DDR_LOW | ZONE_PS_DDR_HIGH | ZONE_PL_DDR)

/* IO request types */
#define IOREQ_READ         1
#define IOREQ_WRITE        2
#define IOREQ_FLUSH        3
#define IOREQ_FLUSH_DATA   4
#define IOREQ_SYNC         5
#define IOREQ_WRITE_ZEROES 6

/* Default stack size */
#define K_STACK_SIZE 0x1000

/* Profiling buffer */
#define PROFILE_BUF_SIZE  (128UL << 20)
#define PROFILE_BUF_START ((2UL << 30) - PROFILE_BUF_SIZE)

#endif
