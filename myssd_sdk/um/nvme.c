#include <xil_assert.h>
#include "thread.h"
#include "proto.h"
#include <utils.h>
#include <types.h>
#include <tls.h>
#include <iov_iter.h>

#include <ublksrv.h>
#include <ublksrv_aio.h>

/* Designated thread to perform shutdown operations. */
#define SHUTDOWN_THREAD 0

struct nvme_command_slot {
    struct ublksrv_aio* req;
    int valid;
};

static bitchunk_t cmd_slot_empty[BITCHUNKS(NR_WORKER_THREADS)];
static bitchunk_t worker_idle[BITCHUNKS(NR_WORKER_THREADS)];

static DEFINE_TLS(struct nvme_command_slot, nvme_command_slot);

static int nvme_shutdown_req = FALSE;

struct ublksrv_aio* nvme_pcie_get_sqe(void);
void nvme_pcie_pop_sqe(void);
void nvme_pcie_post_completion(struct ublksrv_aio* aio, int result);

int nvme_dma_read(struct user_request* req, struct iov_iter* iter, size_t count)
{
    iov_iter_copy_to(iter, (void*)req->prps[0], count);
    return 0;
}

int nvme_dma_write(struct user_request* req, struct iov_iter* iter,
                   size_t count)
{
    iov_iter_copy_from(iter, (void*)req->prps[0], count);
    return 0;
}

static int process_io_command(struct ublksrv_aio* aio)
{
    const struct ublksrv_io_desc* iod = &aio->io;
    struct user_request req;
    int r;

    memset(&req, 0, sizeof(req));

    switch (ublksrv_get_op(iod)) {
    case UBLK_IO_OP_READ:
        req.req_type = IOREQ_READ;
        break;
    case UBLK_IO_OP_WRITE:
        req.req_type = IOREQ_WRITE;
        break;
    case UBLK_IO_OP_FLUSH:
        req.req_type = IOREQ_FLUSH;
        break;
    default:
        return -EINVAL;
    }

    if (req.req_type != IOREQ_FLUSH) {
        assert(((iod->start_sector << 9) & ((1UL << SECTOR_SHIFT) - 1)) == 0);
        assert(((iod->nr_sectors << 9) & ((1UL << SECTOR_SHIFT) - 1)) == 0);
    }

    req.nsid = 1;
    req.start_lba = (iod->start_sector << 9) >> SECTOR_SHIFT;
    req.sector_count = (iod->nr_sectors << 9) >> SECTOR_SHIFT;
    req.prps[0] = (uint64_t)iod->addr;
    req.prps[1] = 0;

    xil_printf("%d type %d ns %d slba %lx sects %d prp0 %lx\n",
               worker_self()->tid, req.req_type, req.nsid, req.start_lba,
               req.sector_count, req.prps[0]);

    INIT_LIST_HEAD(&req.txn_list);
    r = ftl_process_request(&req);
    if (r) xil_printf("Command error %d\n", r);

    return (r > 0) ? -r : (iod->nr_sectors << 9);
}

int nvme_worker_dispatch_aio(struct ublksrv_aio* aio)
{
    unsigned int worker;
    struct nvme_command_slot* cmd_slot;

    worker =
        find_next_and_bit(worker_idle, cmd_slot_empty, NR_WORKER_THREADS, 0);
    if (worker >= NR_WORKER_THREADS) {
        worker = find_first_bit(cmd_slot_empty, NR_WORKER_THREADS);
        if (worker >= NR_WORKER_THREADS) return FALSE;
    }

    cmd_slot = get_tls_var_ptr(worker, nvme_command_slot);
    Xil_AssertNonvoid(!cmd_slot->valid);
    cmd_slot->valid = TRUE;
    cmd_slot->req = aio;
    UNSET_BIT(cmd_slot_empty, worker);
    worker_wake(worker_get(worker), WT_BLOCKED_ON_NVME_SQ);

    return TRUE;
}

static int nvme_worker_get_work(struct ublksrv_aio** req)
{
    struct worker_thread* self = worker_self();
    struct nvme_command_slot* cmd_slot = get_local_var_ptr(nvme_command_slot);
    struct ublksrv_aio* aio;
    int found;

    if (nvme_shutdown_req && self->tid == SHUTDOWN_THREAD) return FALSE;

    /* First see if there is pending work ... */
    if (cmd_slot->valid) {
        Xil_AssertNonvoid(!GET_BIT(cmd_slot_empty, self->tid));

        *req = cmd_slot->req;

        SET_BIT(cmd_slot_empty, self->tid);
        cmd_slot->valid = FALSE;
        return TRUE;
    }

    Xil_AssertNonvoid(GET_BIT(cmd_slot_empty, self->tid));

    /* No luck. Try to get some from FIFO. */
    if ((aio = nvme_pcie_get_sqe()) != NULL) {
        *req = aio;
        nvme_pcie_pop_sqe();
        return TRUE;
    }

    /* Nope. Need to wait now. */
    SET_BIT(worker_idle, self->tid);
    worker_wait(WT_BLOCKED_ON_NVME_SQ);
    UNSET_BIT(worker_idle, self->tid);

    found = cmd_slot->valid;

    if (found) {
        *req = cmd_slot->req;

        SET_BIT(cmd_slot_empty, self->tid);
        cmd_slot->valid = FALSE;
    }

    return found;
}

void nvme_request_shutdown(void)
{
    nvme_shutdown_req = TRUE;

    worker_wake(worker_get(SHUTDOWN_THREAD), WT_BLOCKED_ON_NVME_SQ);
}

int nvme_shutdown_busy(void) { return nvme_shutdown_req; }

void nvme_worker_main(void)
{
    struct worker_thread* self = worker_self();
    struct ublksrv_aio* aio;

    printk("Started thread %d %p\n", self->tid, self);

    SET_BIT(cmd_slot_empty, self->tid);

    local_irq_enable();

    while (nvme_worker_get_work(&aio)) {
        int status;

        status = process_io_command(aio);

        nvme_pcie_post_completion(aio, status);
    }

    if (nvme_shutdown_req && self->tid == SHUTDOWN_THREAD) {
        ftl_shutdown(FALSE);
        nvme_shutdown_req = FALSE;
    }
}
