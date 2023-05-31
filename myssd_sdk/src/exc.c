#include <xil_printf.h>

#include <types.h>
#include <esr.h>
#include <sysreg.h>
#include <stackframe.h>
#include <intr.h>
#include <utils.h>

static void __panic_unhandled(struct stackframe* frame, const char* vector,
                              unsigned int esr)
{
    xil_printf("Unhandled %s exception, ESR 0x%08x, PC %lx\n", vector, esr,
               frame->pc);
    while (TRUE)
        ;
    panic("Unhandled exception");
}

#define UNHANDLED(el, regsize, vector)                                 \
    void el##_##regsize##_##vector##_handler(struct stackframe* frame) \
    {                                                                  \
        const char* desc = #regsize "-bit " #el " " #vector;           \
        __panic_unhandled(frame, desc, read_sysreg(esr_el1));          \
    }

UNHANDLED(el1t, 64, sync)
UNHANDLED(el1t, 64, irq)
UNHANDLED(el1t, 64, fiq)
UNHANDLED(el1t, 64, error)

UNHANDLED(el0t, 64, sync)
UNHANDLED(el0t, 64, irq)
UNHANDLED(el0t, 64, fiq)
UNHANDLED(el0t, 64, error)

UNHANDLED(el0t, 32, sync)
UNHANDLED(el0t, 32, irq)
UNHANDLED(el0t, 32, fiq)
UNHANDLED(el0t, 32, error)

UNHANDLED(el1h, 64, fiq)
UNHANDLED(el1h, 64, error)

void el1h_64_irq_handler(struct stackframe* frame) { intr_handle_irq(); }
