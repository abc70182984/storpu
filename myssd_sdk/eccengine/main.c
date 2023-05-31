#include "xparameters.h"
#include <stdio.h>
#include <stdlib.h>
#include "xil_printf.h"
#include "xil_mmu.h"
#include "sleep.h"
#include "xil_cache.h"
#include <errno.h>

#include <const.h>
#include <barrier.h>
#include <ecc_config.h>
#include <flash_config.h>
#include <ecc.h>
#include <proto.h>
#include <ringq.h>
#include <dma.h>

#include "bch_engine.h"

#define PS_DDR_LOW_BASE_ADDR 0x1100000UL
#define PS_DDR_LOW_LIMIT     0x80000000UL /* 2G */

#define NR_BCH_DECODER 1
#define BDRING_SIZE    0x2000

static u16 bch_dma_ids[NR_BCH_DECODER] = {
#if USE_HARDWARE_ECC
    XPAR_AXI_DMA_0_DEVICE_ID,
#else
    0,
#endif
};

static struct ringq ecc_ringq;
static int send_ipi;

static void handle_ecc_task(struct bch_engine* engine, struct ecc_task* task);

static struct ecc_task* dequeue_request(struct bch_engine* engine)
{
    uint32_t elem;
    struct ecc_task* task = NULL;

    ringq_read_avail_tail(&ecc_ringq);

    if (ringq_get_avail(&ecc_ringq, &elem)) {
        task = (struct ecc_task*)(uintptr_t)elem;
        dma_sync_single_for_cpu(task, sizeof(*task), DMA_FROM_DEVICE);
        dma_sync_single_for_cpu((void*)(uintptr_t)task->data, task->length,
                                DMA_FROM_DEVICE);

        /* For correction, we also need to invalidate the code buffer. */
        if (task->type == ECT_CORRECT)
            dma_sync_single_for_cpu((void*)(uintptr_t)task->code,
                                    task->code_length, DMA_FROM_DEVICE);
    }

    return task;
}

static void enqueue_response(struct ecc_task* task)
{
    Xil_AssertVoid(task->completed);

    dma_sync_single_for_device(task, sizeof(*task), DMA_TO_DEVICE);

    if (task->type == ECT_CORRECT)
        dma_sync_single_for_device((void*)(uintptr_t)task->data, task->length,
                                   DMA_TO_DEVICE);

    if (task->type == ECT_CALC)
        dma_sync_single_for_device((void*)(uintptr_t)task->code,
                                   task->code_length, DMA_TO_DEVICE);

    ringq_add_used(&ecc_ringq, (uint32_t)(uintptr_t)task);
    ringq_write_used_tail(&ecc_ringq);
    send_ipi = TRUE;
}

static void notify_task_complete(struct ecc_task* task)
{
    task->completed = TRUE;
    enqueue_response(task);
}

static void handle_ecc_task(struct bch_engine* engine, struct ecc_task* task)
{
    int r;
    int status;

    switch (task->type) {
    case ECT_CALC:
        r = bch_engine_calculate(engine, (u8*)(uintptr_t)task->data,
                                 task->length, (u8*)(uintptr_t)task->code,
                                 &task->code_length, task->offset);
        break;
    case ECT_CORRECT:
        r = bch_engine_correct(engine, (u8*)(uintptr_t)task->data, task->length,
                               (u8*)(uintptr_t)task->code, &task->code_length,
                               task->err_bitmap);
        break;
    default:
        r = -ENOSYS;
        break;
    }

    switch (r) {
    case 0:
        status = ETS_OK;
        break;
    case -ENOMEM:
        status = ETS_NOSPC;
        break;
    case -EBADMSG:
        status = ETS_DEC_ERROR;
        break;
    case -ENOSYS:
        status = ETS_NOTSUPPORTED;
        break;
    case -EIO:
        status = ETS_IO_ERROR;
        break;
    default:
        if (r > 0) status = ETS_OK;
        break;
    }

    task->status = status;
}

int main(void)
{
    struct bch_engine* engine;
    struct ecc_task* task = NULL;
    void* ecc_ringq_buf;
    static u8 bdring_buffer[1 << 20] __attribute__((aligned(1 << 20)));

    perfcounter_init(TRUE, TRUE);

    ipi_setup();

    ecc_ringq_buf = (void*)(uintptr_t)PS_DDR_LOW_BASE_ADDR;
    Xil_SetTlbAttributes((UINTPTR)ecc_ringq_buf,
                         NORM_SHARED_NCACHE | PRIV_RW_USER_NA);

    ringq_init(&ecc_ringq, ecc_ringq_buf, 1 << 20);

    Xil_DCacheInvalidateRange((UINTPTR)bdring_buffer, sizeof(bdring_buffer));
    Xil_SetTlbAttributes((UINTPTR)bdring_buffer,
                         NORM_SHARED_NCACHE | PRIV_RW_USER_NA);

#if USE_HARDWARE_ECC
    engine = bch_engine_init_hard(BCH_BLOCK_SIZE, BCH_CODE_SIZE, bch_dma_ids[0],
                                  bdring_buffer, bdring_buffer + BDRING_SIZE,
                                  BDRING_SIZE);
    if (!engine) {
        xil_printf("Failed to initialize BCH hardware engine\n");
        return XST_FAILURE;
    }
#else
    engine = bch_engine_init_soft(BCH_BLOCK_SIZE, BCH_CODE_SIZE);
    if (!engine) {
        xil_printf("Failed to initialize BCH software engine\n");
        return XST_FAILURE;
    }
#endif

    wfe();

    while (TRUE) {
        if (!task) {
            task = dequeue_request(engine);
            if (task) handle_ecc_task(engine, task);
        }

        if (task) {
            if (bch_engine_is_ready(engine)) {
                notify_task_complete(task);
                task = NULL;
            }
        }

        if (send_ipi) {
            ipi_trigger();
            send_ipi = FALSE;
        }

        usleep(1);
    }

    return XST_SUCCESS;
}
