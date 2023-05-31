#include <xil_printf.h>
#include <xil_assert.h>
#include <errno.h>

#include <config.h>
#include <const.h>
#include "../proto.h"
#include "../thread.h"
#include <iov_iter.h>
#include <bitmap.h>
#include "../tls.h"
#include <utils.h>
#include <page.h>
#include <intr.h>
#include <dma.h>
#include <memalloc.h>

#include <slab.h>
#include <storpu.h>

#include "nvme.h"
#include "nvme_pcie.h"

#define NAME "[NVMe]"

/* Designated thread to perform shutdown operations. */
#define SHUTDOWN_THREAD 0

#define NVME_PAGE_SIZE (0x1000)

struct nvme_command_slot {
    u16 qid;
    struct nvme_command sqe;
    struct storpu_ftl_task* storpu_task;
    int valid;
};

static bitchunk_t cmd_slot_empty[BITCHUNKS(NR_WORKER_THREADS)];
static bitchunk_t worker_idle[BITCHUNKS(NR_WORKER_THREADS)];

static DEFINE_TLS(struct nvme_command_slot, nvme_command_slot);

static DEFINE_TLS(u8*, prp_list_buf);

static int nvme_shutdown_req = FALSE;

/* Identify commands. */
extern int nvme_identify_namespace(u32 nsid, u8* data);
extern void nvme_identify_controller(u8* data);
extern void nvme_identify_ns_active_list(u8* data);
extern void nvme_identify_cs_controller(u8 csi, u8* data);

static inline int err2statuscode(int err)
{
    switch (err) {
    case 0:
        return NVME_SC_SUCCESS;
    case EPERM:
        return NVME_SC_ACCESS_DENIED;
    case EINVAL:
        return NVME_SC_INVALID_FIELD;
    case EEXIST:
        return NVME_SC_CMDID_CONFLICT;
    case EIO:
        return NVME_SC_DATA_XFER_ERROR;
    case ENOSYS:
        return NVME_SC_INVALID_OPCODE;
    case ESRCH:
        return NVME_SC_INVALID_NS;
    case EBADMSG:
        return NVME_SC_READ_ERROR;
    default:
        return NVME_SC_INTERNAL;
    }
}

static inline int flush_dma(int do_write, u64 addr, struct iov_iter* iter,
                            size_t count)
{
    int r;

    if (do_write)
        r = pcie_dma_write_iter(addr, iter, count);
    else
        r = pcie_dma_read_iter(addr, iter, count);

    return r;
}

static int read_prp_list(u64 addr, u8* prp_list, size_t len)
{
    int r;

    dma_sync_single_for_device(prp_list, len, DMA_FROM_DEVICE);
    r = pcie_dma_read(addr, prp_list, len);
    if (r == 0) dma_sync_single_for_cpu(prp_list, len, DMA_FROM_DEVICE);

    return r;
}

