#include <string.h>
#include <errno.h>
#include <xparameters.h>
#include <xil_assert.h>

#include "proto.h"

#include <types.h>
#include <page.h>
#include <pagetable.h>
#include <fixmap.h>
#include <mmu.h>
#include <sysreg.h>
#include <memalloc.h>

#define NO_BLOCK_MAPPINGS BIT(0)
#define NO_CONT_MAPPINGS  BIT(1)
#define NO_ALLOC_PT       BIT(2)
#define ALLOC_PHYS_MEM    BIT(3)

unsigned long va_pa_offset = 0;

extern pde_t idmap_pg_dir[];
static pde_t* swapper_pg_dir = idmap_pg_dir;

static pte_t bm_pt[ARCH_VM_PT_ENTRIES] __attribute__((aligned(ARCH_PG_SIZE)));
static pmd_t bm_pmd[ARCH_VM_PMD_ENTRIES] __attribute__((aligned(ARCH_PG_SIZE)));
static pud_t bm_pud[ARCH_VM_PUD_ENTRIES] __attribute__((aligned(ARCH_PG_SIZE)));

static pmd_t pk_pmd[ARCH_VM_PMD_ENTRIES] __attribute__((aligned(ARCH_PG_SIZE)));

static int pt_follow(pgdir_t* pgd, unsigned long vir_addr, pte_t** ptepp);

static inline pud_t* fixmap_pude(unsigned long addr)
{
    pde_t* pde;

    pde = pgd_offset(swapper_pg_dir, addr);
    return pud_offset(pde, addr);
}

static inline pmd_t* fixmap_pmde(unsigned long addr)
{
    pud_t* pude = fixmap_pude(addr);
    return pmd_offset(pude, addr);
}

static inline pte_t* fixmap_pte(unsigned long addr)
{
    return &bm_pt[ARCH_PTE(addr)];
}

static phys_addr_t pg_alloc_pt(void)
{
    phys_addr_t phys_addr = alloc_pages(1, ZONE_PS_DDR);
    void* pt;

    if (!phys_addr) {
        panic("failed to allocate page table page");
    }

    pt = pte_set_fixmap(phys_addr);

    memset(pt, 0, ARCH_PG_SIZE);

    pte_clear_fixmap();

    return phys_addr;
}

static void alloc_init_pte(pmd_t* pmde, unsigned long addr, unsigned long end,
                           phys_addr_t phys, pgprot_t prot, int flags)
{
    pte_t* pte;

    if (pmde_none(*pmde)) {
        pmdval_t pmdval = _ARM64_PMD_TYPE_TABLE;
        phys_addr_t pt_phys;

        if (flags & NO_ALLOC_PT) panic("page table allocation not allowed");
        pt_phys = pg_alloc_pt();
        __pmde_populate(pmde, pt_phys, pmdval);
    }

    pte = pte_set_fixmap_offset(pmde, addr);
    do {
        phys_addr_t ph = phys;
        if (flags & ALLOC_PHYS_MEM) ph = alloc_pages(1, ZONE_PS_DDR);

        set_pte(pte, pfn_pte(ph >> ARCH_PG_SHIFT, prot));

        phys += ARCH_PG_SIZE;
    } while (pte++, addr += ARCH_PG_SIZE, addr != end);

    pte_clear_fixmap();
}

static void alloc_init_pmd(pud_t* pude, unsigned long addr, unsigned long end,
                           phys_addr_t phys, pgprot_t prot, int flags)
{
    unsigned long next;
    pmd_t* pmde;

    if (pude_none(*pude)) {
        pudval_t pudval = _ARM64_PUD_TYPE_TABLE;
        phys_addr_t pmd_phys;

        if (flags & NO_ALLOC_PT) panic("page table allocation not allowed");
        pmd_phys = pg_alloc_pt();
        __pude_populate(pude, pmd_phys, pudval);
    }

    pmde = pmd_set_fixmap_offset(pude, addr);
    do {
        next = pmd_addr_end(addr, end);

        if (((addr | next | phys) & ~ARCH_PMD_MASK) == 0 &&
            !(flags & NO_BLOCK_MAPPINGS)) {
            set_pmde(pmde,
                     pfn_pmd(phys >> ARCH_PG_SHIFT, mk_pmd_sect_prot(prot)));
        } else {
            alloc_init_pte(pmde, addr, next, phys, prot, flags);
        }
        phys += next - addr;
    } while (pmde++, addr = next, addr != end);

    pmd_clear_fixmap();
}

