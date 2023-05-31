#ifndef _STACKFRAME_H_
#define _STACKFRAME_H_

typedef unsigned long reg_t;

struct stackframe {
    reg_t regs[31];
    reg_t sp;
    reg_t pc;
    reg_t pstate;
    reg_t kernel_sp;
    reg_t orig_x0;

    reg_t stackframe[2];

    /* Current CPU */
    unsigned int cpu;
    unsigned int __unused0;
    unsigned long __unused1;
} __attribute__((aligned(16)));

#endif
