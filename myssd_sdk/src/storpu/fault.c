#include <signal.h>

#include <types.h>
#include <storpu/vm.h>
#include <esr.h>
#include <sysreg.h>
#include <stackframe.h>
#include <storpu/thread.h>
#include <utils.h>

struct fault_info {
    int (*fn)(int in_kernel, unsigned long far, unsigned int esr,
              struct stackframe* frame);
    int sig;
    const char* name;
};

static const struct fault_info fault_info[];

static inline const struct fault_info* esr_to_fault_info(unsigned int esr)
{
    return fault_info + (esr & ESR_ELx_FSC);
}

static int is_el0_instruction_abort(unsigned int esr)
{
    return ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_LOW;
}

static int is_write_abort(unsigned int esr)
{
    return (esr & ESR_ELx_WNR) && !(esr & ESR_ELx_CM);
}

static int do_bad(int in_kernel, unsigned long far, unsigned int esr,
                  struct stackframe* frame)
{
    return 1;
}

static int do_page_fault(int in_kernel, unsigned long far, unsigned int esr,
                         struct stackframe* frame)
{
    unsigned int vr_flags;
    unsigned int mm_flags = FAULT_FLAG_INTERRUPTIBLE;
    unsigned long addr = far;
    int handled;

    if (!in_kernel) mm_flags |= FAULT_FLAG_USER;

    if (is_el0_instruction_abort(esr)) {
        vr_flags = RF_EXEC;
        mm_flags |= FAULT_FLAG_INSTRUCTION;
    } else if (is_write_abort(esr)) {
        vr_flags = RF_WRITE;
        mm_flags |= FAULT_FLAG_WRITE;
    } else {
        vr_flags = RF_READ | RF_WRITE | RF_EXEC;
    }

    handled = vm_handle_page_fault(addr, mm_flags, vr_flags, frame);

    if (!handled) {
        printk(
            "unhandled memory abort in userspace, pc: %lx, esr: %x, far: %lx",
            frame->pc, esr, far);
        thread_exit(-1);
    }

    return 0;
}

static int do_translation_fault(int in_kernel, unsigned long far,
                                unsigned int esr, struct stackframe* frame)
{
    return do_page_fault(in_kernel, far, esr, frame);
}

