#include "xparameters.h"
#include "xaxidma.h"
#include "xil_mmu.h"

#include <errno.h>

#include <config.h>
#include <const.h>
#include "../proto.h"
#include "../thread.h"
#include <iov_iter.h>
#include <list.h>
#include <utils.h>
#include "../tls.h"
#include <barrier.h>
#include <intr.h>
#include <page.h>
#include <dma.h>
#include <memalloc.h>

#include "nvme.h"
#include "nvme_pcie.h"

#define DMA_DEV_ID XPAR_AXI_DMA_1_DEVICE_ID

#define STATE_INTR_ID XPAR_FABRIC_NVME_PCIE_TOP_0_STATE_INTROUT_INTR
#define RX_INTR_ID    XPAR_FABRIC_NVME_PCIE_TOP_0_RC_INTROUT_INTR
#define TX_INTR_ID    XPAR_FABRIC_AXI_DMA_1_MM2S_INTROUT_INTR

#define TLP_REQ_MEMORY_READ  0
#define TLP_REQ_MEMORY_WRITE 1

#define RQ_TLP_HEADER_SIZE 32

/* This should match PCIE_DMA_TAG_WIDTH of the IP core. */
#define TAG_MAX 64

#define MAX_PCIE_MRD_REQUEST_SIZE \
    4096 /* 1024 DWs, 11 bits dword count in TLP */

#define PCIE_MAX_READ_REQUEST 256

/* Should match IN_FIFO_DEPTH of the requester completion interface. */
#define COMPLETION_FIFO_SIZE 16384UL /* 16k */

struct requester_request {
    unsigned long addr;
    u8 at;
    size_t byte_count;
    u8 req_type;
    u8 tag;
    u16 completer_id;
};

struct rq_queue_entry {
    struct worker_thread* thread;
    void* rx_buf;
    u16 rx_count;
    int completed;
};

/* A single data buffer presented to the requester interface for DMA read/write.
 */
struct rq_buffer_desc {
    u8 tlp_header[RQ_TLP_HEADER_SIZE];
    u8* data;
    size_t length;
    u8 tag;
    u8 __padding[15];
};

struct nvme_pcie_ctlr {
    void* reg_base;
    XAxiDma axi_dma;
    int dma_dev_id;

    int state_intr;
    int rx_intr;
    int tx_intr;

    phys_addr_t tx_bdring_phys;
    u8* tx_bdring;

    struct rq_queue_entry rq_queue[TAG_MAX];
    size_t rqes_in_used;
    mutex_t rq_queue_mutex;
    cond_t rq_queue_cond;

    size_t tx_free_slots;
    mutex_t tx_slot_mutex;
    cond_t tx_slot_cond;

    size_t rxfifo_avail;
    mutex_t rxfifo_mutex;
    cond_t rxfifo_cond;

    size_t max_dma_read_payload_size;
    size_t max_dma_write_payload_size;
    unsigned int dma_align;
};

#define RQ_BUFFER_MAX 16
static DEFINE_TLS(struct rq_buffer_desc, rq_buffers[RQ_BUFFER_MAX])
    __attribute__((aligned(64)));

static struct nvme_pcie_ctlr nvme_ctlr;

static int setup_rx_dma(struct nvme_pcie_ctlr* ctlr);
static int setup_tx_dma(struct nvme_pcie_ctlr* ctlr);
static void state_intr_handler(void* callback);
static void rx_dma_intr_handler(void* callback);
static void tx_dma_intr_handler(void* callback);

static inline u32 nvme_pcie_readl(struct nvme_pcie_ctlr* ctlr, int reg)
{
    return *(volatile u32*)(ctlr->reg_base + reg);
}

static inline u32 nvme_pcie_readl_acquire(struct nvme_pcie_ctlr* ctlr, int reg)
{
    volatile u32* addr = ctlr->reg_base + reg;
    return __atomic_load_n(addr, __ATOMIC_ACQUIRE);
}

static inline void nvme_pcie_writel(struct nvme_pcie_ctlr* ctlr, int reg,
                                    u32 val)
{
    *(volatile u32*)(ctlr->reg_base + reg) = val;
}

static inline void nvme_pcie_writel_release(struct nvme_pcie_ctlr* ctlr,
                                            int reg, u32 val)
{
    volatile u32* addr = ctlr->reg_base + reg;
    __atomic_store_n(addr, val, __ATOMIC_RELEASE);
}

static inline u64 nvme_pcie_readq(struct nvme_pcie_ctlr* ctlr, int reg)
{
    u32 lo, hi;

    lo = *(volatile u32*)(ctlr->reg_base + reg);
    hi = *(volatile u32*)(ctlr->reg_base + reg + 4);

    return ((u64)hi << 32UL) | lo;
}

static inline void nvme_pcie_writeq(struct nvme_pcie_ctlr* ctlr, int reg,
                                    u64 val)
{
    *(volatile u32*)(ctlr->reg_base + reg) = (u32)val;
    *(volatile u32*)(ctlr->reg_base + reg + 4) = (u32)(val >> 32UL);
}

