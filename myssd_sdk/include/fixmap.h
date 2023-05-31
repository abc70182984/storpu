#ifndef _ARCH_FIXMAP_H_
#define _ARCH_FIXMAP_H_

#include <page.h>

enum fixed_address {
    FIX_HOLE,

#define FIX_FDT_SIZE (4 << 20)
    FIX_FDT_END,
    FIX_FDT = FIX_FDT_END + (FIX_FDT_SIZE >> ARCH_PG_SHIFT) - 1,

    FIX_EARLYCON_MEM_BASE,

    __end_of_permanent_fixed_addresses,

    FIX_CPU_RELEASE_ADDR,

    FIX_PTE,
    FIX_PMD,
    FIX_PUD,
    FIX_PGD,

    __end_of_fixed_addresses
};

#define FIXADDR_SIZE  (__end_of_permanent_fixed_addresses << ARCH_PG_SHIFT)
#define FIXADDR_START (FIXADDR_TOP - FIXADDR_SIZE)

#define FIXMAP_PAGE_NORMAL ARM64_PG_KERNEL
#define FIXMAP_PAGE_IO     __pgprot(_ARM64_DEVICE_nGnRE)

extern void early_fixmap_init(void);

extern void __set_fixmap(enum fixed_address idx, phys_addr_t phys,
                         pgprot_t prot);

#define __fix_to_virt(x) (FIXADDR_TOP - ((x) << ARCH_PG_SHIFT))

#ifndef FIXMAP_PAGE_CLEAR
#define FIXMAP_PAGE_CLEAR __pgprot(0)
#endif

#ifndef set_fixmap
#define set_fixmap(idx, phys) __set_fixmap(idx, phys, FIXMAP_PAGE_NORMAL)
#endif

#ifndef clear_fixmap
#define clear_fixmap(idx) __set_fixmap(idx, 0, FIXMAP_PAGE_CLEAR)
#endif

/* Return a pointer with offset calculated */
#define __set_fixmap_offset(idx, phys, flags)                              \
    ({                                                                     \
        unsigned long ________addr;                                        \
        __set_fixmap(idx, phys, flags);                                    \
        ________addr = __fix_to_virt(idx) + ((phys) & (ARCH_PG_SIZE - 1)); \
        ________addr;                                                      \
    })

#define set_fixmap_offset(idx, phys) \
    __set_fixmap_offset(idx, phys, FIXMAP_PAGE_NORMAL)

#define set_fixmap_io(idx, phys) __set_fixmap(idx, phys, FIXMAP_PAGE_IO)

#define set_fixmap_offset_io(idx, phys) \
    __set_fixmap_offset(idx, phys, FIXMAP_PAGE_IO)

#endif