static void alloc_init_pud(pde_t* pde, unsigned long addr, unsigned long end,
                           phys_addr_t phys, pgprot_t prot, int flags)
{
    unsigned long next;
    pud_t* pude;

    if (pde_none(*pde)) {
        pdeval_t pdeval = _ARM64_PGD_TYPE_TABLE;
        phys_addr_t pud_phys;

        if (flags & NO_ALLOC_PT) panic("page table allocation not allowed");
        pud_phys = pg_alloc_pt();
        __pde_populate(pde, pud_phys, pdeval);
    }

    pude = pud_set_fixmap_offset(pde, addr);
    do {
        next = pud_addr_end(addr, end);

        if (pud_sect_supported() &&
            ((addr | next | phys) & ~ARCH_PUD_MASK) == 0 &&
            !(flags & NO_BLOCK_MAPPINGS)) {
            set_pude(pude,
                     pfn_pud(phys >> ARCH_PG_SHIFT, mk_pud_sect_prot(prot)));
        } else {
            alloc_init_pmd(pude, addr, next, phys, prot, flags);
        }

        phys += next - addr;
    } while (pude++, addr = next, addr != end);

    pud_clear_fixmap();
}

static void __create_pgd_mapping(pde_t* pgd_page, phys_addr_t phys,
                                 unsigned long virt, phys_addr_t size,
                                 pgprot_t prot, int flags)
{
    unsigned long addr, end, next;
    pde_t* pde = pgd_offset(pgd_page, virt);

    phys &= ARCH_PG_MASK;
    addr = virt & ARCH_PG_MASK;
    end = roundup(virt + size, ARCH_PG_SIZE);

    do {
        next = pgd_addr_end(addr, end);
        alloc_init_pud(pde, addr, next, phys, prot, flags);
        phys += next - addr;
    } while (pde++, addr = next, addr != end);
}

void early_fixmap_init(void)
{
    pde_t* pde;
    pud_t* pude;
    pmd_t* pmde;
    unsigned long addr = FIXADDR_START;

    pde = pgd_offset(swapper_pg_dir, addr);
    if (pde_none(*pde))
        __pde_populate(pde, __pa(bm_pud), _ARM64_PGD_TYPE_TABLE);
    pude = fixmap_pude((unsigned long)addr);

    if (pude_none(*pude))
        __pude_populate(pude, __pa(bm_pmd), _ARM64_PUD_TYPE_TABLE);
    pmde = fixmap_pmde(addr);

    __pmde_populate(pmde, __pa(bm_pt), _ARM64_PMD_TYPE_TABLE);

    addr = PKMAP_START;
    pude = fixmap_pude((unsigned long)addr);

    if (pude_none(*pude))
        __pude_populate(pude, __pa(pk_pmd), _ARM64_PUD_TYPE_TABLE);
}

void __set_fixmap(enum fixed_address idx, phys_addr_t phys, pgprot_t prot)
{
    unsigned long addr = __fix_to_virt(idx);
    pte_t* pte;

    pte = fixmap_pte(addr);

    if (pgprot_val(prot)) {
        set_pte(pte, pfn_pte(phys >> ARCH_PG_SHIFT, prot));
    } else {
        pte_clear(pte);
        flush_tlb();
    }
}

