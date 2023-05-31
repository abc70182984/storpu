#define _GNU_SOURCE
#define __USE_GNU 1

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <error.h>
#include <signal.h>
#include <pthread.h>
#include <poll.h>

#include <config.h>
#include "proto.h"
#include "thread.h"
#include <libcoro.h>
#include <flash_config.h>
#include <fil.h>
#include <utils.h>

#include "liburing.h"
#include <ublksrv.h>
#include <ublksrv_aio.h>

static int backing_fd = -1;
static struct ublksrv_aio_ctx* aio_ctx = NULL;
static struct ublksrv_ctrl_dev* this_dev;

static struct aio_list submission_list;
static struct aio_list completion_list;

struct thread_control_info {
    struct ublksrv_dev* dev;
    int qid;
    pthread_t thread;
};

ppa_t address_to_ppa(struct flash_address* addr);
static int reap_uring(struct ublksrv_aio_ctx* ctx, int* got_efd);

static void sig_handler(int sig)
{
    fprintf(stderr, "got signal %d\n", sig);
    ublksrv_ctrl_stop_dev(this_dev);
}

static int queue_event(struct ublksrv_aio_ctx* ctx)
{
    struct io_uring* ring = (struct io_uring*)ctx->ctx_data;
    struct io_uring_sqe* sqe;

    sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        fprintf(stderr, "%s: uring run out of sqe\n", __func__);
        return -1;
    }

    io_uring_prep_poll_add(sqe, ctx->efd, POLLIN);
    io_uring_sqe_set_data64(sqe, 0);

    return 0;
}

struct ublksrv_aio* nvme_pcie_get_sqe(void) { return submission_list.head; }

void nvme_pcie_pop_sqe(void) { aio_list_pop(&submission_list); }

void nvme_pcie_post_completion(struct ublksrv_aio* aio, int result)
{
    aio->res = result;
    aio_list_add(&completion_list, aio);
}

static int submit_user_request(struct ublksrv_aio_ctx* ctx,
                               struct ublksrv_aio* req)
{
    aio_list_add(&submission_list, req);
    return FALSE;
}

static void* myssd_uring_io_handler_fn(void* data)
{
    struct ublksrv_aio_ctx* ctx = (struct ublksrv_aio_ctx*)data;
    unsigned dev_id = ctx->dev->ctrl_dev->dev_info.dev_id;
    struct io_uring ring;
    unsigned qd;
    int ret;

    qd = NR_WORKER_THREADS * 2;

    io_uring_queue_init(qd, &ring, 0);
    ret = io_uring_register_eventfd(&ring, ctx->efd);
    if (ret) {
        fprintf(stdout, "ublk dev %d fails to register eventfd\n", dev_id);
        return NULL;
    }

    ctx->ctx_data = &ring;

    tls_init();

    coro_init();
    worker_init(ftl_init);

    aio_list_init(&submission_list);
    aio_list_init(&completion_list);

    queue_event(ctx);
    io_uring_submit_and_wait(&ring, 0);

    while (!ublksrv_aio_ctx_dead(ctx)) {
        int got_efd = 0;
        struct ublksrv_aio* aio;

        ublksrv_aio_submit_worker(ctx, submit_user_request, NULL);
        while ((aio = nvme_pcie_get_sqe()) != NULL) {
            if (likely(nvme_worker_dispatch_aio(aio)))
                nvme_pcie_pop_sqe();
            else
                break;
        }

        reap_uring(ctx, &got_efd);

        worker_yield();

        ublksrv_aio_complete_worker(ctx, &completion_list);

        if (got_efd) queue_event(ctx);
        io_uring_submit_and_wait(&ring, 1);
    }

    nvme_request_shutdown();

    while (nvme_shutdown_busy()) {
        int got_efd;

        reap_uring(ctx, &got_efd);

        worker_yield();

        io_uring_submit(&ring);
    }

    return NULL;
}

static void* myssd_io_handler_fn(void* data)
{
    struct thread_control_info* info = (struct thread_control_info*)data;
    struct ublksrv_dev* dev = info->dev;
    unsigned dev_id = dev->ctrl_dev->dev_info.dev_id;
    unsigned short q_id = info->qid;
    struct ublksrv_queue* q;

    q = ublksrv_queue_init(dev, q_id, NULL);
    if (!q) {
        fprintf(stderr, "ublk dev %d queue %d init queue failed\n",
                dev->ctrl_dev->dev_info.dev_id, q_id);
        return NULL;
    }

    for (;;) {
        if (ublksrv_process_io(q) < 0) break;
    }

    ublksrv_queue_deinit(q);

    return NULL;
}