static const struct fault_info fault_info[] = {
    {do_bad, SIGKILL, "ttbr address size fault"},
    {do_bad, SIGKILL, "level 1 address size fault"},
    {do_bad, SIGKILL, "level 2 address size fault"},
    {do_bad, SIGKILL, "level 3 address size fault"},
    {do_translation_fault, SIGSEGV, "level 0 translation fault"},
    {do_translation_fault, SIGSEGV, "level 1 translation fault"},
    {do_translation_fault, SIGSEGV, "level 2 translation fault"},
    {do_translation_fault, SIGSEGV, "level 3 translation fault"},
    {do_bad, SIGKILL, "unknown 8"},
    {do_page_fault, SIGSEGV, "level 1 access flag fault"},
    {do_page_fault, SIGSEGV, "level 2 access flag fault"},
    {do_page_fault, SIGSEGV, "level 3 access flag fault"},
    {do_bad, SIGKILL, "unknown 12"},
    {do_page_fault, SIGSEGV, "level 1 permission fault"},
    {do_page_fault, SIGSEGV, "level 2 permission fault"},
    {do_page_fault, SIGSEGV, "level 3 permission fault"},
    {do_bad, SIGBUS, "synchronous external abort"},
    {do_bad, SIGSEGV, "synchronous tag check fault"},
    {do_bad, SIGKILL, "unknown 18"},
    {do_bad, SIGKILL, "unknown 19"},
    {do_bad, SIGKILL, "level 0 (translation table walk)"},
    {do_bad, SIGKILL, "level 1 (translation table walk)"},
    {do_bad, SIGKILL, "level 2 (translation table walk)"},
    {do_bad, SIGKILL, "level 3 (translation table walk)"},
    {do_bad, SIGBUS, "synchronous parity or ECC error"},
    {do_bad, SIGKILL, "unknown 25"},
    {do_bad, SIGKILL, "unknown 26"},
    {do_bad, SIGKILL, "unknown 27"},
    {do_bad, SIGKILL,
     "level 0 synchronous parity error (translation table walk)"},
    {do_bad, SIGKILL,
     "level 1 synchronous parity error (translation table walk)"},
    {do_bad, SIGKILL,
     "level 2 synchronous parity error (translation table walk)"},
    {do_bad, SIGKILL,
     "level 3 synchronous parity error (translation table walk)"},
    {do_bad, SIGKILL, "unknown 32"},
    {do_bad, SIGBUS, "alignment fault"},
    {do_bad, SIGKILL, "unknown 34"},
    {do_bad, SIGKILL, "unknown 35"},
    {do_bad, SIGKILL, "unknown 36"},
    {do_bad, SIGKILL, "unknown 37"},
    {do_bad, SIGKILL, "unknown 38"},
    {do_bad, SIGKILL, "unknown 39"},
    {do_bad, SIGKILL, "unknown 40"},
    {do_bad, SIGKILL, "unknown 41"},
    {do_bad, SIGKILL, "unknown 42"},
    {do_bad, SIGKILL, "unknown 43"},
    {do_bad, SIGKILL, "unknown 44"},
    {do_bad, SIGKILL, "unknown 45"},
    {do_bad, SIGKILL, "unknown 46"},
    {do_bad, SIGKILL, "unknown 47"},
    {do_bad, SIGKILL, "TLB conflict abort"},
    {do_bad, SIGKILL, "Unsupported atomic hardware update fault"},
    {do_bad, SIGKILL, "unknown 50"},
    {do_bad, SIGKILL, "unknown 51"},
    {do_bad, SIGKILL, "implementation fault (lockdown abort)"},
    {do_bad, SIGBUS, "implementation fault (unsupported exclusive)"},
    {do_bad, SIGKILL, "unknown 54"},
    {do_bad, SIGKILL, "unknown 55"},
    {do_bad, SIGKILL, "unknown 56"},
    {do_bad, SIGKILL, "unknown 57"},
    {do_bad, SIGKILL, "unknown 58"},
    {do_bad, SIGKILL, "unknown 59"},
    {do_bad, SIGKILL, "unknown 60"},
    {do_bad, SIGKILL, "section domain fault"},
    {do_bad, SIGKILL, "page domain fault"},
    {do_bad, SIGKILL, "unknown 63"},
};

void do_mem_abort(int in_kernel, unsigned long far, unsigned int esr,
                  struct stackframe* frame)
{
    const struct fault_info* inf = esr_to_fault_info(esr);

    if (!inf->fn(in_kernel, far, esr, frame)) return;

    if (in_kernel) {
        panic("unhandled memory abort in kernel, pc: %lx, esr: %x, far: %lx",
              frame->pc, esr, far);
    } else {
        xil_printf(
            "kernel: memory abort in userspace, pc: %lx, esr: %x, far: %lx",
            frame->pc, esr, far);
    }
}

static void el1_abort(struct stackframe* frame, unsigned long esr)
{
    unsigned long far = read_sysreg(far_el1);
    do_mem_abort(TRUE, far, esr, frame);
}

void el1h_64_sync_handler(struct stackframe* frame)
{
    unsigned long esr = read_sysreg(esr_el1);

    switch (ESR_ELx_EC(esr)) {
    case ESR_ELx_EC_DABT_CUR:
    case ESR_ELx_EC_IABT_CUR:
        el1_abort(frame, esr);
        break;
    default:
        printk("unhandled sync abort in kernel, esr: %lx, pc: %lx\n",
               ESR_ELx_EC(esr), frame->pc);
        break;
    }
}