static int __nvme_pcie_init_txdma(struct nvme_pcie_ctlr* ctlr)
{
    XAxiDma_Config* cfg_ptr;
    int status;

    cfg_ptr = XAxiDma_LookupConfig(ctlr->dma_dev_id);
    if (!cfg_ptr) {
        xil_printf("No config found for %d\r\n", ctlr->dma_dev_id);
        return XST_FAILURE;
    }

    status = XAxiDma_CfgInitialize(&ctlr->axi_dma, cfg_ptr);
    if (status != XST_SUCCESS) {
        xil_printf("Initialization failed %d\r\n", status);
        return XST_FAILURE;
    }

    if (!XAxiDma_HasSg(&ctlr->axi_dma)) {
        xil_printf("Device configured as simple mode \r\n");
        return XST_FAILURE;
    }

    status = setup_tx_dma(ctlr);
    if (status != XST_SUCCESS) {
        xil_printf("Failed to setup TX buffer descriptor ring\r\n");
        return XST_FAILURE;
    }

    status = intr_setup_irq(ctlr->tx_intr, 0x3,
                            (Xil_InterruptHandler)tx_dma_intr_handler, ctlr);
    if (status != XST_SUCCESS) {
        xil_printf("Failed to setup TX DMA IRQ\r\n");
        return XST_FAILURE;
    }

    intr_enable_irq(ctlr->tx_intr);

    return status;
}

static void __nvme_pcie_stop_txdma(struct nvme_pcie_ctlr* ctlr)
{
    intr_disable_irq(ctlr->tx_intr);
    intr_disconnect_irq(ctlr->tx_intr);
}

static int __nvme_pcie_init(struct nvme_pcie_ctlr* ctlr, void* reg_base,
                            unsigned int dma_dev_id, int state_intr,
                            int rx_intr, int tx_intr)
{
    phys_addr_t bdring_phys;
    mutexattr_t mutex_attr = {
        .tag = LKT_NVME_PCIE,
    };
    int status;

    ctlr->reg_base = reg_base;
    ctlr->max_dma_read_payload_size = MAX_PCIE_MRD_REQUEST_SIZE;

    bdring_phys = alloc_mem(2 << 20, 2 << 20, ZONE_PS_DDR_LOW);
    ctlr->tx_bdring_phys = bdring_phys;
    ctlr->tx_bdring = ioremap_nc(bdring_phys, 2 << 20);

    if (mutex_init(&ctlr->rq_queue_mutex, &mutex_attr) != 0) {
        panic("failed to initialize RQ queue mutex");
    }
    if (cond_init(&ctlr->rq_queue_cond, NULL) != 0) {
        panic("failed to initialize RQ queue condvar");
    }

    ctlr->dma_dev_id = dma_dev_id;

    status = setup_rx_dma(ctlr);
    if (status != XST_SUCCESS) {
        xil_printf("Failed to setup RX DMA\r\n");
        return XST_FAILURE;
    }

    ctlr->state_intr = state_intr;
    ctlr->rx_intr = rx_intr;
    ctlr->tx_intr = tx_intr;

    status = intr_setup_irq(ctlr->state_intr, 0x1,
                            (irq_handler_t)state_intr_handler, ctlr);
    if (status != XST_SUCCESS) {
        xil_printf("Failed to setup state change IRQ\r\n");
        return XST_FAILURE;
    }

    status = intr_setup_irq(ctlr->rx_intr, 0x1,
                            (irq_handler_t)rx_dma_intr_handler, ctlr);
    if (status != XST_SUCCESS) {
        xil_printf("Failed to setup RX DMA IRQ\r\n");
        return XST_FAILURE;
    }

    intr_enable_irq(ctlr->state_intr);
    intr_enable_irq(ctlr->rx_intr);

#if LAZY_DMA_INIT
    if (nvme_pcie_readl(ctlr, NVME_PCIE_STATUS_REG) & NVME_PCIE_SR_LINK_UP)
#endif
        __nvme_pcie_init_txdma(ctlr);

    nvme_pcie_writel(ctlr, NVME_PCIE_INT_ENABLE_REG,
                     NVME_PCIE_INT_CC_EN | NVME_PCIE_INT_CC_SHN |
                         NVME_PCIE_INT_SQ_FIFO | NVME_PCIE_INT_LNK_CHG);

    return XST_SUCCESS;
}

static inline void nvme_pcie_enable(struct nvme_pcie_ctlr* ctlr)
{
    nvme_pcie_writel(ctlr, NVME_PCIE_STATUS_REG, NVME_PCIE_SR_RDY);
    nvme_pcie_writel(ctlr, NVME_PCIE_ADMINQ_REG,
                     NVME_PCIE_ADMINQ_SQ_VALID | NVME_PCIE_ADMINQ_CQ_VALID |
                         NVME_PCIE_ADMINQ_IRQ_EN);
}

static inline void nvme_pcie_disable(struct nvme_pcie_ctlr* ctlr)
{
    nvme_pcie_writel(ctlr, NVME_PCIE_ADMINQ_REG, 0);
    nvme_pcie_writel(ctlr, NVME_PCIE_STATUS_REG, 0);
}

static inline void nvme_pcie_set_shst(struct nvme_pcie_ctlr* ctlr, u32 shst)
{
    u32 status;

    status =
        nvme_pcie_readl(ctlr, NVME_PCIE_STATUS_REG) & ~NVME_PCIE_SR_SHST_MASK;

    nvme_pcie_writel(ctlr, NVME_PCIE_STATUS_REG,
                     status | ((shst >> 2) << NVME_PCIE_SR_SHST_SHIFT));
}

static inline void nvme_pcie_send_rxcmd(struct nvme_pcie_ctlr* ctlr, u8 tag,
                                        u64 addr, size_t len)
{
    nvme_pcie_writeq(ctlr, NVME_PCIE_RX_CMD_ADDR_REG, addr);
    nvme_pcie_writel_release(ctlr, NVME_PCIE_RX_CMD_TAGLEN_REG,
                             (len & 0xffff) |
                                 ((u32)tag << NVME_PCIE_RC_TAG_SHIFT));
}