static int transfer_prp_data(struct iov_iter* iter, int do_write, u64 prp1,
                             u64 prp2, size_t count, size_t max_size)
{
    size_t chunk;
    unsigned int offset;
    u64* prp_list = (u64*)get_local_var(prp_list_buf);
    unsigned int nprps;
    u64 dma_addr;
    size_t dma_size;
    int i, r;

    Xil_AssertNonvoid(max_size >= count);

    if (count == 0) return 0;

    /* PRP 1 */
    offset = prp1 % NVME_PAGE_SIZE;
    chunk = min(NVME_PAGE_SIZE - offset, count);

    if (chunk > 0) {
        dma_addr = prp1;
        dma_size = chunk;

        count -= chunk;
        max_size -= chunk;
    }

    if (count == 0) return flush_dma(do_write, dma_addr, iter, dma_size);

    /* PRP 2 */
    if (max_size <= NVME_PAGE_SIZE) {
        chunk = count;

        if (dma_addr + dma_size == prp2) {
            dma_size += chunk;
        } else {
            r = flush_dma(do_write, dma_addr, iter, dma_size);
            if (r != 0) return r;

            dma_addr = prp2;
            dma_size = chunk;
        }

        return flush_dma(do_write, dma_addr, iter, dma_size);
    }

    /* PRP list */
    offset = prp2 % NVME_PAGE_SIZE;
    nprps = roundup(count, NVME_PAGE_SIZE) / NVME_PAGE_SIZE + 1;
    nprps = (nprps + 3) & ~0x3; /* Align to 32 bytes */
    nprps = min(nprps, (NVME_PAGE_SIZE - offset) >> 3);
    r = read_prp_list(prp2, (u8*)prp_list, nprps << 3);
    if (r != 0) return r;

    i = 0;
    for (;;) {
        if (i == nprps - 1) {
            if (count > NVME_PAGE_SIZE) {
                /* More PRP lists. */
                Xil_AssertNonvoid(!(prp_list[i] & (NVME_PAGE_SIZE - 1)));
                nprps = roundup(count, NVME_PAGE_SIZE) / NVME_PAGE_SIZE + 1;
                nprps = (nprps + 3) & ~0x3; /* Align to 32 bytes */
                nprps = min(nprps, NVME_PAGE_SIZE >> 3);
                r = read_prp_list(prp_list[i], (u8*)prp_list, nprps << 3);
                if (r != 0) return r;
                i = 0;
            }
        }

        offset = prp_list[i] % NVME_PAGE_SIZE;
        chunk = min(NVME_PAGE_SIZE - offset, count);

        if (dma_addr + dma_size == prp_list[i]) {
            dma_size += chunk;
        } else {
            r = flush_dma(do_write, dma_addr, iter, dma_size);
            if (r != 0) return r;

            dma_addr = prp_list[i];
            dma_size = chunk;
        }

        i++;
        count -= chunk;

        if (count == 0) break;
    }

    if (dma_size > 0) {
        r = flush_dma(do_write, dma_addr, iter, dma_size);
        if (r != 0) return r;
    }

    return 0;
}

static inline int read_prp_data(struct iov_iter* iter, u64 prp1, u64 prp2,
                                size_t count, size_t max_size)
{
    return transfer_prp_data(iter, 0, prp1, prp2, count, max_size);
}

static inline int write_prp_data(struct iov_iter* iter, u64 prp1, u64 prp2,
                                 size_t count, size_t max_size)
{
    return transfer_prp_data(iter, 1, prp1, prp2, count, max_size);
}

int nvme_dma_read(struct user_request* req, struct iov_iter* iter, size_t count,
                  size_t max_size)
{
    if (req->buf_phys) {
        ssize_t r;

        Xil_AssertNonvoid(req->prps[0] == 0);
        Xil_AssertNonvoid(req->prps[1] == 0);

        r = zdma_iter_copy_to(iter, __va(req->buf_phys), count, TRUE);
        if (r != count) return EFAULT;

        return 0;
    }

    return read_prp_data(iter, req->prps[0], req->prps[1], count, max_size);
}

int nvme_dma_write(struct user_request* req, struct iov_iter* iter,
                   size_t count, size_t max_size)
{
    if (req->buf_phys) {
        ssize_t r;

        Xil_AssertNonvoid(req->prps[0] == 0);
        Xil_AssertNonvoid(req->prps[1] == 0);

        r = zdma_iter_copy_from(iter, __va(req->buf_phys), count, TRUE);
        if (r != count) return EFAULT;

        return 0;
    }

    return write_prp_data(iter, req->prps[0], req->prps[1], count, max_size);
}

static int process_set_features_command(struct nvme_features* cmd,
                                        union nvme_result* result)
{
    int status = NVME_SC_SUCCESS;

    switch (cmd->fid) {
    case NVME_FEAT_NUM_QUEUES:
        result->u32 = CONFIG_NVME_IO_QUEUE_MAX - 1;
        result->u32 |= result->u32 << 16;
        break;
    default:
        status = NVME_SC_FEATURE_NOT_SAVEABLE;
        break;
    }

    return status;
}

static int process_create_cq_command(struct nvme_create_cq* cmd,
                                     union nvme_result* result)
{
    u16 qid = cmd->cqid;

    /* Invalid queue identifier. */
    if (qid == 0 || qid > CONFIG_NVME_IO_QUEUE_MAX) return NVME_SC_QID_INVALID;

    xil_printf("Create CQ %d %lx %d %d\n", qid, cmd->prp1, cmd->qsize,
               cmd->irq_vector);
    nvme_pcie_config_cq(qid, cmd->prp1, cmd->qsize, cmd->irq_vector, 1, 1);

    return 0;
}

