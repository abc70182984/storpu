    #include <sconst.h>

    .section .text

    .globl vectors

    .macro kernel_ventry, el:req, ht:req, regsize:req, label:req
    .align 7
    .Lventry_start\@:
    sub sp, sp, #STACKFRAME_SIZE
    b   el\el\ht\()_\regsize\()_\label
    .org .Lventry_start\@ + 128
    .endm

    .macro  kernel_entry, el, regsize = 64
    .if     \regsize == 32
    mov     w0, w0
    .endif
    stp     x0, x1, [sp, #16 * 0]
    stp     x2, x3, [sp, #16 * 1]
    stp     x4, x5, [sp, #16 * 2]
    stp     x6, x7, [sp, #16 * 3]
    stp     x8, x9, [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]

    add     x21, sp, #STACKFRAME_SIZE
    mrs     x22, elr_el1
    mrs     x23, spsr_el1
    stp     lr, x21, [sp, #LRREG]
    stp     x22, x23, [sp, #PCREG]

    stp     x29, x22, [sp, #P_STACKFRAME]
    add     x29, sp, #P_STACKFRAME
    .endm

    .macro kernel_exit, el
    msr	daifset, #0xf

    ldp     x21, x22, [sp, #PCREG]

    msr     elr_el1, x21
    msr     spsr_el1, x22
    ldp     x0, x1, [sp, #16 * 0]
    ldp     x2, x3, [sp, #16 * 1]
    ldp     x4, x5, [sp, #16 * 2]
    ldp     x6, x7, [sp, #16 * 3]
    ldp     x8, x9, [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]

    ldr     lr, [sp, #LRREG]
    add     sp, sp, #STACKFRAME_SIZE
    eret
    .endm


    .align 11
vectors:
    kernel_ventry   1, t, 64, sync
    kernel_ventry   1, t, 64, irq
    kernel_ventry   1, t, 64, fiq
    kernel_ventry   1, t, 64, error

    kernel_ventry   1, h, 64, sync
    kernel_ventry   1, h, 64, irq
    kernel_ventry   1, h, 64, fiq
    kernel_ventry   1, h, 64, error

    kernel_ventry   0, t, 64, sync
    kernel_ventry   0, t, 64, irq
    kernel_ventry   0, t, 64, fiq
    kernel_ventry   0, t, 64, error

    kernel_ventry   0, t, 32, sync
    kernel_ventry   0, t, 32, irq
    kernel_ventry   0, t, 32, fiq
    kernel_ventry   0, t, 32, error

    .macro entry_handler el:req, ht:req, regsize:req, label:req
    el\el\ht\()_\regsize\()_\label:
    kernel_entry \el, \regsize

    mov x0, sp

    bl  el\el\ht\()_\regsize\()_\label\()_handler

    b   resume_kernel
    .endm

    entry_handler	1, t, 64, sync
    entry_handler	1, t, 64, irq
    entry_handler	1, t, 64, fiq
    entry_handler	1, t, 64, error

    entry_handler	1, h, 64, sync
    entry_handler	1, h, 64, irq
    entry_handler	1, h, 64, fiq
    entry_handler	1, h, 64, error

    entry_handler	0, t, 64, sync
    entry_handler	0, t, 64, irq
    entry_handler	0, t, 64, fiq
    entry_handler	0, t, 64, error

    entry_handler	0, t, 32, sync
    entry_handler	0, t, 32, irq
    entry_handler	0, t, 32, fiq
    entry_handler	0, t, 32, error

resume_kernel:
    kernel_exit 1
