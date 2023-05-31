#include "xil_assert.h"
#include "xzdma.h"
#include "xparameters.h"
#include <sys/types.h>
#include <errno.h>

#include <bitmap.h>
#include "thread.h"
#include <intr.h>
#include <list.h>
#include <utils.h>
#include <dma.h>
#include <iov_iter.h>

#define NR_ZDMA_CHANNELS (XPAR_XZDMA_NUM_INSTANCES - 1)

#define ZDMA_BD_MAX 32

#define ZDMA_SIZE_THRESHOLD 4096

static struct zdma_config {
    int dev_id;
    int irq;
} zdma_config[NR_ZDMA_CHANNELS] = {
    /* FSBL uses channel 0 to do ECC initialization so don't touch it. */
    /* {XPAR_XZDMA_0_DEVICE_ID, XPAR_XADMAPS_0_INTR}, */
    {XPAR_XZDMA_1_DEVICE_ID, XPAR_XADMAPS_1_INTR},
    {XPAR_XZDMA_2_DEVICE_ID, XPAR_XADMAPS_2_INTR},
    {XPAR_XZDMA_3_DEVICE_ID, XPAR_XADMAPS_3_INTR},
    {XPAR_XZDMA_4_DEVICE_ID, XPAR_XADMAPS_4_INTR},
    {XPAR_XZDMA_5_DEVICE_ID, XPAR_XADMAPS_5_INTR},
    {XPAR_XZDMA_6_DEVICE_ID, XPAR_XADMAPS_6_INTR},
    {XPAR_XZDMA_7_DEVICE_ID, XPAR_XADMAPS_7_INTR},
    {XPAR_XZDMA_8_DEVICE_ID, XPAR_XZDMAPS_0_INTR},
    {XPAR_XZDMA_9_DEVICE_ID, XPAR_XZDMAPS_1_INTR},
    {XPAR_XZDMA_10_DEVICE_ID, XPAR_XZDMAPS_2_INTR},
    {XPAR_XZDMA_11_DEVICE_ID, XPAR_XZDMAPS_3_INTR},
    {XPAR_XZDMA_12_DEVICE_ID, XPAR_XZDMAPS_4_INTR},
    {XPAR_XZDMA_13_DEVICE_ID, XPAR_XZDMAPS_5_INTR},
    {XPAR_XZDMA_14_DEVICE_ID, XPAR_XZDMAPS_6_INTR},
    {XPAR_XZDMA_15_DEVICE_ID, XPAR_XZDMAPS_7_INTR},
};

struct zdma_chan {
    int index;
    XZDma zdma;
    struct worker_thread* owner;
    int error;
};

static struct zdma_chan zdma_chan[NR_ZDMA_CHANNELS];

static u8 zdma_bd_buffer[NR_ZDMA_CHANNELS * ZDMA_BD_MAX * 2 *
                         sizeof(XZDma_LiDscr)] __attribute__((aligned(64)));

static bitchunk_t channel_busy[BITCHUNKS(NR_ZDMA_CHANNELS)];
static mutex_t channel_mutex;
static cond_t channel_cond;

static void zdma_done_handler(void* callback)
{
    struct zdma_chan* chan = list_entry(callback, struct zdma_chan, zdma);
    if (!chan->owner) {
        WARN_ONCE(TRUE, "Spurious ZDMA interrupt");
        return;
    }
    chan->error = FALSE;
    worker_wake(chan->owner, WT_BLOCKED_ON_ZDMA);
}

static void zdma_error_handler(void* callback, u32 mask)
{
    struct zdma_chan* chan = list_entry(callback, struct zdma_chan, zdma);
    WARN(TRUE, "ZDMA error mask=%x", mask);
    if (!chan->owner) return;
    chan->error = TRUE;
    worker_wake(chan->owner, WT_BLOCKED_ON_ZDMA);
}