static inline void nvme_pcie_setup_rcdma_bd(struct nvme_pcie_ctlr* ctlr, u8 tag,
                                            void* addr, size_t len)
{
    nvme_pcie_writeq(ctlr, NVME_PCIE_RCDMA_BD_ADDR_REG, (u64)(UINTPTR)addr);
    nvme_pcie_writel_release(ctlr, NVME_PCIE_RCDMA_BD_TAGLEN_REG,
                             (len & 0xffff) |
                                 ((u32)tag << NVME_PCIE_RCDMA_TAG_SHIFT));
}

static inline u64 nvme_pcie_get_ioc_bitmap(struct nvme_pcie_ctlr* ctlr)
{
    return nvme_pcie_readq(ctlr, NVME_PCIE_RCDMA_IOC_BITMAP_REG);
}

static inline u64 nvme_pcie_get_err_bitmap(struct nvme_pcie_ctlr* ctlr)
{
    return nvme_pcie_readq(ctlr, NVME_PCIE_RCDMA_ERR_BITMAP_REG);
}

static inline void nvme_pcie_rcdma_ack_intr(struct nvme_pcie_ctlr* ctlr,
                                            u64 bitmap)
{
    nvme_pcie_writeq(ctlr, NVME_PCIE_RCDMA_IOC_ACK_REG, bitmap);
}

static inline void __nvme_pcie_config_sq(struct nvme_pcie_ctlr* ctlr, u16 qid,
                                         u64 bs_addr, u16 size, u16 cq_vec,
                                         int valid)
{
    unsigned int reg_base =
        NVME_PCIE_SQ_REG_BASE + (qid - 1) * NVME_PCIE_SQ_REG_SIZE;
    nvme_pcie_writeq(ctlr, reg_base + NVME_PCIE_SQ_BS_ADDR_REG, bs_addr);
    nvme_pcie_writel(ctlr, reg_base + NVME_PCIE_SQ_SIZE_REG,
                     (((u32)cq_vec) << 16) | size);
    nvme_pcie_writel(ctlr, reg_base + NVME_PCIE_SQ_VALID_REG, !!valid);
}

static inline void __nvme_pcie_config_cq(struct nvme_pcie_ctlr* ctlr, u16 qid,
                                         u64 bs_addr, u16 size, u16 irq_vec,
                                         int valid, int irqen)
{
    unsigned int reg_base =
        NVME_PCIE_CQ_REG_BASE + (qid - 1) * NVME_PCIE_CQ_REG_SIZE;
    nvme_pcie_writeq(ctlr, reg_base + NVME_PCIE_CQ_BS_ADDR_REG, bs_addr);
    nvme_pcie_writel(ctlr, reg_base + NVME_PCIE_CQ_SIZE_REG,
                     (((u32)irq_vec) << 16) | size);
    nvme_pcie_writel(ctlr, reg_base + NVME_PCIE_CQ_VALID_REG,
                     ((!!irqen) << 1) | (!!valid));
}

static int __nvme_pcie_get_sqe(struct nvme_pcie_ctlr* ctlr, u16* qidp,
                               struct nvme_command* sqe)
{
    int i;
    u32 status;
    u32* sqe_dws = (u32*)sqe;

    status = nvme_pcie_readl_acquire(ctlr, NVME_PCIE_SQ_FIFO_STATUS_REG);
    if (!(status & NVME_PCIE_SQF_SR_RDY)) return 0;

    if (qidp) *qidp = status & NVME_PCIE_SQF_SR_QID_MASK;

    for (i = 0; i < 16; i++)
        sqe_dws[i] =
            nvme_pcie_readl(ctlr, NVME_PCIE_SQ_FIFO_SQE_REG + (i << 2));

    return 1;
}

static inline void __nvme_pcie_pop_sqe(struct nvme_pcie_ctlr* ctlr)
{
    u32 status;
    u16 old_seq, seq;

    status = nvme_pcie_readl_acquire(ctlr, NVME_PCIE_SQ_FIFO_STATUS_REG);
    old_seq =
        (status & NVME_PCIE_SQF_SR_SEQ_MASK) >> NVME_PCIE_SQF_SR_SEQ_SHIFT;

    nvme_pcie_writel_release(ctlr, NVME_PCIE_SQ_FIFO_ACK_REG, 1);

    do {
        status = nvme_pcie_readl(ctlr, NVME_PCIE_SQ_FIFO_STATUS_REG);
        seq =
            (status & NVME_PCIE_SQF_SR_SEQ_MASK) >> NVME_PCIE_SQF_SR_SEQ_SHIFT;
    } while (old_seq == seq);

    dmb(sy);
}

static inline void __nvme_pcie_post_cqe(struct nvme_pcie_ctlr* ctlr, u16 sqid,
                                        u16 status, u16 cmdid, u64 result)
{
    nvme_pcie_writel(ctlr, NVME_PCIE_CQ_FIFO_SQID_REG, sqid & 0xffff);
    nvme_pcie_writel(ctlr, NVME_PCIE_CQ_FIFO_RESULT_REG, (u32)result);
    if ((result >> 32) != 0) {
        nvme_pcie_writel(ctlr, NVME_PCIE_CQ_FIFO_RESULT_REG + 4,
                         (u32)(result >> 32));
    }
    nvme_pcie_writel_release(ctlr, NVME_PCIE_CQ_FIFO_STATUS_REG,
                             (status & 0xffff) | ((cmdid & 0xffff) << 16));
}