static int myssd_init_tgt(struct ublksrv_dev* dev, int type, int argc,
                          char* argv[])
{
    const struct ublksrv_ctrl_dev_info* info = &dev->ctrl_dev->dev_info;
    struct ublksrv_tgt_info* tgt = &dev->tgt;

    if (type != UBLKSRV_TGT_TYPE_LOOP) return -1;

    tgt->dev_size = CONFIG_STORAGE_CAPACITY_BYTES;
    /* tgt->dev_size = 32UL << 30; */
    tgt->tgt_ring_depth = info->queue_depth;
    tgt->nr_fds = 0;

    return 0;
}

static int myssd_handle_io_async(struct ublksrv_queue* q, int tag)
{
    const struct ublksrv_io_desc* iod = ublksrv_get_iod(q, tag);
    struct ublksrv_aio* req = ublksrv_aio_alloc_req(aio_ctx, 0);

    req->io = *iod;
    req->fd = backing_fd;
    req->id = ublksrv_aio_pid_tag(q->q_id, tag);
    ublksrv_aio_submit_req(aio_ctx, q, req);

    return 0;
}

static void myssd_set_parameters(struct ublksrv_ctrl_dev* cdev,
                                 const struct ublksrv_dev* dev)
{
    struct ublksrv_ctrl_dev_info* info = &cdev->dev_info;
    struct ublk_params p = {
        .types = UBLK_PARAM_TYPE_BASIC,
        .basic =
            {
                .attrs = UBLK_ATTR_VOLATILE_CACHE,
                .logical_bs_shift = SECTOR_SHIFT,
                .physical_bs_shift = FLASH_PG_SHIFT,
                .io_opt_shift = SECTOR_SHIFT,
                .io_min_shift = SECTOR_SHIFT,
                .max_sectors = info->max_io_buf_bytes >> 9,
                .dev_sectors = dev->tgt.dev_size >> 9,
            },
    };
    int ret;

    ret = ublksrv_ctrl_set_params(cdev, &p);
    if (ret)
        fprintf(stderr, "dev %d set basic parameter failed %d\n", info->dev_id,
                ret);
}

static int ublksrv_start_daemon(struct ublksrv_ctrl_dev* ctrl_dev)
{
    struct thread_control_info* thread_control;
    int dev_id = ctrl_dev->dev_info.dev_id;
    struct ublksrv_dev* dev;
    struct ublksrv_ctrl_dev_info* dinfo = &ctrl_dev->dev_info;
    pthread_t io_thread;
    int i, ret;

    if (ublksrv_ctrl_get_affinity(ctrl_dev) < 0) return -1;

    thread_control = (struct thread_control_info*)calloc(
        sizeof(struct thread_control_info), dinfo->nr_hw_queues);

    dev = ublksrv_dev_init(ctrl_dev);
    if (!dev) return -ENOMEM;

    aio_ctx = ublksrv_aio_ctx_init(dev, 0);
    if (!aio_ctx) {
        fprintf(stderr, "dev %d call ublk_aio_ctx_init failed\n", dev_id);
        return -ENOMEM;
    }

    pthread_create(&io_thread, NULL, myssd_uring_io_handler_fn, aio_ctx);

    for (i = 0; i < dinfo->nr_hw_queues; i++) {
        thread_control[i].dev = dev;
        thread_control[i].qid = i;
        pthread_create(&thread_control[i].thread, NULL, myssd_io_handler_fn,
                       &thread_control[i]);
    }

    myssd_set_parameters(ctrl_dev, dev);

    ret = ublksrv_ctrl_start_dev(ctrl_dev, getpid());
    if (ret < 0) goto fail;

    for (i = 0; i < dinfo->nr_hw_queues; i++)
        pthread_join(thread_control[i].thread, NULL);

    ublksrv_aio_ctx_shutdown(aio_ctx);
    pthread_join(io_thread, NULL);
    ublksrv_aio_ctx_deinit(aio_ctx);

fail:
    ublksrv_dev_deinit(dev);
    free(thread_control);
    return ret;
}

static void myssd_handle_event(struct ublksrv_queue* q)
{
    ublksrv_aio_handle_event(aio_ctx, q);
}

static struct ublksrv_tgt_type myssd_tgt_type = {
    .type = UBLKSRV_TGT_TYPE_LOOP,
    .name = "myssd",
    .init_tgt = myssd_init_tgt,
    .handle_io_async = myssd_handle_io_async,
    .handle_event = myssd_handle_event,
};

