    #define __ASSEMBLY__
    #include <config.h>
    #include <sconst.h>
    #include <const.h>
    #include <sysreg.h>
    #include <page.h>

    .globl _head
    .globl secondary_entry
    .globl cpu_release_addr

    .section	".head.text","ax"
_head:
    b _start

    .section .text
_start:
    mov x0, xzr
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    mov x4, xzr
    mov x5, xzr
    mov x6, xzr
    mov x7, xzr
    mov x8, xzr
    mov x9, xzr
    mov x10, xzr
    mov x11, xzr
    mov x12, xzr
    mov x13, xzr
    mov x14, xzr
    mov x15, xzr
    mov x16, xzr
    mov x17, xzr
    mov x18, xzr
    mov x19, xzr
    mov x20, xzr
    mov x21, xzr
    mov x22, xzr
    mov x23, xzr
    mov x24, xzr
    mov x25, xzr
    mov x26, xzr
    mov x27, xzr
    mov x28, xzr
    mov x29, xzr
    mov x30, xzr

    mrs x0, mpidr_el1
    and x0, x0, #0xff
    cbz x0, primary_entry

    adr_l   x1, cpu_release_addr
    add     x1, x1, x0, lsl #3
secondary_holding_pen:
    wfe
    ldr x2, [x1]
    cbz x2, secondary_holding_pen
    br  x2

primary_entry:
    bl  setup_el
    bl  __create_page_tables

    bl  __cpu_setup
    bl  __primary_switch

setup_el:
    mrs     x0, CurrentEL
    cmp     x0, #CurrentEL_EL3
    b.eq    init_el3

init_el1:
    wfe
    b   init_el1