static int setup_rx_dma(struct nvme_pcie_ctlr* ctlr)
{
    mutexattr_t mutex_attr = {
        .tag = LKT_NVME_PCIE,
    };

    if (mutex_init(&ctlr->rxfifo_mutex, &mutex_attr) != 0) {
        panic("failed to initialize completion FIFO mutex");
    }
    if (cond_init(&ctlr->rxfifo_cond, NULL) != 0) {
        panic("failed to initialize completion FIFO condvar");
    }

    ctlr->rxfifo_avail = COMPLETION_FIFO_SIZE -
                         CONFIG_NVME_IO_QUEUE_MAX * sizeof(struct nvme_command);

    return XST_SUCCESS;
}

static int setup_tx_dma(struct nvme_pcie_ctlr* ctlr)
{
    XAxiDma_BdRing* tx_ring;
    u32 max_transfer_len;
    XAxiDma_Bd bd_template;
    u8* bdr_buffer;
    phys_addr_t bdr_buffer_phys;
    int bd_count;
    int status;
    mutexattr_t mutex_attr = {
        .tag = LKT_NVME_PCIE,
    };

    if (mutex_init(&ctlr->tx_slot_mutex, &mutex_attr) != 0) {
        panic("failed to initialize RQ TX slot mutex");
    }
    if (cond_init(&ctlr->tx_slot_cond, NULL) != 0) {
        panic("failed to initialize RQ TX slot condvar");
    }

    tx_ring = XAxiDma_GetTxRing(&ctlr->axi_dma);
    max_transfer_len = tx_ring->MaxTransferLen;

    max_transfer_len = 1 << (31 - clz(max_transfer_len));
    ctlr->max_dma_write_payload_size = max_transfer_len;

    ctlr->dma_align = tx_ring->DataWidth;

    bdr_buffer = ctlr->tx_bdring;
    bdr_buffer_phys = ctlr->tx_bdring_phys;

    XAxiDma_BdRingIntDisable(tx_ring, XAXIDMA_IRQ_ALL_MASK);

    bd_count = XAxiDma_BdRingCntCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT,
                                     (1 << 18)); /* 4096 slots */
    ctlr->tx_free_slots = bd_count;

    status = XAxiDma_BdRingCreate(tx_ring, (UINTPTR)bdr_buffer_phys,
                                  (UINTPTR)bdr_buffer,
                                  XAXIDMA_BD_MINIMUM_ALIGNMENT, bd_count);
    if (status != XST_SUCCESS) {
        xil_printf("Tx bd create failed with %d\r\n", status);
        return XST_FAILURE;
    }

    XAxiDma_BdClear(&bd_template);
    status = XAxiDma_BdRingClone(tx_ring, &bd_template);
    if (status != XST_SUCCESS) {
        xil_printf("Tx bd clone failed with %d\r\n", status);
        return XST_FAILURE;
    }

    XAxiDma_BdRingIntEnable(tx_ring, XAXIDMA_IRQ_ALL_MASK);

    status = XAxiDma_BdRingStart(tx_ring);
    if (status != XST_SUCCESS) {
        xil_printf("Tx start BD ring failed with %d\r\n", status);
        return XST_FAILURE;
    }

    return XST_SUCCESS;
}

/* This function and complete_rq() below should be called with interrupt
 * DISABLED because the interrupt handler touches the RQ table too. _force_
 * specifies whether to wait when no RQ table entry is available or just to
 * return an invalid tag. */
static int setup_rq(struct nvme_pcie_ctlr* ctlr, void* buf, size_t rx_count,
                    int force)
{
    int i;

    Xil_AssertNonvoid(ctlr->rqes_in_used <= TAG_MAX);

    if (force) {
        mutex_lock(&ctlr->rq_queue_mutex);

        while (ctlr->rqes_in_used == TAG_MAX) {
            cond_wait(&ctlr->rq_queue_cond, &ctlr->rq_queue_mutex);
        }

        mutex_unlock(&ctlr->rq_queue_mutex);
    }

    for (i = 0; i < TAG_MAX; i++) {
        struct rq_queue_entry* rqe = &ctlr->rq_queue[i];

        if (rqe->thread == NULL) {
            rqe->thread = worker_self();
            rqe->rx_buf = buf;
            rqe->rx_count = rx_count;
            rqe->completed = FALSE;

            nvme_pcie_setup_rcdma_bd(ctlr, i, buf, rx_count);

            ctlr->rqes_in_used++;
            return i;
        }
    }

    Xil_AssertNonvoid(!force);
    return -1;
}

static void complete_rq(struct nvme_pcie_ctlr* ctlr, int tag)
{
    struct rq_queue_entry* rqe = &ctlr->rq_queue[tag];

    Xil_AssertVoid(rqe->thread);
    Xil_AssertVoid(ctlr->rqes_in_used > 0);

    ctlr->rqes_in_used--;
    rqe->thread = NULL;
    rqe->rx_buf = NULL;
    rqe->rx_count = 0;
    rqe->completed = FALSE;

    if (ctlr->rqes_in_used == TAG_MAX - 1) cond_broadcast(&ctlr->rq_queue_cond);
}

static void complete_all_rqs(struct nvme_pcie_ctlr* ctlr,
                             const struct rq_buffer_desc* rqs, size_t nr_rqs)
{
    int i;
    struct worker_thread* self = worker_self();

    if (!nr_rqs) return;

    local_irq_disable();

    for (i = 0; i < nr_rqs; i++) {
        u8 tag = rqs[i].tag;
        struct rq_queue_entry* rqe = &ctlr->rq_queue[tag];

        Xil_AssertVoid(rqe->thread == self);

        complete_rq(ctlr, tag);
    }

    local_irq_enable();
}

