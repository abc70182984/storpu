/***************************** Include Files *********************************/
#include "xparameters.h"
#include "xil_exception.h"
#include "xdebug.h"
#include "xil_io.h"
#include "xil_mmu.h"
#include "ff.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <config.h>
#include <const.h>
#include "proto.h"
#include "thread.h"
#include <libcoro.h>
#include <fil.h>
#include <ecc.h>
#include <ringq.h>
#include <utils.h>
#include <barrier.h>
#include <page.h>
#include <intr.h>
#include <cpulocals.h>
#include <timer.h>
#include <dma.h>
#include <smp.h>
#include <memalloc.h>
#include <slab.h>
#include <storpu.h>
#include <spinlock.h>

#include "hostif/nvme.h"
#include "hostif/nvme_pcie.h"

#define PS_DDR_LOW_BASE_ADDR  0x1000000UL
#define PS_DDR_LOW_LIMIT      0x80000000UL /* 2G */
#define PS_DDR_HIGH_BASE_ADDR ((unsigned long)XPAR_PSU_DDR_1_S_AXI_BASEADDR)
#define PS_DDR_HIGH_LIMIT \
    ((unsigned long)XPAR_PSU_DDR_1_S_AXI_HIGHADDR + 1) /* 2G */
#define AXI_MM_BASE_ADDR XPAR_DDR4_0_BASEADDR
#define AXI_MM_LIMIT     (AXI_MM_BASE_ADDR + (256UL << 20))
#define PL_DDR_BASE_ADDR AXI_MM_LIMIT
#define PL_DDR_LIMIT     (XPAR_DDR4_0_HIGHADDR + 1)

static FATFS fatfs;

static struct ringq fil_ringq;
static struct ringq ecc_ringq;

static DEFINE_SPINLOCK(print_lock);

static void dump_fil(void);

static void assert_callback(const char* file, s32 line)
{
    printk("FTL: Assertion failed %s:%d\n", file, line);
}

static void tick_handler(void) { worker_check_timeout(); }

void* map_scratchpad(unsigned long offset, size_t* size)
{
    unsigned long addr = (unsigned long)AXI_MM_BASE_ADDR + offset;

    if (addr >= AXI_MM_LIMIT) return NULL;
    if (size && (addr + *size > AXI_MM_LIMIT)) *size = AXI_MM_LIMIT - addr;
    return (void*)addr;
}

static inline struct fil_task* get_fil_task(unsigned int offset)
{
    void* btcm_base =
        (void*)(unsigned long)XPAR_PSU_R5_0_BTCM_GLOBAL_S_AXI_BASEADDR;
    return (struct fil_task*)(btcm_base + offset);
}

static void enqueue_fil_task(unsigned int offset)
{
    struct fil_task* task = get_fil_task(offset);

    dma_sync_single_for_device(task, sizeof(*task), DMA_BIDIRECTIONAL);

    ringq_add_avail(&fil_ringq, (uint32_t)offset);
    ringq_write_avail_tail(&fil_ringq);
}

static void handle_fil_response(struct fil_task* resp)
{
    struct worker_thread* worker = (struct worker_thread*)resp->opaque;
    worker_wake(worker, WT_BLOCKED_ON_FIL);
}

static void poll_fil_responses(void)
{
    uint32_t offset;
    struct fil_task* task = NULL;

    ringq_read_used_tail(&fil_ringq);

    while (ringq_get_used(&fil_ringq, &offset)) {
        task = get_fil_task(offset);

        /* We have flushed the cacheline when enqueueing the task and we do not
         * touch the task when FIL is processing it so there shouldn't be stale
         * cachelines and task->completed should always be TRUE. However, tasks
         * can still be accidentally loaded into cache due to (possibly)
         * speculative execution. Just play safe here... */
        if (unlikely(!task->completed))
            dma_sync_single_for_cpu(task, sizeof(*task), DMA_BIDIRECTIONAL);
        Xil_AssertVoid(task->completed);

        handle_fil_response(task);
    }
}