void paging_init(void)
{
    /* 2GB PS DDR */
    __create_pgd_mapping(swapper_pg_dir, 0, 0, 2UL << 30, ARM64_PG_KERNEL, 0);

    /* 1GB lower PL */
    __create_pgd_mapping(swapper_pg_dir, 0x80000000UL, 0x80000000UL,
                         0x40000000UL, __pgprot(_ARM64_DEVICE_nGnRE), 0);

    /* 2MB GIC */
    __create_pgd_mapping(swapper_pg_dir, 0xf9000000UL, 0xf9000000UL, 0x200000UL,
                         __pgprot(_ARM64_DEVICE_nGnRE), 0);

    /* 16MB FPD devices */
    __create_pgd_mapping(swapper_pg_dir, 0xfd000000UL, 0xfd000000UL,
                         0x1000000UL, __pgprot(_ARM64_DEVICE_nGnRE), 0);

    /* 28MB LPD devices */
    __create_pgd_mapping(swapper_pg_dir, 0xfe000000UL, 0xfe000000UL,
                         0x1c00000UL, __pgprot(_ARM64_DEVICE_nGnRE), 0);

    /* 2MB OCM/TCM */
    __create_pgd_mapping(swapper_pg_dir, 0xffe00000UL, 0xffe00000UL, 0x200000UL,
                         ARM64_PG_KERNEL, 0);

    /* 2GB PS DDR */
    __create_pgd_mapping(swapper_pg_dir,
                         (unsigned long)XPAR_PSU_DDR_1_S_AXI_BASEADDR,
                         (unsigned long)XPAR_PSU_DDR_1_S_AXI_BASEADDR,
                         (unsigned long)XPAR_PSU_DDR_1_S_AXI_HIGHADDR -
                             (unsigned long)XPAR_PSU_DDR_1_S_AXI_BASEADDR + 1,
                         ARM64_PG_KERNEL, 0);

    /* 4GB PL DDR */
    __create_pgd_mapping(swapper_pg_dir, (unsigned long)XPAR_DDR4_0_BASEADDR,
                         (unsigned long)XPAR_DDR4_0_BASEADDR,
                         (unsigned long)XPAR_DDR4_0_HIGHADDR -
                             (unsigned long)XPAR_DDR4_0_BASEADDR + 1,
                         __pgprot(_ARM64_DEVICE_nGnRE), 0);
}

static void* __ioremap(unsigned long phys_addr, size_t size, pgprot_t prot)
{
    static unsigned long map_base = PKMAP_START;
    void* ret_addr;

    ret_addr = (void*)map_base;
    __create_pgd_mapping(swapper_pg_dir, phys_addr, map_base, size, prot, 0);
    map_base += roundup(size, ARCH_PMD_SIZE);

    return ret_addr;
}

void* ioremap_nc(unsigned long phys_addr, size_t size)
{
    return __ioremap(phys_addr, size, __pgprot(_ARM64_PG_NORMAL_NC));
}

int pgd_new(pgdir_t* pgd)
{
    phys_addr_t pgd_phys;

    pgd_phys = alloc_pages(1, ZONE_PS_DDR);
    if (!pgd_phys) return ENOMEM;

    pgd->phys_addr = pgd_phys;
    pgd->vir_addr = (pde_t*)__va(pgd_phys);

    memset(pgd->vir_addr, 0, ARCH_PG_SIZE);

    return 0;
}

int pgd_writemap(pgdir_t* pgd, phys_addr_t phys_addr, unsigned long vir_addr,
                 size_t length, pgprot_t prot)
{
    __create_pgd_mapping(pgd->vir_addr, phys_addr, vir_addr, length, prot,
                         NO_BLOCK_MAPPINGS | NO_CONT_MAPPINGS);

    return 0;
}

void pgd_unmap_memory(pgdir_t* pgd, unsigned long vir_addr, size_t length)
{
    Xil_AssertVoid(vir_addr % ARCH_PG_SIZE == 0);
    Xil_AssertVoid(length % ARCH_PG_SIZE == 0);

    while (length > 0) {
        pte_t* pte;

        if (pt_follow(pgd, vir_addr, &pte) == 0) {
            pte_clear(pte);
        }

        length -= ARCH_PG_SIZE;
        vir_addr += ARCH_PG_SIZE;
    }
}

int pgd_free(pgdir_t* pgd)
{
    free_mem(pgd->phys_addr, sizeof(pde_t) * ARCH_VM_DIR_ENTRIES);
    return 0;
}

void switch_address_space(pgdir_t* pgd)
{
    flush_tlb();
    write_sysreg(pgd->phys_addr, ttbr1_el1);
    isb();
}

static int pt_follow(pgdir_t* pgd, unsigned long vir_addr, pte_t** ptepp)
{
    pde_t* pde;
    pud_t* pude;
    pmd_t* pmde;
    pte_t* pte;

    pde = pgd_offset(pgd->vir_addr, vir_addr);
    if (pde_none(*pde)) return EFAULT;

    pude = pud_offset(pde, vir_addr);
    if (pude_none(*pude)) return EFAULT;

    pmde = pmd_offset(pude, vir_addr);
    if (pmde_none(*pmde)) return EFAULT;

    pte = pte_offset(pmde, vir_addr);
    if (!pte_present(*pte)) return EFAULT;

    *ptepp = pte;
    return 0;
}