static size_t pack_requester_request(const struct requester_request* req,
                                     const u8* buffer, size_t count,
                                     struct rq_buffer_desc* rq_buf)
{
    u32* dws = (u32*)rq_buf->tlp_header;
    size_t out_size;

    memset(rq_buf, 0, sizeof(*rq_buf));

    dws[0] = (req->addr & 0xfffffffc) | (req->at & 0x3);
    dws[1] = (req->addr >> 32) & 0xffffffff;
    dws[2] = (u32)req->byte_count;
    dws[3] = req->tag;
    dws[3] |= req->completer_id << 8;
    dws[3] |= (req->req_type & 0xf) << 24;

    out_size = 32;

    if (buffer) {
        if (count <= 16) {
            /* Piggybacking */
            *(u64*)&rq_buf->tlp_header[16] = *(u64*)buffer;
            *(u64*)&rq_buf->tlp_header[24] = *(u64*)&buffer[8];
        } else {
            rq_buf->data = (u8*)buffer;
            rq_buf->length = count;
            out_size += count;
        }
    }

    rq_buf->tag = req->tag;

    return out_size;
}

static void unpack_requester_request(const struct rq_buffer_desc* rq_buf,
                                     struct requester_request* req)
{
    const u32* dws = (const u32*)rq_buf->tlp_header;

    memset(req, 0, sizeof(*req));
    req->addr = ((unsigned long)dws[1] << 32) | (dws[0] & 0xfffffffc);
    req->at = dws[0] & 0x3;
    req->byte_count = (size_t)dws[2];
    req->tag = dws[3] & 0xff;
    req->completer_id = (dws[3] >> 8) & 0xffff;
    req->req_type = (dws[3] >> 24) & 0xf;
}

static int send_rq_packets_rx(struct nvme_pcie_ctlr* ctlr,
                              const struct rq_buffer_desc* bufs, size_t count)
{
    int i;
    struct requester_request req;

    mutex_lock(&ctlr->rxfifo_mutex);

    for (i = 0; i < count; i++) {
        const struct rq_buffer_desc* buf = &bufs[i];

        unpack_requester_request(buf, &req);

        while (req.byte_count > ctlr->rxfifo_avail) {
            cond_wait(&ctlr->rxfifo_cond, &ctlr->rxfifo_mutex);
        }

        Xil_AssertNonvoid(ctlr->rxfifo_avail >= req.byte_count);
        ctlr->rxfifo_avail -= req.byte_count;

        nvme_pcie_send_rxcmd(ctlr, req.tag, req.addr, req.byte_count);
    }

    mutex_unlock(&ctlr->rxfifo_mutex);

    return 0;
}

static int send_rq_packets_tx(struct nvme_pcie_ctlr* ctlr,
                              const struct rq_buffer_desc* bufs, size_t count)
{
    struct worker_thread* self = worker_self();
    XAxiDma_BdRing* tx_ring;
    XAxiDma_Bd *bd_ptr, *first_bd;
    int i;
    size_t bd_count;
    int status, r;

    bd_count = 0;
    for (i = 0; i < count; i++) {
        bd_count += 1;
        /* Need an extra BD for the data buffer. */
        if (bufs[i].data) bd_count += 1;
    }

    /* Secure enough buffer slots for submission. */
    mutex_lock(&ctlr->tx_slot_mutex);

    while (bd_count > ctlr->tx_free_slots) {
        cond_wait(&ctlr->tx_slot_cond, &ctlr->tx_slot_mutex);
    }

    Xil_AssertNonvoid(ctlr->tx_free_slots >= bd_count);
    ctlr->tx_free_slots -= bd_count;

    mutex_unlock(&ctlr->tx_slot_mutex);

    tx_ring = XAxiDma_GetTxRing(&ctlr->axi_dma);

    status = XAxiDma_BdRingAlloc(tx_ring, bd_count, &bd_ptr);
    if (status != XST_SUCCESS) {
        xil_printf("Bd ring alloc %d failed\r\n", bd_count);

        ctlr->tx_free_slots += bd_count;
        cond_broadcast(&ctlr->tx_slot_cond);

        return ENOSPC;
    }

    first_bd = bd_ptr;

    r = EINVAL;
    for (i = 0; i < count; i++) {
        const struct rq_buffer_desc* buf = &bufs[i];

        dma_sync_single_for_device((void*)buf->tlp_header, RQ_TLP_HEADER_SIZE,
                                   DMA_TO_DEVICE);

        /* Set up TLP header. */
        status = XAxiDma_BdSetBufAddr(bd_ptr, (UINTPTR)buf->tlp_header);
        if (status != XST_SUCCESS) {
            xil_printf("Tx set header addr %p on BD %p failed %d\r\n",
                       buf->tlp_header, bd_ptr, status);

            goto cleanup;
        }

        status = XAxiDma_BdSetLength(bd_ptr, RQ_TLP_HEADER_SIZE,
                                     tx_ring->MaxTransferLen);
        if (status != XST_SUCCESS) {
            xil_printf("Tx set length %d on BD %p failed %d\r\n",
                       RQ_TLP_HEADER_SIZE, bd_ptr, status);

            goto cleanup;
        }

        XAxiDma_BdSetCtrl(bd_ptr, (buf->data ? 0 : XAXIDMA_BD_CTRL_TXEOF_MASK) |
                                      XAXIDMA_BD_CTRL_TXSOF_MASK);
        XAxiDma_BdSetId(bd_ptr, (UINTPTR)self);

        if (buf->data) {
            /* Set up data part if necessary. */
            bd_ptr = (XAxiDma_Bd*)XAxiDma_BdRingNext(tx_ring, bd_ptr);

            status = XAxiDma_BdSetBufAddr(bd_ptr, (UINTPTR)buf->data);
            if (status != XST_SUCCESS) {
                xil_printf("Tx set buffer addr %p length %d on BD %p "
                           "failed %d\r\n",
                           buf->data, buf->length, bd_ptr, status);

                goto cleanup;
            }

            status = XAxiDma_BdSetLength(bd_ptr, buf->length,
                                         tx_ring->MaxTransferLen);
            if (status != XST_SUCCESS) {
                xil_printf("Tx set length %d on BD %p failed %d\r\n",
                           buf->length, bd_ptr, status);

                goto cleanup;
            }

            XAxiDma_BdSetCtrl(bd_ptr, XAXIDMA_BD_CTRL_TXEOF_MASK);
            XAxiDma_BdSetId(bd_ptr, (UINTPTR)self);
        }

        bd_ptr = (XAxiDma_Bd*)XAxiDma_BdRingNext(tx_ring, bd_ptr);
    }

    r = EIO;
    status = XAxiDma_BdRingToHw(tx_ring, bd_count, first_bd);
    if (status != XST_SUCCESS) {
        xil_printf("to hw failed %d\r\n", status);
        goto cleanup;
    }

    self->rq_error = FALSE;
    self->pending_rqs = bd_count;

    r = worker_wait_timeout(WT_BLOCKED_ON_PCIE_RQ, 3000);

    if (self->rq_error) return EIO;

    return r;

cleanup:
    XAxiDma_BdRingFree(tx_ring, bd_count, first_bd);

    ctlr->tx_free_slots += bd_count;
    cond_broadcast(&ctlr->tx_slot_cond);

    return r;
}