static void enqueue_ecc_task(struct ecc_task* task)
{
    dma_sync_single_for_device(task, sizeof(*task), DMA_BIDIRECTIONAL);

    ringq_add_avail(&ecc_ringq, (uint32_t)(uintptr_t)task);
    ringq_write_avail_tail(&ecc_ringq);
}

static void handle_ecc_response(struct ecc_task* resp)
{
    struct worker_thread* worker = (struct worker_thread*)resp->opaque;
    worker_wake(worker, WT_BLOCKED_ON_ECC);
}

static void poll_ecc_responses(void)
{
    uint32_t elem;
    struct ecc_task* task = NULL;

    ringq_read_used_tail(&ecc_ringq);

    while (ringq_get_used(&ecc_ringq, &elem)) {
        task = (struct ecc_task*)(uintptr_t)elem;

        if (unlikely(!task->completed))
            dma_sync_single_for_cpu(task, sizeof(*task), DMA_BIDIRECTIONAL);
        Xil_AssertVoid(task->completed);

        handle_ecc_response(task);
    }
}

#ifdef FORMAT_EMMC
static int format_emmc(void)
{
    static BYTE work[FF_MAX_SS];
    return f_mkfs("", FM_FAT32, 0, work, sizeof work);
}
#endif

static inline int init_emmc(void) { return f_mount(&fatfs, "", 0); }

int primary_main(void)
{
    int status;
    phys_addr_t ringq_phys;
    void *fil_ringq_buf, *ecc_ringq_buf;
    int start_cpu;

    Xil_AssertSetCallback(assert_callback);

    /* The FTL core uses cpuid == (NR_CPUS - 1). See @cpulocals.c. */
    Xil_AssertNonvoid(cpuid == FTL_CPU_ID);

    mem_init(MEMZONE_PS_DDR_LOW, PS_DDR_LOW_BASE_ADDR,
             PS_DDR_LOW_LIMIT - PS_DDR_LOW_BASE_ADDR - PROFILE_BUF_SIZE);
    mem_init(MEMZONE_PS_DDR_HIGH, PS_DDR_HIGH_BASE_ADDR,
             PS_DDR_HIGH_LIMIT - PS_DDR_HIGH_BASE_ADDR);
    mem_init(MEMZONE_PL_DDR, PL_DDR_BASE_ADDR, PL_DDR_LIMIT - PL_DDR_BASE_ADDR);

    /* We need to do some remapping for the ring queues with fixmap and we want
     * to get them up and running ASAP. */
    early_fixmap_init();

    /* Initialize the ring queues for RPUs. */
    ringq_phys =
        alloc_mem(2 << 20, 2 << 20, ZONE_PS_DDR_LOW); /* 2M with 2M alignment */
    Xil_AssertNonvoid(ringq_phys == (phys_addr_t)PS_DDR_LOW_BASE_ADDR);

    fil_ringq_buf = ioremap_nc(ringq_phys, 2 << 20);
    ecc_ringq_buf = fil_ringq_buf + (1 << 20);

    ringq_init(&fil_ringq, fil_ringq_buf, 1 << 20);
    ringq_write_avail_tail(&fil_ringq);
    ringq_write_used_tail(&fil_ringq);

    ringq_init(&ecc_ringq, ecc_ringq_buf, 1 << 20);
    ringq_write_avail_tail(&ecc_ringq);
    ringq_write_used_tail(&ecc_ringq);

    /* Allow RPUs to run. */
    /* XXX: Does this wfe() really block RPUs? */
    sev();
    wfe();

    /* Populate the main page table (ttbr0_el1). After this all RAM and device
     * memory can be accessed. */
    paging_init();

    /* Initialize CPU local variables. After this local variables like ~cpuid~
     * can be used. */
    cpulocals_init();

    /* Format EMMC if requested. */
#ifdef FORMAT_EMMC
    xil_printf("Formatting EMMC ...\n");
    format_emmc();
#endif

    /* Initialize the FS on EMMC. */
    status = init_emmc();
    if (status != 0) {
        xil_printf("Failed to initialize EMMC\n");
        return XST_FAILURE;
    }

    tls_init();
    slabs_init();

    status = intr_setup_cpu();
    if (status != XST_SUCCESS) {
        xil_printf("Failed to setup interrupt\n");
        return XST_FAILURE;
    }

    status = uart_setup();
    if (status != XST_SUCCESS) {
        xil_printf("Failed to setup UART\n");
        return XST_FAILURE;
    }

    status = ipi_setup_cpu();
    if (status != XST_SUCCESS) {
        xil_printf("Failed to setup IPI\n");
        return XST_FAILURE;
    }

    status = timer_setup();
    if (status != XST_SUCCESS) {
        xil_printf("Failed to setup timer\n");
        return XST_FAILURE;
    }

    status = zdma_setup();
    if (status != XST_SUCCESS) {
        xil_printf("Failed to setup ZDMA\n");
        return XST_FAILURE;
    }

    set_tick_handler(tick_handler);

    dbgcon_setup();

    nvme_pcie_init(AXI_MM_BASE_ADDR);

    profile_init();

    coro_init();
    worker_init(ftl_init);

    for (start_cpu = 1; start_cpu < NR_CPUS; start_cpu++) {
        Xil_AssertNonvoid(smp_start_cpu(start_cpu) == start_cpu - 1);
    }

    printk("\r\n--- Entering nvme main() plddr --- \r\n");

    for (;;) {
        u16 qid;
        struct nvme_command sqe;
        struct storpu_ftl_task* ftl_task;

        local_irq_enable();
        wfe();
        local_irq_disable();

        /* Poll NVMe FIFO */
        while (nvme_pcie_get_sqe(&qid, &sqe)) {
            if (likely(nvme_worker_dispatch(qid, &sqe)))
                nvme_pcie_pop_sqe();
            else
                break;
        }

        /* Poll StorPU tasks */
        while ((ftl_task = dequeue_storpu_ftl_task()) != NULL) {
            if (unlikely(!nvme_worker_dispatch_storpu(ftl_task))) {
                /* Not dispatched. Put it back on the queue. */
                enqueue_storpu_ftl_task(ftl_task);
                break;
            }
        }

        /* ipi_clear_status(); */

        poll_fil_responses();
        poll_ecc_responses();

        worker_yield();
    }

    pcie_stop();
    printk("--- Exiting main() --- \r\n");

    return XST_SUCCESS;
}

