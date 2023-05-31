#include "xparameters.h"
#include "xil_exception.h"
#include "xdebug.h"
#include "xil_io.h"
#include "xscugic.h"

#include <types.h>
#include <intr.h>
#include <barrier.h>
#include <cpulocals.h>
#include <cpumask.h>
#include <smp.h>
#include <spinlock.h>

#define INTC_DEVICE_ID XPAR_SCUGIC_SINGLE_DEVICE_ID
#define INTC           XScuGic
#define INTC_HANDLER   XScuGic_InterruptHandler

static DEFINE_CPULOCAL(INTC, Intc);
static DEFINE_CPULOCAL(XScuGic_Config, IntcConfig);

#define LOCAL_INTC get_cpulocal_var_ptr(Intc)

static u8 gic_cpu_map[8];

static DEFINE_SPINLOCK(intc_lock);

static inline void gic_select_cpu(void)
{
    XScuGic_SetCpuID(get_cpulocal_var(cpu_phys_id));
}

static u8 gic_get_cpumask(void)
{
    INTC* IntcInstancePtr = LOCAL_INTC;
    u32 mask, i;

    for (i = mask = 0; i < 32; i += 4) {
        mask = XScuGic_DistReadReg(IntcInstancePtr, 0x800 + i);
        mask |= mask >> 16;
        mask |= mask >> 8;
        if (mask) break;
    }

    return mask;
}

int intr_setup_cpu(void)
{
    int Status;
    XScuGic_Config* config;
    XScuGic_Config* local_config = get_cpulocal_var_ptr(IntcConfig);
    INTC* IntcInstancePtr = LOCAL_INTC;

    config = XScuGic_LookupConfig(INTC_DEVICE_ID);
    if (NULL == config) {
        return XST_FAILURE;
    }

    *local_config = *config;

    spin_lock(&intc_lock);

    gic_select_cpu();
    Status = XScuGic_CfgInitialize(IntcInstancePtr, local_config,
                                   local_config->CpuBaseAddress);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    spin_unlock(&intc_lock);

    gic_cpu_map[cpuid] = gic_get_cpumask();

    /* Enable interrupts from the hardware */
    Xil_ExceptionInit();

    return XST_SUCCESS;
}

int intr_setup_irq(u16 intr_id, int trigger_type, irq_handler_t handler,
                   void* cb_data)
{
    INTC* IntcInstancePtr = LOCAL_INTC;

    spin_lock(&intc_lock);
    XScuGic_SetPriorityTriggerType(IntcInstancePtr, intr_id, 0xA0,
                                   trigger_type);
    spin_unlock(&intc_lock);

    return XScuGic_Connect(IntcInstancePtr, intr_id,
                           (Xil_InterruptHandler)handler, cb_data);
}

void intr_disconnect_irq(u16 intr_id)
{
    INTC* IntcInstancePtr = LOCAL_INTC;
    XScuGic_Disconnect(IntcInstancePtr, intr_id);
}

int intr_enable_irq(u16 intr_id)
{
    INTC* IntcInstancePtr = LOCAL_INTC;

    spin_lock(&intc_lock);
    gic_select_cpu();
    XScuGic_Enable(IntcInstancePtr, intr_id);
    spin_unlock(&intc_lock);

    return 0;
}

int intr_disable_irq(u16 intr_id)
{
    INTC* IntcInstancePtr = LOCAL_INTC;

    spin_lock(&intc_lock);
    gic_select_cpu();
    XScuGic_Disable(IntcInstancePtr, intr_id);
    spin_unlock(&intc_lock);

    return 0;
}

void intr_ipi_send_mask(u16 irq, const struct cpumask* mask)
{
    INTC* IntcInstancePtr = LOCAL_INTC;
    int cpu;
    unsigned long map = 0;

    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        if (cpumask_test_cpu(mask, cpu)) map |= gic_cpu_map[cpu];
    }

    dmb(ishst);

    XScuGic_SoftwareIntr(IntcInstancePtr, irq, map);
}

void intr_handle_irq(void)
{
    INTC* IntcInstancePtr = LOCAL_INTC;
    INTC_HANDLER(IntcInstancePtr);
}