static inline int send_rq_packets_sync(struct nvme_pcie_ctlr* ctlr,
                                       int do_write,
                                       const struct rq_buffer_desc* bufs,
                                       size_t count)
{
    if (do_write)
        return send_rq_packets_tx(ctlr, bufs, count);
    else
        return send_rq_packets_rx(ctlr, bufs, count);
}

static int pcie_do_dma_iter(struct nvme_pcie_ctlr* ctlr, unsigned long addr,
                            int do_write, struct iov_iter* iter, size_t count)
{
    struct worker_thread* self = worker_self();
    struct requester_request req = {.at = 0, .completer_id = 0};
    size_t max_payload_size;
    struct rq_buffer_desc* rq_bufs = get_local_var(rq_buffers);
    size_t nbufs = 0;
    int tag = 0;
    int r;

    max_payload_size = do_write ? ctlr->max_dma_write_payload_size
                                : ctlr->max_dma_read_payload_size;

    while (count > 0) {
        struct rq_buffer_desc* rq_buf;
        void* buf = NULL;
        size_t chunk = count;

        if (chunk > max_payload_size) chunk = max_payload_size;

        iov_iter_get_bufaddr(iter, &buf, &chunk);

        if (unlikely(do_write && ((addr & (PCIE_MAX_READ_REQUEST - 1)) != 0))) {
            unsigned int rem =
                PCIE_MAX_READ_REQUEST - (addr & (PCIE_MAX_READ_REQUEST - 1));

            chunk = min(chunk, rem);
        }

        if (unlikely(do_write &&
                     ((unsigned long)buf & (ctlr->dma_align - 1)) != 0)) {
            unsigned int rem =
                ctlr->dma_align - ((unsigned long)buf & (ctlr->dma_align - 1));

            rem = min(rem, 16);
            chunk = min(chunk, rem);
        }

        iov_iter_consume(iter, chunk);

        if (!do_write) {
            local_irq_disable();
            tag = setup_rq(ctlr, buf, chunk, nbufs == 0);
            local_irq_enable();
        }

        if (nbufs >= RQ_BUFFER_MAX || (!do_write && tag < 0)) {
            /* Flush RQ buffers when full or we cannot allocate a tag from
             * the RQ table. */

            /* Disable interrupt from here so that we will not be woken up
             * by the interrupt handler before we start to wait. */
            local_irq_disable();

            Xil_AssertNonvoid(nbufs > 0);

            if (!do_write) {
                self->pending_rcs = nbufs;
                self->rc_error = FALSE;
            }

            r = send_rq_packets_sync(ctlr, do_write, rq_bufs, nbufs);
            if (r) {
                xil_printf("Send rq failed %d\n", r);
                local_irq_enable();
                if (!do_write) complete_all_rqs(ctlr, rq_bufs, nbufs);
                return r;
            }

            if (!do_write) {
                r = 0;

                if (self->pending_rcs) {
                    r = worker_wait_timeout(WT_BLOCKED_ON_PCIE_CPL, 3000);
                }

                local_irq_enable();
                complete_all_rqs(ctlr, rq_bufs, nbufs);

                if (self->rc_error) r = r ?: EIO;
                if (r) return r;
            } else {
                local_irq_enable();
            }

            nbufs = 0;

            if (!do_write && tag < 0) {
                /* Try to get another tag. */
                local_irq_disable();
                tag = setup_rq(ctlr, buf, chunk, TRUE);
                local_irq_enable();
                Xil_AssertNonvoid(tag >= 0);
            }
        }

        rq_buf = &rq_bufs[nbufs++];

        req.tag = tag;
        req.req_type = do_write ? TLP_REQ_MEMORY_WRITE : TLP_REQ_MEMORY_READ;
        req.addr = addr;
        req.byte_count = chunk;

        pack_requester_request(&req, do_write ? buf : NULL, chunk, rq_buf);

        count -= chunk;
        addr += chunk;
    }

    if (nbufs > 0) {
        local_irq_disable();

        if (!do_write) {
            self->pending_rcs = nbufs;
            self->rc_error = FALSE;
        }

        r = send_rq_packets_sync(ctlr, do_write, rq_bufs, nbufs);
        if (r) {
            xil_printf("Send rq failed %d\n", r);
            local_irq_enable();
            if (!do_write) complete_all_rqs(ctlr, rq_bufs, nbufs);
            return r;
        }

        if (!do_write) {
            r = 0;

            if (self->pending_rcs) {
                r = worker_wait_timeout(WT_BLOCKED_ON_PCIE_CPL, 3000);
            }
            local_irq_enable();
            complete_all_rqs(ctlr, rq_bufs, nbufs);

            if (self->rc_error) r = r ?: EIO;
            if (r) return r;
        } else {
            local_irq_enable();
        }
    }

    return 0;
}