int printk(const char* fmt, ...)
{
    va_list arg;
    int r;

    va_start(arg, fmt);

    spin_lock(&print_lock);
    r = vprintf(fmt, arg);
    spin_unlock(&print_lock);

    va_end(arg);

    return r;
}

void panic(const char* fmt, ...)
{
    char buf[256];
    va_list arg;

    va_start(arg, fmt);
    vsprintf(buf, fmt, arg);
    va_end(arg);

    xil_printf("\nKernel panic: %s\n", buf);

    exit(1);
}

/* Dummy main function for libc */
int main(void) { return 0; }

int submit_flash_transaction(struct flash_transaction* txn)
{
    struct worker_thread* self = worker_self();
    unsigned int offset = self->tid * sizeof(struct fil_task);
    struct fil_task* task = get_fil_task(offset);
    int r;

    Xil_AssertNonvoid(txn->type == TXN_ERASE || txn->length > 0);

    memset(task, 0, sizeof(*task));
    task->addr = txn->addr;
    task->source = txn->source;
    task->type = txn->type;
    task->data = (uint64_t)txn->data;
    task->offset = txn->offset;
    task->length = txn->length;
    task->status = 0;
    task->completed = FALSE;
    task->opaque = (uint64_t)worker_self();
    task->err_bitmap = 0;

    task->code_buf = (uint64_t)txn->code_buf;
    task->code_length = txn->code_length;

    txn->stats.fil_enqueue_time = timer_get_cycles();
    enqueue_fil_task(offset);

    r = worker_wait_timeout(WT_BLOCKED_ON_FIL, 3000);

    WARN(
        r == ETIMEDOUT,
        "Flash command timeout t%d s%d ch%d w%d d%d pl%d b%d p%d data %p offset %u length %u\n",
        txn->type, txn->source, task->addr.channel, task->addr.chip,
        task->addr.die, task->addr.plane, task->addr.block, task->addr.page,
        task->data, task->offset, task->length);

    if (r) {
        dump_fil();
        return r;
    }

    txn->stats.fil_finish_time = timer_get_cycles();

    if (task->status) {
        xil_printf("!!!!! Error t%d s%d ch%d w%d d%d pl%d b%d p%d\n", txn->type,
                   txn->source, task->addr.channel, task->addr.chip,
                   task->addr.die, task->addr.plane, task->addr.block,
                   task->addr.page);
    }

    txn->total_xfer_us = task->total_xfer_us;
    txn->total_exec_us = task->total_exec_us;
    txn->err_bitmap = task->err_bitmap;
    return task->status == FTS_ERROR ? EIO : 0;
}

