#ifndef _INTR_H_
#define _INTR_H_

#include <xil_exception.h>

#include <cpumask.h>

typedef void (*irq_handler_t)(void* data);

int intr_setup_cpu(void);

int intr_setup_irq(u16 intr_id, int trigger_type, irq_handler_t handler,
                   void* cb_data);
void intr_disconnect_irq(u16 intr_id);

int intr_enable_irq(u16 intr_id);
int intr_disable_irq(u16 intr_id);

void intr_ipi_send_mask(u16 irq, const struct cpumask* mask);

void intr_handle_irq(void);

#ifdef __UM__

static inline void local_irq_enable(void) {}
static inline void local_irq_disable(void) {}
static inline void local_irq_save(unsigned long* flags) {}
static inline void local_irq_restore(unsigned long flags) {}

#else

static inline void local_irq_enable(void) { Xil_ExceptionEnable(); }
static inline void local_irq_disable(void) { Xil_ExceptionDisable(); }

static inline void local_irq_save(unsigned long* flags)
{
    *flags = mfcpsr();
    local_irq_disable();
}

static inline void local_irq_restore(unsigned long flags)
{
    if (flags & XIL_EXCEPTION_IRQ)
        local_irq_disable();
    else
        local_irq_enable();
}

#endif

#endif