int pcie_dma_read_iter(unsigned long addr, struct iov_iter* iter, size_t count)
{
    return pcie_do_dma_iter(&nvme_ctlr, addr, 0, iter, count);
}

int pcie_dma_write_iter(unsigned long addr, struct iov_iter* iter, size_t count)
{
    return pcie_do_dma_iter(&nvme_ctlr, addr, 1, iter, count);
}

int pcie_dma_read(unsigned long addr, u8* buffer, size_t count)
{
    struct iovec iov = {
        .iov_base = buffer,
        .iov_len = count,
    };
    struct iov_iter iter;

    iov_iter_init(&iter, &iov, 1, count);

    return pcie_dma_read_iter(addr, &iter, count);
}

int pcie_dma_write(unsigned long addr, const u8* buffer, size_t count)
{
    struct iovec iov = {
        .iov_base = (void*)buffer,
        .iov_len = count,
    };
    struct iov_iter iter;

    iov_iter_init(&iter, &iov, 1, count);

    return pcie_dma_write_iter(addr, &iter, count);
}

static void nvme_pcie_shutdown(struct nvme_pcie_ctlr* ctlr, int abrupt)
{
    nvme_pcie_set_shst(ctlr, NVME_CSTS_SHST_OCCUR);

    nvme_pcie_writel(ctlr, NVME_PCIE_ADMINQ_REG, 0);

    nvme_pcie_set_shst(ctlr, NVME_CSTS_SHST_CMPLT);

    nvme_request_shutdown();
}

static void state_intr_handler(void* callback)
{
    struct nvme_pcie_ctlr* ctlr = (struct nvme_pcie_ctlr*)callback;
    u32 irq_status, status;

    irq_status = nvme_pcie_readl(ctlr, NVME_PCIE_INT_STATUS_REG);
    nvme_pcie_writel(ctlr, NVME_PCIE_INT_STATUS_REG, irq_status);

    status = nvme_pcie_readl(ctlr, NVME_PCIE_STATUS_REG);

    if (irq_status & NVME_PCIE_INT_CC_EN) {
        if (status & NVME_PCIE_SR_EN)
            nvme_pcie_enable(ctlr);
        else
            nvme_pcie_disable(ctlr);
    }

    if (irq_status & NVME_PCIE_INT_CC_SHN) {
        u32 cc_shn = (status >> NVME_PCIE_SR_SHN_SHIFT) & 0x3;

        if (cc_shn != NVME_CC_SHN_NONE)
            nvme_pcie_shutdown(ctlr, cc_shn == NVME_CC_SHN_ABRUPT);
    }

    if (irq_status & NVME_PCIE_INT_LNK_CHG) {
        if (status & NVME_PCIE_SR_LINK_UP)
            __nvme_pcie_init_txdma(ctlr);
        else
            __nvme_pcie_stop_txdma(ctlr);
    }
}

static void rx_dma_callback(struct nvme_pcie_ctlr* ctlr, int tag, int error)
{
    struct rq_queue_entry* rqe;
    size_t nbufs;

    rqe = &ctlr->rq_queue[tag];
    if (unlikely(rqe->thread == NULL)) {
        xil_printf("Spurious RX DMA interrupt, tag %d\n", tag);
        return;
    }

    if (unlikely(rqe->completed)) {
        xil_printf("Spurious RX DMA interrupt for completed entry, tag %d\n",
                   tag);
        return;
    }

    ctlr->rxfifo_avail += rqe->rx_count;
    cond_broadcast(&ctlr->rxfifo_cond);

    rqe->thread->rc_error = error;
    rqe->completed = TRUE;

    nbufs = rqe->thread->pending_rcs;
    if (nbufs == 0)
        xil_printf("Worker blocked with nbufs == 0\n");
    else
        nbufs--;
    rqe->thread->pending_rcs = nbufs;

    if (!nbufs) {
        worker_wake(rqe->thread, WT_BLOCKED_ON_PCIE_CPL);
        sevl();
    }
}

static void rx_dma_intr_handler(void* callback)
{
    struct nvme_pcie_ctlr* ctlr = (struct nvme_pcie_ctlr*)callback;
    u64 ioc_bitmap, err_bitmap;
    int i;

    ioc_bitmap = nvme_pcie_get_ioc_bitmap(ctlr);
    err_bitmap = nvme_pcie_get_err_bitmap(ctlr);

    nvme_pcie_rcdma_ack_intr(ctlr, ioc_bitmap | err_bitmap);

    for (i = 0; i < 64; i++) {
        if ((ioc_bitmap & (1UL << i)) || (err_bitmap & (1UL << i)))
            rx_dma_callback(ctlr, i, !!(err_bitmap & (1UL << i)));
    }
}