int zdma_setup(void)
{
    int i;
    XZDma_Config* config;
    XZDma_DataConfig configure;
    u8* bd_buffer = zdma_bd_buffer;
    int status;

    if (mutex_init(&channel_mutex, NULL) != 0) {
        panic("failed to initialize ZDMA channel mutex");
    }
    if (cond_init(&channel_cond, NULL) != 0) {
        panic("failed to initialize ZDMA channel condvar");
    }

    for (i = 0; i < NR_ZDMA_CHANNELS; i++) {
        struct zdma_config* cfg = &zdma_config[i];
        struct zdma_chan* chan = &zdma_chan[i];

        chan->index = i;

        config = XZDma_LookupConfig(cfg->dev_id);
        if (!config) return XST_FAILURE;

        status = XZDma_CfgInitialize(&chan->zdma, config, config->BaseAddress);
        if (status != XST_SUCCESS) return status;

        status = XZDma_SelfTest(&chan->zdma);
        if (status != XST_SUCCESS) return status;

        status = XZDma_SetMode(&chan->zdma, TRUE, XZDMA_NORMAL_MODE);
        if (status != XST_SUCCESS) return status;

        XZDma_CreateBDList(&chan->zdma, XZDMA_LINEAR, (UINTPTR)bd_buffer,
                           ZDMA_BD_MAX * 2 * sizeof(XZDma_LiDscr));

        XZDma_SetCallBack(&chan->zdma, XZDMA_HANDLER_DONE,
                          (void*)zdma_done_handler, &chan->zdma);
        XZDma_SetCallBack(&chan->zdma, XZDMA_HANDLER_ERROR,
                          (void*)zdma_error_handler, &chan->zdma);

        XZDma_GetChDataConfig(&chan->zdma, &configure);
        configure.OverFetch = 0;
        configure.SrcIssue = 0x1F;
        configure.SrcBurstType = XZDMA_INCR_BURST;
        configure.SrcBurstLen = 0xF;
        configure.DstBurstType = XZDMA_INCR_BURST;
        configure.DstBurstLen = 0xF;
        if (config->IsCacheCoherent) {
            configure.SrcCache = 0xF;
            configure.DstCache = 0xF;
        }
        XZDma_SetChDataConfig(&chan->zdma, &configure);

        status = intr_setup_irq(cfg->irq, 0x3, (irq_handler_t)XZDma_IntrHandler,
                                (void*)&chan->zdma);
        if (status != XST_SUCCESS) {
            xil_printf("Failed to setup XZDMA IRQ for channel %d\r\n", i);
            return XST_FAILURE;
        }

        intr_enable_irq(cfg->irq);

        XZDma_EnableIntr(&chan->zdma,
                         (XZDMA_IXR_DMA_DONE_MASK | XZDMA_IXR_DMA_PAUSE_MASK));

        bd_buffer += ZDMA_BD_MAX * 2 * sizeof(XZDma_LiDscr);
    }

    return XST_SUCCESS;
}

static struct zdma_chan* allocate_channel(void)
{
    int index;

    mutex_lock(&channel_mutex);

    while ((index = find_next_zero_bit(channel_busy, NR_ZDMA_CHANNELS, 0)) >=
           NR_ZDMA_CHANNELS) {
        cond_wait(&channel_cond, &channel_mutex);
    }

    SET_BIT(channel_busy, index);
    mutex_unlock(&channel_mutex);

    return &zdma_chan[index];
}

static void release_channel(struct zdma_chan* chan)
{
    chan->owner = NULL;

    mutex_lock(&channel_mutex);
    UNSET_BIT(channel_busy, chan->index);
    cond_signal(&channel_cond);
    mutex_unlock(&channel_mutex);
}

int zdma_memcpy(void* dst, const void* src, size_t n)
{
    struct zdma_chan* chan;
    XZDma_Transfer data;
    struct worker_thread* self = worker_self();
    unsigned long flags;
    int r = 0;

    chan = allocate_channel();
    Xil_AssertNonvoid(chan != NULL);

    dma_sync_single_for_device((void*)src, n, DMA_TO_DEVICE);
    dma_sync_single_for_device(dst, n, DMA_FROM_DEVICE);

    memset(&data, 0, sizeof(data));
    data.SrcAddr = (UINTPTR)src;
    data.Size = n;
    data.DstAddr = (UINTPTR)dst;

    chan->owner = self;
    chan->error = FALSE;

    local_irq_save(&flags);

    XZDma_Start(&chan->zdma, &data, 1);
    worker_wait(WT_BLOCKED_ON_ZDMA);

    local_irq_restore(flags);

    if (chan->error) {
        r = EIO;
        goto out;
    }

    dma_sync_single_for_cpu((void*)src, n, DMA_TO_DEVICE);
    dma_sync_single_for_cpu(dst, n, DMA_FROM_DEVICE);

out:
    release_channel(chan);
    return r;
}

