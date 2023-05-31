#include "xparameters.h"
#include "xil_exception.h"
#include "xdebug.h"
#include "xil_io.h"
#include "xscugic.h"

#include <types.h>
#include <intr.h>
#include <barrier.h>

#define INTC_DEVICE_ID XPAR_SCUGIC_SINGLE_DEVICE_ID
#define INTC           XScuGic
#define INTC_HANDLER   XScuGic_InterruptHandler

static INTC Intc;

int intr_setup_cpu(void)
{
    int Status;
    XScuGic_Config* IntcConfig;
    INTC* IntcInstancePtr = &Intc;

    IntcConfig = XScuGic_LookupConfig(INTC_DEVICE_ID);
    if (NULL == IntcConfig) {
        return XST_FAILURE;
    }

    Status = XScuGic_CfgInitialize(IntcInstancePtr, IntcConfig,
                                   IntcConfig->CpuBaseAddress);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    /* Enable interrupts from the hardware */
    Xil_ExceptionInit();
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
                                 (Xil_ExceptionHandler)INTC_HANDLER,
                                 (void*)IntcInstancePtr);

    return XST_SUCCESS;
}

int intr_setup_irq(u16 intr_id, int trigger_type, irq_handler_t handler,
                   void* cb_data)
{
    INTC* IntcInstancePtr = &Intc;

    XScuGic_SetPriorityTriggerType(IntcInstancePtr, intr_id, 0xA0,
                                   trigger_type);
    return XScuGic_Connect(IntcInstancePtr, intr_id,
                           (Xil_InterruptHandler)handler, cb_data);
}

void intr_disconnect_irq(u16 intr_id) { XScuGic_Disconnect(&Intc, intr_id); }

int intr_enable_irq(u16 intr_id)
{
    INTC* IntcInstancePtr = &Intc;
    XScuGic_Enable(IntcInstancePtr, intr_id);
    return 0;
}

int intr_disable_irq(u16 intr_id)
{
    INTC* IntcInstancePtr = &Intc;
    XScuGic_Disable(IntcInstancePtr, intr_id);
    return 0;
}

void intr_handle_irq(void)
{
    INTC* IntcInstancePtr = &Intc;
    INTC_HANDLER(IntcInstancePtr);
}