static int process_create_sq_command(struct nvme_create_sq* cmd,
                                     union nvme_result* result)
{
    u16 qid = cmd->sqid;
    /* Invalid queue identifier. */
    if (qid == 0 || qid > CONFIG_NVME_IO_QUEUE_MAX) return NVME_SC_QID_INVALID;
    /* Invalid CQ identifier. */
    if (qid != cmd->cqid) return NVME_SC_CQ_INVALID;

    xil_printf("Create SQ %d %lx %d %d\n", qid, cmd->prp1, cmd->qsize,
               cmd->cqid);
    nvme_pcie_config_sq(qid, cmd->prp1, cmd->qsize, cmd->cqid, 1);

    return 0;
}

static int process_identify_command(struct nvme_identify* cmd,
                                    union nvme_result* result)
{
    u8* identify_data;
    struct iovec iov;
    struct iov_iter iter;
    int status = NVME_SC_SUCCESS;

    identify_data = alloc_vmpages(1, ZONE_ALL);

    xil_printf("Identify %lx\n", cmd->dptr.prp1);

    switch (cmd->cns) {
    case NVME_ID_CNS_NS:
        status = nvme_identify_namespace(cmd->nsid, identify_data);
        break;
    case NVME_ID_CNS_CTRL:
        nvme_identify_controller(identify_data);
        break;
    case NVME_ID_CNS_NS_ACTIVE_LIST:
        nvme_identify_ns_active_list(identify_data);
        break;
    case NVME_ID_CNS_CS_CTRL:
        nvme_identify_cs_controller(cmd->csi, identify_data);
        break;
    }

    iov.iov_base = identify_data;
    iov.iov_len = 0x1000;
    iov_iter_init(&iter, &iov, 1, 0x1000);

    dma_sync_single_for_device(identify_data, 0x1000, DMA_TO_DEVICE);
    write_prp_data(&iter, cmd->dptr.prp1, cmd->dptr.prp2, 0x1000, 0x1000);

    free_mem(__pa(identify_data), ARCH_PG_SIZE);

    return status;
}

static int process_ns_mgmt_command(struct nvme_common_command* cmd,
                                   union nvme_result* result)
{
    struct nvme_id_ns* identify_data;
    struct iovec iov;
    struct iov_iter iter;
    struct namespace_info ns_info;
    int r, status = NVME_SC_SUCCESS;

    identify_data = (struct nvme_id_ns*)alloc_vmpages(1, ZONE_ALL);

    switch (cmd->cdw10) {
    case NVME_NS_MGMT_SEL_CREATE:
        memset(&ns_info, 0, sizeof(ns_info));

        iov.iov_base = identify_data;
        iov.iov_len = 0x1000;
        iov_iter_init(&iter, &iov, 1, 0x1000);

        dma_sync_single_for_device(identify_data, 0x1000, DMA_FROM_DEVICE);
        r = read_prp_data(&iter, cmd->dptr.prp1, cmd->dptr.prp2, 0x1000,
                          0x1000);
        dma_sync_single_for_cpu(identify_data, 0x1000, DMA_FROM_DEVICE);

        if (r != 0) {
            status = err2statuscode(r);
            break;
        }

        ns_info.size_blocks = identify_data->nsze;
        ns_info.capacity_blocks = identify_data->ncap;

        r = ftl_create_namespace(&ns_info);
        if (r <= 0) {
            if (r == -ENFILE)
                status = NVME_SC_NS_ID_UNAVAILABLE;
            else
                status = err2statuscode(-r);
        } else {
            result->u32 = r;
        }

        break;
    case NVME_NS_MGMT_SEL_DELETE:
        r = ftl_delete_namespace(cmd->nsid);

        status = err2statuscode(r);
        break;
    default:
        status = NVME_SC_INVALID_FIELD;
        break;
    }

    free_mem(__pa(identify_data), ARCH_PG_SIZE);

    return status;
}

static int process_ns_attach_command(struct nvme_common_command* cmd,
                                     union nvme_result* result)
{
    u16* ctrl_list;
    struct iovec iov;
    struct iov_iter iter;
    int r, status = NVME_SC_SUCCESS;

    ctrl_list = (u16*)alloc_vmpages(1, ZONE_ALL);

    iov.iov_base = ctrl_list;
    iov.iov_len = 0x1000;
    iov_iter_init(&iter, &iov, 1, 0x1000);

    dma_sync_single_for_device(ctrl_list, 0x1000, DMA_FROM_DEVICE);
    r = read_prp_data(&iter, cmd->dptr.prp1, cmd->dptr.prp2, 0x1000, 0x1000);
    dma_sync_single_for_cpu(ctrl_list, 0x1000, DMA_FROM_DEVICE);