ssize_t zdma_iter_copy_from(struct iov_iter* iter, void* buf, size_t bytes,
                            int sync_cache)
{
    struct worker_thread* self = worker_self();
    struct zdma_chan* chan;
    XZDma_Transfer descs[ZDMA_BD_MAX];
    int i, nr_descs = 0;
    size_t copied = 0;
    size_t chunk;
    unsigned long flags;
    int r = 0;

    if (bytes < ZDMA_SIZE_THRESHOLD && !sync_cache)
        return iov_iter_copy_from(iter, buf, bytes);

    chan = allocate_channel();
    Xil_AssertNonvoid(chan != NULL);

    chan->owner = self;
    chan->error = FALSE;

    while (copied < bytes && iter->nr_segs > 0) {
        if (nr_descs == ZDMA_BD_MAX) {
            local_irq_save(&flags);

            XZDma_Start(&chan->zdma, descs, nr_descs);
            worker_wait(WT_BLOCKED_ON_ZDMA);

            local_irq_restore(flags);

            if (chan->error) {
                r = EIO;
                goto out;
            }

            if (sync_cache) {
                for (i = 0; i < nr_descs; i++) {
                    XZDma_Transfer* data = &descs[i];
                    dma_sync_single_for_cpu((void*)data->SrcAddr, data->Size,
                                            DMA_TO_DEVICE);
                    dma_sync_single_for_cpu((void*)data->DstAddr, data->Size,
                                            DMA_FROM_DEVICE);
                }
            }

            nr_descs = 0;
        }

        chunk = min(iter->iov->iov_len - iter->iov_offset, bytes - copied);

        if ((iter->iov->iov_base != NULL) && (chunk > 0)) {
            XZDma_Transfer* data = &descs[nr_descs++];
            memset(data, 0, sizeof(*data));
            data->SrcAddr = (UINTPTR)(iter->iov->iov_base + iter->iov_offset);
            data->DstAddr = (UINTPTR)buf;
            data->Size = chunk;

            if (sync_cache) {
                dma_sync_single_for_device((void*)data->SrcAddr, data->Size,
                                           DMA_TO_DEVICE);
                dma_sync_single_for_device((void*)data->DstAddr, data->Size,
                                           DMA_FROM_DEVICE);
            }
        }

        copied += chunk;
        iter->iov_offset += chunk;
        buf += chunk;

        if (iter->iov_offset == iter->iov->iov_len) {
            iter->iov++;
            iter->nr_segs--;
            iter->iov_offset = 0;
        }
    }

    if (nr_descs > 0) {
        local_irq_save(&flags);

        XZDma_Start(&chan->zdma, descs, nr_descs);
        worker_wait(WT_BLOCKED_ON_ZDMA);

        local_irq_restore(flags);

        if (chan->error) {
            r = EIO;
            goto out;
        }

        if (sync_cache) {
            for (i = 0; i < nr_descs; i++) {
                XZDma_Transfer* data = &descs[i];
                dma_sync_single_for_cpu((void*)data->SrcAddr, data->Size,
                                        DMA_TO_DEVICE);
                dma_sync_single_for_cpu((void*)data->DstAddr, data->Size,
                                        DMA_FROM_DEVICE);
            }
        }
    }

out:
    release_channel(chan);
    return r ? (-r) : copied;
}

ssize_t zdma_iter_copy_to(struct iov_iter* iter, const void* buf, size_t bytes,
                          int sync_cache)
{
    struct worker_thread* self = worker_self();
    struct zdma_chan* chan;
    XZDma_Transfer descs[ZDMA_BD_MAX];
    int i, nr_descs = 0;
    size_t copied = 0;
    size_t chunk;
    unsigned long flags;
    int r = 0;

    if (bytes < ZDMA_SIZE_THRESHOLD && !sync_cache)
        return iov_iter_copy_to(iter, buf, bytes);

    chan = allocate_channel();
    Xil_AssertNonvoid(chan != NULL);

    chan->owner = self;
    chan->error = FALSE;

    while (copied < bytes && iter->nr_segs > 0) {
        if (nr_descs == ZDMA_BD_MAX) {
            local_irq_save(&flags);

            XZDma_Start(&chan->zdma, descs, nr_descs);
            worker_wait(WT_BLOCKED_ON_ZDMA);

            local_irq_restore(flags);

            if (chan->error) {
                r = EIO;
                goto out;
            }

            if (sync_cache) {
                for (i = 0; i < nr_descs; i++) {
                    XZDma_Transfer* data = &descs[i];
                    dma_sync_single_for_cpu((void*)data->SrcAddr, data->Size,
                                            DMA_TO_DEVICE);
                    dma_sync_single_for_cpu((void*)data->DstAddr, data->Size,
                                            DMA_FROM_DEVICE);
                }
            }

            nr_descs = 0;
        }

        chunk = min(iter->iov->iov_len - iter->iov_offset, bytes - copied);

        if ((iter->iov->iov_base != NULL) && (chunk > 0)) {
            XZDma_Transfer* data = &descs[nr_descs++];
            memset(data, 0, sizeof(*data));
            data->DstAddr = (UINTPTR)(iter->iov->iov_base + iter->iov_offset);
            data->SrcAddr = (UINTPTR)buf;
            data->Size = chunk;

            if (sync_cache) {
                dma_sync_single_for_device((void*)data->SrcAddr, data->Size,
                                           DMA_TO_DEVICE);
                dma_sync_single_for_device((void*)data->DstAddr, data->Size,
                                           DMA_FROM_DEVICE);
            }
        }

        copied += chunk;
        iter->iov_offset += chunk;
        buf += chunk;

        if (iter->iov_offset == iter->iov->iov_len) {
            iter->iov++;
            iter->nr_segs--;
            iter->iov_offset = 0;
        }
    }

    if (nr_descs > 0) {
        local_irq_save(&flags);

        XZDma_Start(&chan->zdma, descs, nr_descs);
        worker_wait(WT_BLOCKED_ON_ZDMA);

        local_irq_restore(flags);

        if (chan->error) {
            r = EIO;
            goto out;
        }

        if (sync_cache) {
            for (i = 0; i < nr_descs; i++) {
                XZDma_Transfer* data = &descs[i];
                dma_sync_single_for_cpu((void*)data->SrcAddr, data->Size,
                                        DMA_TO_DEVICE);
                dma_sync_single_for_cpu((void*)data->DstAddr, data->Size,
                                        DMA_FROM_DEVICE);
            }
        }
    }

out:
    release_channel(chan);
    return r ? (-r) : copied;
}
