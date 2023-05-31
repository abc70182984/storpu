#include "xparameters.h"
#include "xipipsu.h"

#include <config.h>
#include <types.h>
#include <intr.h>
#include <cpulocals.h>
#include <storpu.h>

#define IPI_IRQ_BASE 8

enum ipi_msg_type {
    IPI_RESCHEDULE,
    IPI_STORPU_COMPLETION,
    NR_IPI,
};

static XIpiPsu ipi_psu_inst;

static u16 ipi_intr_ids[NR_IPI];

static void ipi_psu_handler(void* callback)
{
    XIpiPsu* inst_ptr = (XIpiPsu*)callback;
    u32 src_mask;

    src_mask = XIpiPsu_GetInterruptStatus(inst_ptr);
    XIpiPsu_ClearInterruptStatus(inst_ptr, src_mask);
}

static void ipi_handler(void* callback)
{
    int irq = (unsigned int)(unsigned long)callback;
    int ipinr = irq - IPI_IRQ_BASE;

    switch (ipinr) {
    case IPI_RESCHEDULE:
        break;
    case IPI_STORPU_COMPLETION:
        handle_storpu_completion();
        break;
    default:
        break;
    }
}

int ipi_setup_cpu(void)
{
    int i, status;
    static int initialized = FALSE;

    if (!initialized) {
        XIpiPsu_Config* config;
        config = XIpiPsu_LookupConfig(XPAR_XIPIPSU_0_DEVICE_ID);
        if (config == NULL) {
            xil_printf("IPI PSU not found\r\n");
            return XST_FAILURE;
        }

        status =
            XIpiPsu_CfgInitialize(&ipi_psu_inst, config, config->BaseAddress);
        if (status != XST_SUCCESS) {
            xil_printf("IPI config failed\r\n");
            return XST_FAILURE;
        }

        for (i = 0; i < NR_IPI; i++)
            ipi_intr_ids[i] = IPI_IRQ_BASE + i;

        initialized = TRUE;
    }

    for (i = 0; i < NR_IPI; i++) {
        status =
            intr_setup_irq(ipi_intr_ids[i], 0x3, (irq_handler_t)ipi_handler,
                           (void*)(unsigned long)ipi_intr_ids[i]);
        if (status != XST_SUCCESS) {
            xil_printf("Failed to set up IPI%d IRQ\r\n", i);
            return XST_FAILURE;
        }

        intr_enable_irq(ipi_intr_ids[i]);
    }

    status = intr_setup_irq(XPAR_XIPIPSU_0_INT_ID, 0x1,
                            (irq_handler_t)ipi_psu_handler, &ipi_psu_inst);
    if (status != XST_SUCCESS) {
        xil_printf("Failed to set up IPI PSU IRQ\r\n");
        return XST_FAILURE;
    }

    intr_enable_irq(XPAR_XIPIPSU_0_INT_ID);

    /* Enable IPI from RPUs. */
    XIpiPsu_InterruptEnable(&ipi_psu_inst, XIPIPSU_ALL_MASK);
    XIpiPsu_ClearInterruptStatus(&ipi_psu_inst, XIPIPSU_ALL_MASK);

    return XST_SUCCESS;
}

static inline void smp_cross_call(const struct cpumask* target,
                                  unsigned int ipinr)
{
    intr_ipi_send_mask(ipi_intr_ids[ipinr], target);
}

void smp_send_reschedule(int cpu)
{
    smp_cross_call(cpumask_of(cpu), IPI_RESCHEDULE);
}

void smp_send_storpu_completion(void)
{
    smp_cross_call(cpumask_of(FTL_CPU_ID), IPI_STORPU_COMPLETION);
}