    if (r != 0) {
        status = err2statuscode(r);
        goto out;
    }

    switch (cmd->cdw10) {
    case NVME_NS_ATTACH_SEL_CTRL_ATTACH:
        if (ctrl_list[0] != 0) {
            r = ftl_attach_namespace(cmd->nsid);

            if (r == EBUSY)
                status = NVME_SC_NS_ALREADY_ATTACHED;
            else
                r = err2statuscode(r);
        }

        break;
    case NVME_NS_ATTACH_SEL_CTRL_DETACH:
        if (ctrl_list[0] != 0) {
            r = ftl_detach_namespace(cmd->nsid);

            if (r == ENOENT)
                status = NVME_SC_NS_NOT_ATTACHED;
            else
                r = err2statuscode(r);
        }

        break;
    default:
        status = NVME_SC_INVALID_FIELD;
        break;
    }

out:
    free_mem(__pa(ctrl_list), ARCH_PG_SIZE);

    return status;
}

static int
process_storpu_create_context_command(struct nvme_common_command* cmd,
                                      union nvme_result* result)
{
    struct storpu_task task;
    unsigned long addr = cmd->dptr.prp1;
    int r;

    memset(&task, 0, sizeof(task));
    task.type = SPU_TYPE_CREATE_CONTEXT;
    task.create_context.so_addr = map_scratchpad(addr, NULL);

    r = submit_storpu_task(&task, 0);
    if (r == 0) result->u32 = task.create_context.cid;

    return err2statuscode(r);
}

static int
process_storpu_delete_context_command(struct nvme_common_command* cmd,
                                      union nvme_result* result)
{
    struct storpu_task task;
    int r;

    memset(&task, 0, sizeof(task));
    task.type = SPU_TYPE_DELETE_CONTEXT;
    task.delete_context.cid = cmd->cdw10;

    r = submit_storpu_task(&task, 0);

    return err2statuscode(r);
}

static int process_admin_command(struct nvme_command* cmd,
                                 union nvme_result* result)
{
    int status;

    switch (cmd->common.opcode) {
    case nvme_admin_identify:
        status = process_identify_command(&cmd->identify, result);
        break;
    case nvme_admin_set_features:
        status = process_set_features_command(&cmd->features, result);
        break;
    case nvme_admin_create_cq:
        status = process_create_cq_command(&cmd->create_cq, result);
        break;
    case nvme_admin_create_sq:
        status = process_create_sq_command(&cmd->create_sq, result);
        break;
    case nvme_admin_ns_mgmt:
        status = process_ns_mgmt_command(&cmd->common, result);
        break;
    case nvme_admin_ns_attach:
        status = process_ns_attach_command(&cmd->common, result);
        break;
    case nvme_admin_storpu_create_context:
        status = process_storpu_create_context_command(&cmd->common, result);
        break;
    case nvme_admin_storpu_delete_context:
        status = process_storpu_delete_context_command(&cmd->common, result);
        break;
    default:
        status = NVME_SC_INVALID_OPCODE;
        break;
    }

    return status;
}

static int process_ftl_command(struct nvme_command* cmd,
                               union nvme_result* result)
{
    struct worker_thread* self = worker_self();
    struct user_request req;
    int r, status;

    memset(&req, 0, sizeof(req));

    switch (cmd->rw.opcode) {
    case nvme_cmd_read:
        req.req_type = IOREQ_READ;
        break;
    case nvme_cmd_write:
        req.req_type = IOREQ_WRITE;
        break;
    case nvme_cmd_flush:
        req.req_type = IOREQ_FLUSH;
        break;
    case nvme_cmd_write_zeroes:
        req.req_type = IOREQ_WRITE_ZEROES;
        break;
    }

    req.nsid = cmd->rw.nsid;
    req.start_lba = cmd->rw.slba;
    req.sector_count = cmd->rw.length + 1;
    req.prps[0] = cmd->rw.dptr.prp1;
    req.prps[1] = cmd->rw.dptr.prp2;

    /* xil_printf( */
    /*     "%d cmd %d cid %d ns %d flags %x slba %p sects %d prp0 %p prp1 %p\n",
     */
    /*     worker_self()->tid, cmd->common.opcode, cmd->common.command_id, */
    /*     req.nsid, cmd->rw.flags, req.start_lba, req.sector_count,
     * req.prps[0], */
    /*     req.prps[1]); */