int pgd_va2pa(pgdir_t* pgd, unsigned long vir_addr, phys_addr_t* phys_addr)
{
    pte_t* pte;
    int retval;

    /* Kernel address? */
    if (vir_addr < (1UL << VA_BITS)) {
        *phys_addr = __pa((void*)vir_addr);
        return 0;
    }

    if ((retval = pt_follow(pgd, vir_addr, &pte)) != 0) {
        return retval;
    }

    *phys_addr = (pte_pfn(*pte) << ARCH_PG_SHIFT) + (vir_addr % ARCH_PG_SIZE);
    return 0;
}

size_t pgd_va2pa_range(pgdir_t* pgd, unsigned long vir_addr,
                       phys_addr_t* phys_addr, size_t bytes)
{
    phys_addr_t phys, next_phys;
    size_t len;

    if (pgd_va2pa(pgd, vir_addr, &phys) != 0) return 0;

    if (phys_addr) *phys_addr = phys;

    len = ARCH_PG_SIZE - (vir_addr % ARCH_PG_SIZE);
    vir_addr += len;
    next_phys = phys + len;

    while (len < bytes) {
        if (pgd_va2pa(pgd, vir_addr, &phys) != 0) break;

        if (next_phys != phys) break;

        len += ARCH_PG_SIZE;
        vir_addr += ARCH_PG_SIZE;
        next_phys += ARCH_PG_SIZE;
    }

    return min(bytes, len);
}

#ifndef __PAGETABLE_PUD_FOLDED
static void pud_free(pud_t* pud)
{
    free_mem(__pa(pud), sizeof(pud_t) * ARCH_VM_PUD_ENTRIES);
}
#else
static inline void pud_free(pud_t* pud) {}
#endif

#ifndef __PAGETABLE_PMD_FOLDED
static void pmd_free(pmd_t* pmd)
{
    free_mem(__pa(pmd), sizeof(pmd_t) * ARCH_VM_PMD_ENTRIES);
}
#else
static inline void pmd_free(pmd_t* pmd) {}
#endif

static void pt_free(pte_t* pt)
{
    free_mem(__pa(pt), sizeof(pte_t) * ARCH_VM_PT_ENTRIES);
}

void pt_free_range(pmd_t* pt)
{
    pte_t* pte = pte_offset(pt, 0);
    pmde_clear(pt);
    pt_free(pte);
}

void pmd_free_range(pud_t* pmd, unsigned long addr, unsigned long end,
                    unsigned long floor, unsigned long ceiling)
{
    unsigned long start = addr;
    unsigned long next;
    pmd_t* pmde = pmd_offset(pmd, addr);

    do {
        next = pmd_addr_end(addr, end);

        if (pmde_none(*pmde) || pmde_bad(*pmde)) {
            pmde++;
            addr = next;
            continue;
        }
        pt_free_range(pmde);

        pmde++;
        addr = next;
    } while (addr != end);

    start &= ARCH_PUD_MASK;
    if (start < floor) return;

    if (ceiling) {
        ceiling &= ARCH_PUD_MASK;
    }

    if (end > ceiling) return;

    pmde = pmd_offset(pmd, 0);
    pude_clear(pmd);
    pmd_free(pmde);
}

void pud_free_range(pde_t* pud, unsigned long addr, unsigned long end,
                    unsigned long floor, unsigned long ceiling)
{
    unsigned long start = addr;
    unsigned long next;
    pud_t* pude = pud_offset(pud, addr);

    do {
        next = pud_addr_end(addr, end);

        if (pude_none(*pude) || pude_bad(*pude)) {
            pude++;
            addr = next;
            continue;
        }
        pmd_free_range(pude, addr, end, floor, ceiling);

        pude++;
        addr = next;
    } while (addr != end);

    start &= ARCH_PGD_MASK;
    if (start < floor) return;

    if (ceiling) {
        ceiling &= ARCH_PGD_MASK;
    }

    if (end > ceiling) return;

    pude = pud_offset(pud, 0);
    pde_clear(pud);
    pud_free(pude);
}

void pgd_free_range(pgdir_t* pgd, unsigned long addr, unsigned long end,
                    unsigned long floor, unsigned long ceiling)
{
    pde_t* pde = pgd_offset(pgd->vir_addr, addr);
    unsigned long next;

    do {
        next = pgd_addr_end(addr, end);

        if (pde_none(*pde) || pde_bad(*pde)) {
            pde++;
            addr = next;
            continue;
        }
        pud_free_range(pde, addr, end, floor, ceiling);

        pde++;
        addr = next;
    } while (addr != end);
}