static void dump_fil(void)
{
    unsigned int offset = NR_FTL_THREADS * sizeof(struct fil_task);
    struct fil_task* task = get_fil_task(offset);

    memset(task, 0, sizeof(*task));
    task->type = TXN_DUMP;
    task->opaque = (uint64_t)worker_self();

    enqueue_fil_task(offset);

    worker_wait_timeout(WT_BLOCKED_ON_FIL, 3000);
}

static int submit_ecc_task(int type, u8* data, size_t data_length, u8* code,
                           size_t code_length, size_t offset,
                           uint64_t err_bitmap)
{
    u8 task_buf[64 + sizeof(struct ecc_task)];
    struct ecc_task* task =
        (struct ecc_task*)((uintptr_t)&task_buf[63] & ~0x3f);
    int r;

    memset(task, 0, sizeof(*task));
    task->type = type;
    task->data = (uint64_t)data;
    task->offset = (uint32_t)offset;
    task->length = (uint32_t)data_length;
    task->code = (uint64_t)code;
    task->code_length = (uint32_t)code_length;
    task->err_bitmap = err_bitmap;
    task->opaque = (uint64_t)worker_self();

    enqueue_ecc_task(task);

    r = worker_wait_timeout(WT_BLOCKED_ON_ECC, 3000);
    if (r) return r;

    switch (task->status) {
    case ETS_OK:
        r = task->code_length;
        break;
    case ETS_NOSPC:
        r = -ENOMEM;
        break;
    case ETS_DEC_ERROR:
        r = -EBADMSG;
        break;
    case ETS_NOTSUPPORTED:
        r = -ENOSYS;
        break;
    case ETS_IO_ERROR:
        r = -EIO;
        break;
    }

    return r;
}

int ecc_calculate(const u8* data, size_t data_length, u8* code,
                  size_t code_length, size_t offset)
{
    return submit_ecc_task(ECT_CALC, (u8*)data, data_length, code, code_length,
                           offset, 0);
}

int ecc_correct(u8* data, size_t data_length, const u8* code,
                size_t code_length, uint64_t err_bitmap)
{
    return submit_ecc_task(ECT_CORRECT, data, data_length, (u8*)code,
                           code_length, 0, err_bitmap);
}

int submit_storpu_task(struct storpu_task* task, u32 timeout_ms)
{
    struct worker_thread* self = worker_self();
    unsigned long flags;
    int r;

    local_irq_save(&flags);

    task->opaque = self;

    enqueue_storpu_request(task);

    r = worker_wait_timeout(WT_BLOCKED_ON_STORPU, timeout_ms);

    local_irq_restore(flags);

    return r ?: task->retval;
}

void handle_storpu_completion(void)
{
    struct llist_node* entry;
    struct storpu_task* task;

    entry = dequeue_storpu_completions();

    llist_for_each_entry(task, entry, llist)
    {
        struct worker_thread* worker = (struct worker_thread*)task->opaque;

        if (worker) {
            worker_wake(worker, WT_BLOCKED_ON_STORPU);
        }
    }
}