static int tx_dma_callback(XAxiDma_BdRing* tx_ring)
{
    int bd_count;
    XAxiDma_Bd *bd_ptr, *bd_cur_ptr;
    struct worker_thread* worker;
    u32 bd_status;
    int i, status;

    bd_count = XAxiDma_BdRingFromHw(tx_ring, XAXIDMA_ALL_BDS, &bd_ptr);

    bd_cur_ptr = bd_ptr;

    for (i = 0; i < bd_count; i++) {
        bd_status = XAxiDma_BdGetSts(bd_cur_ptr);
        worker = (struct worker_thread*)(UINTPTR)XAxiDma_BdGetId(bd_cur_ptr);

        if ((bd_status & XAXIDMA_BD_STS_ALL_ERR_MASK) ||
            (!(bd_status & XAXIDMA_BD_STS_COMPLETE_MASK))) {
            xil_printf("RQ TX error worker %p\n", worker);
            worker->rq_error = TRUE;
        }

        if (worker->pending_rqs <= 0)
            xil_printf("Spurious RQ TX completion\n");
        else {
            if (--worker->pending_rqs == 0) {
                worker_wake(worker, WT_BLOCKED_ON_PCIE_RQ);
                sevl();
            }
        }

        bd_cur_ptr = (XAxiDma_Bd*)XAxiDma_BdRingNext(tx_ring, bd_cur_ptr);
    }

    if (bd_count > 0) {
        status = XAxiDma_BdRingFree(tx_ring, bd_count, bd_ptr);
        if (status != XST_SUCCESS) {
            xil_printf("TX bd ring free failed\r\n");
        }
    }

    return bd_count;
}

static void tx_dma_intr_handler(void* callback)
{
    struct nvme_pcie_ctlr* ctlr = (struct nvme_pcie_ctlr*)callback;
    XAxiDma_BdRing* tx_ring;
    u32 IrqStatus;
    int TimeOut;
    int found;

    tx_ring = XAxiDma_GetTxRing(&ctlr->axi_dma);

    IrqStatus = XAxiDma_BdRingGetIrq(tx_ring);
    XAxiDma_BdRingAckIrq(tx_ring, IrqStatus);

    if (!(IrqStatus & XAXIDMA_IRQ_ALL_MASK)) {
        return;
    }

    if ((IrqStatus & XAXIDMA_IRQ_ERROR_MASK)) {
        XAxiDma_BdRingDumpRegs(tx_ring);

        xil_printf("Reseting TX DMA\n");
        XAxiDma_Reset(&ctlr->axi_dma);
        TimeOut = 10000;
        while (TimeOut) {
            if (XAxiDma_ResetIsDone(&ctlr->axi_dma)) {
                break;
            }

            TimeOut -= 1;
        }

        return;
    }

    if (IrqStatus & (XAXIDMA_IRQ_DELAY_MASK | XAXIDMA_IRQ_IOC_MASK)) {
        found = tx_dma_callback(tx_ring);

        if (found) {
            ctlr->tx_free_slots += found;
            cond_broadcast(&ctlr->tx_slot_cond);
        }
    }
}

int nvme_pcie_init(u64 mm_base)
{
    int status;

    status = __nvme_pcie_init(
        &nvme_ctlr, (void*)(UINTPTR)XPAR_NVME_PCIE_TOP_0_BASEADDR, DMA_DEV_ID,
        STATE_INTR_ID, RX_INTR_ID, TX_INTR_ID);
    if (status != XST_SUCCESS) return status;

    nvme_pcie_writeq(&nvme_ctlr, NVME_PCIE_MM_BASE_ADDR_REG, mm_base);

    return XST_SUCCESS;
}

void nvme_pcie_config_sq(u16 qid, u64 bs_addr, u16 size, u16 cq_vec, int valid)
{
    __nvme_pcie_config_sq(&nvme_ctlr, qid, bs_addr, size, cq_vec, valid);
}

void nvme_pcie_config_cq(u16 qid, u64 bs_addr, u16 size, u16 irq_vec, int valid,
                         int irqen)
{
    __nvme_pcie_config_cq(&nvme_ctlr, qid, bs_addr, size, irq_vec, valid,
                          irqen);
}

int nvme_pcie_get_sqe(u16* qidp, struct nvme_command* sqe)
{
    return __nvme_pcie_get_sqe(&nvme_ctlr, qidp, sqe);
}

void nvme_pcie_pop_sqe(void) { __nvme_pcie_pop_sqe(&nvme_ctlr); }

void nvme_pcie_post_cqe(u16 sqid, u16 status, u16 cmdid, u64 result)
{
    __nvme_pcie_post_cqe(&nvme_ctlr, sqid, status, cmdid, result);
}

void nvme_pcie_report_stats(void)
{
    int i;

    for (i = 0; i < TAG_MAX; i++) {
        struct rq_queue_entry* rqe = &nvme_ctlr.rq_queue[i];
        xil_printf("RQE %d: ", i);
        if (rqe->thread == NULL) {
            xil_printf("empty");
        } else {
            xil_printf("thread %d buf %p len %d completed %d error %d",
                       rqe->thread->tid, rqe->rx_buf, rqe->rx_count,
                       rqe->completed, rqe->thread->rc_error);
        }
        xil_printf("\n");
    }
}
