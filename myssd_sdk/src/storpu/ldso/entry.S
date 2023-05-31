    .section .text

    .align  4
    .globl  ldso_bind_entry
    .type   ldso_bind_entry,@function
ldso_bind_entry:
    stp     x8, x9, [sp, #-(80+8*16)]!
    stp     x6, x7, [sp,  #16]
    stp     x4, x5, [sp,  #32]
    stp     x2, x3, [sp,  #48]
    stp     x0, x1, [sp,  #64]

    stp     q0, q1, [sp, #(80+0*16)]
    stp     q2, q3, [sp, #(80+2*16)]
    stp     q4, q5, [sp, #(80+4*16)]
    stp     q6, q7, [sp, #(80+6*16)]

    ldr     x0, [x16, #-8]
    ldr     x1, [sp, 80+8*16]

    sub     x1, x1, x16
    add     x1, x1, x1, lsl #1
    lsl     x1, x1, #3
    sub     x1, x1, #((3*8)<<3)
    lsr     x1, x1, #3

    bl      ldso_bind
    mov     x16, x0

    ldp     q0, q1, [sp, #(80+0*16)]
    ldp     q2, q3, [sp, #(80+2*16)]
    ldp     q4, q5, [sp, #(80+4*16)]
    ldp     q6, q7, [sp, #(80+6*16)]

    ldp     x0, x1, [sp, #64]
    ldp     x2, x3, [sp, #48]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #16]
    ldp     x8, x9, [sp], #(80+8*16)

    ldp     x17, lr, [sp], #16
    br      x16

    .align  4
    .globl  ldso_tlsdesc_static
    .type   ldso_tlsdesc_static,@function
ldso_tlsdesc_static:
    ldr     x0, [x0, #8]
    ret