    INIT_LIST_HEAD(&req.txn_list);

    self->cur_request = &req;

    r = ftl_process_request(&req);
    if (r) xil_printf("Command error %d\n", r);

    self->cur_request = NULL;

    status = err2statuscode(r);

    return status;
}

static int process_storpu_invoke_command(struct nvme_storpu_invoke_command* cmd,
                                         union nvme_result* result)
{
    struct storpu_task task;
    int r;

    memset(&task, 0, sizeof(task));
    task.type = SPU_TYPE_INVOKE;
    task.invoke.cid = cmd->cid;
    task.invoke.entry = (unsigned long)cmd->entry;
    task.invoke.arg = (unsigned long)cmd->arg;

    r = submit_storpu_task(&task, 0);
    if (r == 0) result->u64 = (u64)task.invoke.result;

    return err2statuscode(r);
}

static int process_io_command(struct nvme_command* cmd,
                              union nvme_result* result)
{
    int status;

    switch (cmd->common.opcode) {
    case nvme_cmd_read:
    case nvme_cmd_write:
    case nvme_cmd_flush:
        status = process_ftl_command(cmd, result);
        break;
    case nvme_cmd_storpu_invoke:
        status = process_storpu_invoke_command(&cmd->storpu_invoke, result);
        break;
    default:
        status = NVME_SC_INVALID_OPCODE;
        break;
    }

    return status;
}

int nvme_worker_dispatch(u16 qid, struct nvme_command* sqe)
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
    cmd_slot->qid = qid;
    cmd_slot->sqe = *sqe;
    cmd_slot->storpu_task = NULL;
    UNSET_BIT(cmd_slot_empty, worker);
    worker_wake(worker_get(worker), WT_BLOCKED_ON_NVME_SQ);

    return TRUE;
}

int nvme_worker_dispatch_storpu(struct storpu_ftl_task* task)
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
    cmd_slot->storpu_task = task;
    UNSET_BIT(cmd_slot_empty, worker);
    worker_wake(worker_get(worker), WT_BLOCKED_ON_NVME_SQ);

    return TRUE;
}

static int nvme_worker_get_work(u16* qidp, struct nvme_command* sqe,
                                struct storpu_ftl_task** storpu_task)
{
    struct worker_thread* self = worker_self();
    struct nvme_command_slot* cmd_slot = get_local_var_ptr(nvme_command_slot);
    struct storpu_ftl_task* task;
    int found;

    *storpu_task = NULL;

    /* Check shutdown request. This is the MOST urgent. */
    if (unlikely(nvme_shutdown_req && self->tid == SHUTDOWN_THREAD))
        return FALSE;

    /* First see if there is pending work ... */
    if (cmd_slot->valid) {
        Xil_AssertNonvoid(!GET_BIT(cmd_slot_empty, self->tid));

        if (cmd_slot->storpu_task) {
            *storpu_task = cmd_slot->storpu_task;
        } else {
            *qidp = cmd_slot->qid;
            *sqe = cmd_slot->sqe;
        }

        SET_BIT(cmd_slot_empty, self->tid);
        cmd_slot->valid = FALSE;
        return TRUE;
    }

    Xil_AssertNonvoid(GET_BIT(cmd_slot_empty, self->tid));

    /* No luck. Try to get some from FIFO. */
    if (nvme_pcie_get_sqe(qidp, sqe)) {
        nvme_pcie_pop_sqe();
        return TRUE;
    }

    if ((task = dequeue_storpu_ftl_task()) != NULL) {
        *storpu_task = task;
        return TRUE;
    }

    /* Nope. Need to wait now. */
    SET_BIT(worker_idle, self->tid);
    worker_wait(WT_BLOCKED_ON_NVME_SQ);
    UNSET_BIT(worker_idle, self->tid);

    if (unlikely(nvme_shutdown_req && self->tid == SHUTDOWN_THREAD))
        return FALSE;

    found = cmd_slot->valid;

    if (found) {
        if (cmd_slot->storpu_task) {
            *storpu_task = cmd_slot->storpu_task;
        } else {
            *qidp = cmd_slot->qid;
            *sqe = cmd_slot->sqe;
        }

        SET_BIT(cmd_slot_empty, self->tid);
        cmd_slot->valid = FALSE;
    }

    return found;
}