int main(int argc, char* argv[])
{
    static const struct option longopts[] = {{"backing_file", 1, NULL, 'f'},
                                             {NULL}};
    int opt;
    struct ublksrv_dev_data data = {
        .dev_id = -1,
        .max_io_buf_bytes = DEF_BUF_SIZE,
        .nr_hw_queues = DEF_NR_HW_QUEUES,
        .queue_depth = DEF_QD,
        .tgt_type = "myssd",
        .tgt_ops = &myssd_tgt_type,
        .flags = 0,
    };
    struct ublksrv_ctrl_dev* ctrl_dev;
    int ret;

    while ((opt = getopt_long(argc, argv, "f:", longopts, NULL)) != -1) {
        switch (opt) {
        case 'f':
            backing_fd = open(optarg, O_RDWR | O_DIRECT);
            if (backing_fd < 0) backing_fd = -1;
            break;
        }
    }

    if (signal(SIGTERM, sig_handler) == SIG_ERR)
        error(EXIT_FAILURE, errno, "signal");
    if (signal(SIGINT, sig_handler) == SIG_ERR)
        error(EXIT_FAILURE, errno, "signal");

    data.ublksrv_flags = UBLKSRV_F_NEED_EVENTFD;

    ctrl_dev = ublksrv_ctrl_init(&data);
    if (!ctrl_dev) error(EXIT_FAILURE, ENODEV, "ublksrv_ctrl_init");

    this_dev = ctrl_dev;

    ret = ublksrv_ctrl_add_dev(ctrl_dev);
    if (ret < 0) {
        error(0, -ret, "can't add dev %d", data.dev_id);
        goto fail;
    }

    ret = ublksrv_start_daemon(ctrl_dev);
    if (ret < 0) {
        error(0, -ret, "can't start daemon");
        goto fail_del_dev;
    }

    ublksrv_ctrl_del_dev(ctrl_dev);
    ublksrv_ctrl_deinit(ctrl_dev);
    return 0;

fail_del_dev:
    ublksrv_ctrl_del_dev(ctrl_dev);
fail:
    ublksrv_ctrl_deinit(ctrl_dev);

    return 1;
}

int printk(const char* fmt, ...)
{
    va_list arg;
    int r;

    va_start(arg, fmt);
    r = vprintf(fmt, arg);
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

    printf("\nKernel panic: %s\n", buf);

    exit(1);
}

static int submit_async_io(struct fil_task* task)
{
    struct io_uring* ring = (struct io_uring*)aio_ctx->ctx_data;
    struct io_uring_sqe* sqe;
    ppa_t ppa;

    sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        fprintf(stderr, "%s: uring run out of sqe\n", __func__);
        return -ENOMEM;
    }

    io_uring_sqe_set_data(sqe, task);

    ppa = address_to_ppa(&task->addr);
    switch (task->type) {
    case TXN_READ:
        io_uring_prep_read(sqe, backing_fd, (void*)task->data + task->offset,
                           task->length,
                           (off_t)ppa * FLASH_PG_SIZE + task->offset);
        break;
    case TXN_WRITE:
        io_uring_prep_write(sqe, backing_fd, (void*)task->data + task->offset,
                            task->length,
                            (off_t)ppa * FLASH_PG_SIZE + task->offset);
        break;
    }

    return 0;
}

int submit_flash_transaction(struct flash_transaction* txn)
{
    struct fil_task task;
    int r;

    if (txn->type == TXN_READ && txn->offset == FLASH_PG_SIZE &&
        txn->length == 1) {
        *(unsigned char*)txn->data = 0xff;
        return 0;
    }

    memset(&task, 0, sizeof(task));
    task.addr = txn->addr;
    task.source = txn->source;
    task.type = txn->type;
    task.data = (uint64_t)txn->data;
    task.offset = txn->offset;
    task.length = txn->length;
    task.status = 0;
    task.completed = FALSE;
    task.opaque = (uint64_t)worker_self();
    task.err_bitmap = 0;

    printf("t%d s%d ch%d w%d d%d pl%d b%d p%d\n", txn->type, txn->source,
           txn->addr.channel, txn->addr.chip, txn->addr.die, txn->addr.plane,
           txn->addr.block, txn->addr.page);

    r = submit_async_io(&task);
    if (r < 0) return -r;

    r = worker_wait_timeout(WT_BLOCKED_ON_FIL, 3000);
    if (r) return r;

    return task.status == FTS_ERROR ? EIO : 0;
}

static int reap_uring(struct ublksrv_aio_ctx* ctx, int* got_efd)
{
    struct io_uring* ring = (struct io_uring*)ctx->ctx_data;
    struct io_uring_cqe* cqe;
    unsigned head;
    int count = 0;

    io_uring_for_each_cqe(ring, head, cqe)
    {
        if (cqe->user_data) {
            struct fil_task* task = (struct fil_task*)cqe->user_data;

            if (cqe->res == -EAGAIN)
                submit_async_io(task);
            else {
                struct worker_thread* worker =
                    (struct worker_thread*)task->opaque;

                task->completed = TRUE;
                task->status = (cqe->res != task->length) ? FTS_ERROR : FTS_OK;
                task->err_bitmap = 0;

                worker_wake(worker, WT_BLOCKED_ON_FIL);
            }
        } else {
            if (cqe->res < 0) fprintf(stderr, "eventfd result %d\n", cqe->res);
            *got_efd = 1;
        }

        count++;
    }
    io_uring_cq_advance(ring, count);

    return count;
}

int ecc_correct(u8* data, size_t data_length, const u8* code,
                size_t code_length, uint64_t err_bitmap)
{
    return 0;
}