init_el3:
    mrs      x0, S3_1_c15_c2_1
    orr      x0, x0, #(1 << 6)
    msr      S3_1_c15_c2_1, x0
    isb

    // Drop to EL1
    mov_q   x0, SCTLR_EL1_RES1
    msr     sctlr_el1, x0

    mrs     x0, scr_el3
    orr     x0, x0, #BIT(10)
    msr     scr_el3, x0

    mov     x0, #(PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT | PSR_MODE_EL1h)
    msr     spsr_el3, x0
    msr     elr_el3, lr
    eret

    .macro  create_table_entry, tbl, virt, shift, ptrs, tmp1, tmp2
    lsr     \tmp1, \virt, #\shift
    and     \tmp1, \tmp1, #\ptrs - 1
    add     \tmp2, \tbl, #ARCH_PG_SIZE
    orr     \tmp2, \tmp2, #_ARM64_PMD_TYPE_TABLE
    str     \tmp2, [\tbl, \tmp1, lsl #3]
    add     \tbl, \tbl, #ARCH_PG_SIZE
    .endm

    .macro	create_pgd_entry, tbl, virt, tmp1, tmp2
    create_table_entry \tbl, \virt, ARCH_PGD_SHIFT, ARCH_VM_DIR_ENTRIES, \tmp1, \tmp2
#if INIT_PGTABLE_LEVELS > 3
    create_table_entry \tbl, \virt, ARCH_PUD_SHIFT, ARCH_VM_PUD_ENTRIES, \tmp1, \tmp2
#endif
#if INIT_PGTABLE_LEVELS > 2
    create_table_entry \tbl, \virt, SWAPPER_TABLE_SHIFT, ARCH_VM_PT_ENTRIES, \tmp1, \tmp2
#endif
    .endm

    .macro  create_block_map, tbl, flags, phys, start, end
    lsr     \phys, \phys, #SWAPPER_BLOCK_SHIFT
    lsr     \start, \start, #SWAPPER_BLOCK_SHIFT
    and     \start, \start, #ARCH_VM_PT_ENTRIES - 1
    orr     \phys, \flags, \phys, lsl #SWAPPER_BLOCK_SHIFT
    lsr     \end, \end, #SWAPPER_BLOCK_SHIFT
    and     \end, \end, #ARCH_VM_PT_ENTRIES - 1
1:  str     \phys, [\tbl, \start, lsl #3]
    add     \start, \start, #1
    add     \phys, \phys, #SWAPPER_BLOCK_SIZE
    cmp     \start, \end
    b.ls    1b
    .endm

__create_page_tables:
    mov     x28, lr

    adrp    x0, idmap_pg_dir
    ldr     x1, =(IDMAP_DIR_SIZE + INIT_DIR_SIZE)

1:  stp     xzr, xzr, [x0], #16
    stp     xzr, xzr, [x0], #16
    stp     xzr, xzr, [x0], #16
    stp     xzr, xzr, [x0], #16
    subs    x1, x1, #64
    b.ne    1b

    mov     x7,  (_ARM64_PMD_TYPE_SECT|_ARM64_SECT_S|_ARM64_SECT_AF)

    adrp    x0, idmap_pg_dir
    mov     x3, xzr
    mov     x5, xzr // VA == PA == 0
    create_pgd_entry x0, x3, x5, x6
    adrp    x6, _end
    create_block_map x0, x7, x3, x5, x6

    ret     x28

__cpu_setup:
    tlbi    vmalle1
    dsb     nsh

    mov x1, #3 << 20
    msr cpacr_el1, x1
    mov x1, #1 << 12
    msr mdscr_el1, x1
    isb

    mair    .req    x17
    tcr     .req    x16

    mov_q   mair, MAIR_EL1_SET
    mov_q   tcr, TCR_TxSZ(VA_BITS) | TCR_CACHE_FLAGS | TCR_SMP_FLAGS | \
    TCR_TG_FLAGS | TCR_ASID16 | TCR_TBI0 | TCR_A1

    mrs     x5, ID_AA64MMFR0_EL1
    ubfx    x5, x5, #ID_AA64MMFR0_PARANGE_SHIFT, #3
    mov     x6, #ID_AA64MMFR0_PARANGE_MAX
    cmp     x5, x6
    csel    x5, x6, x5, hi
    bfi     tcr, x5, #TCR_IPS_SHIFT, #3

    msr     mair_el1, mair
    msr     tcr_el1, tcr

    mov_q   x0, INIT_SCTLR_EL1_MMU_ON
    ret

    .unreq  mair
    .unreq  tcr

__enable_paging:
    adrp    x1, idmap_pg_dir
    msr     ttbr0_el1, x1
    isb
    msr     sctlr_el1, x0
    isb
    ic  iallu
    dsb nsh
    isb
    ret

__primary_switch:
    bl  __enable_paging

    ldr x8, =__primary_switched
    br  x8

__primary_switched:
    adr_l   x5, __el1_stack
    mov     sp, x5

    adr_l   x8, vectors
    msr     vbar_el1, x8
    isb

    mov     x5, xzr
    msr     tpidr_el1, x5

    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    adr_l   x0, __bss_start__
    mov     w1, wzr
    adr_l   x2, __bss_end__
    sub     x2, x2, x0
    bl      memset
    dsb     ishst

    ldp     x29, x30, [sp], #16
    bl      primary_main

secondary_entry:
    bl  setup_el
    bl  __cpu_setup
    bl  __enable_paging

    ldr x8, =__secondary_switched
    br  x8

__secondary_switched:
    adr_l   x5, vectors
    msr     vbar_el1, x5
    isb

    adr_l   x0, secondary_data
    ldr     x2, [x0, P_STACK_POINTER]
    mov     sp, x2
    ldr     x2, [x0, P_CPU_OFFSET]
    msr     tpidr_el1, x2

    b   smp_boot_ap

__secondary_park:
    b __secondary_park

    .section .data
    .align 4
cpu_release_addr:
    .zero (NR_CPUS << 3)

    .globl k_stacks_start, k_stacks_end
    .balign K_STACK_SIZE
k_stacks_start:
    .space 2 * K_STACK_SIZE * NR_CPUS
k_stacks_end:
