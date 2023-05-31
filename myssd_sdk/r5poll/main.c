#include "xparameters.h"
#include <stdio.h>
#include <stdlib.h>
#include "xil_printf.h"
#include "xil_mmu.h"
#include "sleep.h"
#include "xil_cache.h"

#include <proto.h>
#include <barrier.h>
#include <flash.h>
#include <fil.h>
#include "fil/fil.h"
#include <ringq.h>
#include <intr.h>

#define PS_DDR_LOW_BASE_ADDR 0x1000000UL
#define PS_DDR_LOW_LIMIT     0x80000000UL /* 2G */

#define IPI_COALESCE 1
#define IPI_DELAY    10

static struct ringq fil_ringq;
int ipi_coalesce_counter = 0;

static void report_stats(void);

static inline struct fil_task* get_fil_task(unsigned int offset)
{
    return (struct fil_task*)(XPAR_PSU_R5_0_BTCM_S_AXI_BASEADDR + offset);
}

static inline unsigned int get_fil_task_offset(struct fil_task* task)
{
    return (unsigned int)task - XPAR_PSU_R5_0_BTCM_S_AXI_BASEADDR;
}

static int dequeue_requests(void)
{
    uint32_t offset;
    int found = FALSE;
    struct fil_task* task = NULL;

    ringq_read_avail_tail(&fil_ringq);

    while (ringq_get_avail(&fil_ringq, &offset)) {
        found = TRUE;
        task = get_fil_task(offset);

        if (task->type == TXN_DUMP) {
            report_stats();
            notify_task_complete(task, FALSE);
        } else {
            tsu_process_task(task);
        }
    }

    return found;
}

static void enqueue_response(struct fil_task* task)
{
    unsigned int offset = get_fil_task_offset(task);

    Xil_AssertVoid(task->completed);

    ringq_add_used(&fil_ringq, offset);
    ringq_write_used_tail(&fil_ringq);
    ipi_coalesce_counter++;
}

static void assert_callback(const char* file, s32 line)
{
    xil_printf("FIL: Assertion failed %s:%d\n", file, line);
}

int main()
{
    void* fil_ringq_buf;
    int ipi_delay = IPI_DELAY;

    Xil_AssertSetCallback(assert_callback);

    intr_setup_cpu();

    timer_setup();
    ipi_setup();

    fil_ringq_buf = (void*)(uintptr_t)PS_DDR_LOW_BASE_ADDR;
    Xil_SetTlbAttributes((UINTPTR)fil_ringq_buf,
                         NORM_SHARED_NCACHE | PRIV_RW_USER_NA);

    ringq_init(&fil_ringq, fil_ringq_buf, 1 << 20);

    tsu_init();
    fil_init();

    watchdog_init();

    local_irq_enable();

    wfe();

    int profile_started = FALSE;
    int timeout = 0;

    while (TRUE) {
        int found;

        timeout++;
        if (timeout > 1000 && profile_started) {
            profile_started = FALSE;
            profile_stop();
        }

        found = dequeue_requests();

        if (found && !profile_started) {
            profile_start(100);
            profile_started = TRUE;
        }
        if (found) timeout = 0;

        if (found) tsu_flush_queues();

        fil_tick();

        ipi_delay--;

        if (ipi_coalesce_counter >= IPI_COALESCE || ipi_delay == 0) {
            if (ipi_coalesce_counter) ipi_trigger();

            ipi_delay = IPI_DELAY;
            ipi_coalesce_counter = 0;
        }

        /* usleep(1); */
    }

    return 0;
}

void notify_task_complete(struct fil_task* task, int error)
{
    task->status = error ? FTS_ERROR : FTS_OK;
    task->completed = TRUE;

    task->total_xfer_us = timer_cycles_to_us(task->total_xfer_us);
    task->total_exec_us = timer_cycles_to_us(task->total_exec_us);

    enqueue_response(task);
}

void panic(const char* fmt, ...)
{
    char buf[256];
    va_list arg;

    va_start(arg, fmt);
    vsprintf(buf, fmt, arg);
    va_end(arg);

    xil_printf("\nR5 panic: %s\n", buf);

    exit(1);
}

static void report_stats(void)
{
    xil_printf("TSU\n");
    xil_printf("===========================================\n");
    tsu_report_stats();
    xil_printf("===========================================\n");

    xil_printf("FIL\n");
    xil_printf("===========================================\n");
    fil_report_stats();
    xil_printf("===========================================\n");
}