static int process_storpu_flash_task(struct storpu_ftl_task* task)
{
    struct user_request req;
    int r;

    memset(&req, 0, sizeof(req));

    switch (task->type) {
    case FTL_TYPE_FLASH_READ:
        req.req_type = IOREQ_READ;
        break;
    case FTL_TYPE_FLASH_WRITE:
        req.req_type = IOREQ_WRITE;
        break;
    case FTL_TYPE_FLUSH:
        req.req_type = IOREQ_FLUSH;
        break;
    case FTL_TYPE_FLUSH_DATA:
        req.req_type = IOREQ_FLUSH_DATA;
        break;
    case FTL_TYPE_SYNC:
        req.req_type = IOREQ_SYNC;
        break;
    }

    req.nsid = task->nsid;
    req.start_lba = task->addr >> SECTOR_SHIFT;
    req.sector_count = task->count >> SECTOR_SHIFT;
    req.buf_phys = task->buf_phys;

    INIT_LIST_HEAD(&req.txn_list);

    r = ftl_process_request(&req);
    if (r) xil_printf("Command error %d\n", r);

    return r;
}

int process_storpu_host_xfer(struct storpu_ftl_task* task)
{
    enum dma_data_direction dir =
        (task->type == FTL_TYPE_HOST_READ) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
    void* buf = __va(task->buf_phys);
    int r;

    dma_sync_single_for_device(buf, task->count, dir);

    switch (task->type) {
    case FTL_TYPE_HOST_READ:
        r = pcie_dma_read(task->addr, buf, task->count);
        break;
    case FTL_TYPE_HOST_WRITE:
        r = pcie_dma_write(task->addr, buf, task->count);
        break;
    default:
        r = EINVAL;
        break;
    }

    dma_sync_single_for_cpu(buf, task->count, dir);

    return r;
}

int process_storpu_task(struct storpu_ftl_task* task)
{
    int r;

    switch (task->type) {
    case FTL_TYPE_FLASH_READ:
    case FTL_TYPE_FLASH_WRITE:
    case FTL_TYPE_FLUSH:
    case FTL_TYPE_FLUSH_DATA:
    case FTL_TYPE_SYNC:
        r = process_storpu_flash_task(task);
        break;
    case FTL_TYPE_HOST_READ:
    case FTL_TYPE_HOST_WRITE:
        r = process_storpu_host_xfer(task);
        break;
    default:
        r = EINVAL;
        break;
    }

    return r;
}

void nvme_request_shutdown(void)
{
    nvme_shutdown_req = TRUE;
    worker_wake(worker_get(SHUTDOWN_THREAD), WT_BLOCKED_ON_NVME_SQ);
}

static void nvme_do_shutdown(int abrupt)
{
    xil_printf(NAME " Initiated %s shutdown\n", abrupt ? "abrupt" : "normal");
    ftl_shutdown(abrupt);
}

void nvme_worker_main(void)
{
    struct worker_thread* self = worker_self();
    u16 qid;
    struct nvme_command sqe;
    struct storpu_ftl_task* storpu_task;
    int found;

    printk("Started thread %d %p\n", self->tid, self);

    get_local_var(prp_list_buf) = alloc_vmpages(1, ZONE_PS_DDR);
    SET_BIT(cmd_slot_empty, self->tid);

    local_irq_enable();

    while ((found = nvme_worker_get_work(&qid, &sqe, &storpu_task)) ||
           unlikely(nvme_shutdown_req && self->tid == SHUTDOWN_THREAD)) {
        int status = 0;
        union nvme_result result = {0};

        /* Handle shutdown first thing in the morning. */
        if (unlikely(nvme_shutdown_req && self->tid == SHUTDOWN_THREAD)) {
            nvme_do_shutdown(FALSE);
            nvme_shutdown_req = FALSE;

            /* Really? */
            if (!found) continue;
        }

        if (storpu_task) {
            storpu_task->retval = process_storpu_task(storpu_task);
            enqueue_storpu_ftl_completion(storpu_task);
        } else {
            /* Normal NVMe commands */

            /* xil_printf("Worker %d get command from %d %d\n", self->tid, qid,
             */
            /*            sqe.common.opcode); */

            if (qid == 0)
                status = process_admin_command(&sqe, &result);
            else
                status = process_io_command(&sqe, &result);

            nvme_pcie_post_cqe(qid, status, sqe.common.command_id, result.u64);
        }
    }
}
