#include "xaxidma.h"
#include "xil_assert.h"

#include "bch_engine.h"

struct bch_hard_engine {
    struct bch_engine base;

    unsigned int step_size;
    unsigned int code_size;

    XAxiDma axi_dma;
    unsigned int pending_tx_bufs;
    unsigned int pending_rx_bufs;
};

#define to_hard_engine(base) ((struct bch_hard_engine*)(base))

static struct bch_hard_engine hard_engine;

static void bch_hard_cleanup(struct bch_engine* base)
{
    struct bch_hard_engine* engine = to_hard_engine(base);
    XAxiDma_Reset(&engine->axi_dma);
}

static int bch_hard_correct(struct bch_engine* base, u8* data,
                            size_t data_length, const u8* code,
                            uint32_t* code_length, uint64_t err_bitmap)
{
    struct bch_hard_engine* engine = to_hard_engine(base);
    int nblocks = ((data_length + engine->step_size - 1) / engine->step_size);
    unsigned tx_count, rx_count;
    XAxiDma_BdRing* ring;
    XAxiDma_Bd *bd, *first_bd;
    int status;
    int i;

    rx_count = tx_count = 0;

    for (i = 0; i < nblocks; i++) {
        if (err_bitmap & (1ULL << i)) {
            rx_count += 1;
            tx_count += 2;
        }
    }

    /* Setup RX first */
    {
        ring = XAxiDma_GetRxRing(&engine->axi_dma);

        status = XAxiDma_BdRingAlloc(ring, rx_count, &bd);
        if (status != XST_SUCCESS) return -ENOMEM;

        first_bd = bd;

        for (i = 0; i < nblocks; i++) {
            if (err_bitmap & (1ULL << i)) {
                status = XAxiDma_BdSetBufAddr(
                    bd, (UINTPTR)(data + i * engine->step_size));
                if (status != XST_SUCCESS) return -EIO;

                status = XAxiDma_BdSetLength(bd, engine->step_size,
                                             ring->MaxTransferLen);
                if (status != XST_SUCCESS) return -EIO;

                XAxiDma_BdSetCtrl(bd, 0);
                XAxiDma_BdSetId(bd, (UINTPTR)i);

                bd = (XAxiDma_Bd*)XAxiDma_BdRingNext(ring, bd);
            }
        }

        status = XAxiDma_BdRingToHw(ring, rx_count, first_bd);
        if (status != XST_SUCCESS) return -EIO;
    }

    /* Now TX */
    {
        ring = XAxiDma_GetTxRing(&engine->axi_dma);

        status = XAxiDma_BdRingAlloc(ring, tx_count, &bd);
        if (status != XST_SUCCESS) return -ENOMEM;

        first_bd = bd;

        for (i = 0; i < nblocks; i++) {
            if (err_bitmap & (1ULL << i)) {
                status = XAxiDma_BdSetBufAddr(
                    bd, (UINTPTR)(data + i * engine->step_size));
                if (status != XST_SUCCESS) return -EIO;

                status = XAxiDma_BdSetLength(bd, engine->step_size,
                                             ring->MaxTransferLen);
                if (status != XST_SUCCESS) return -EIO;

                XAxiDma_BdSetCtrl(bd, XAXIDMA_BD_CTRL_TXSOF_MASK);
                XAxiDma_BdSetId(bd, (UINTPTR)(i * 2));

                bd = (XAxiDma_Bd*)XAxiDma_BdRingNext(ring, bd);

                status = XAxiDma_BdSetBufAddr(
                    bd, (UINTPTR)(code + i * engine->code_size));
                if (status != XST_SUCCESS) return -EIO;

                status = XAxiDma_BdSetLength(bd, engine->code_size,
                                             ring->MaxTransferLen);
                if (status != XST_SUCCESS) return -EIO;

                XAxiDma_BdSetCtrl(bd, XAXIDMA_BD_CTRL_TXEOF_MASK);
                XAxiDma_BdSetId(bd, (UINTPTR)(i * 2 + 1));

                bd = (XAxiDma_Bd*)XAxiDma_BdRingNext(ring, bd);
            }
        }

        status = XAxiDma_BdRingToHw(ring, tx_count, first_bd);
        if (status != XST_SUCCESS) return -EIO;
    }

    engine->pending_rx_bufs = rx_count;
    engine->pending_tx_bufs = tx_count;

    return 0;
}

int bch_hard_is_ready(struct bch_engine* base)
{
    struct bch_hard_engine* engine = to_hard_engine(base);
    XAxiDma_BdRing* ring;
    XAxiDma_Bd* bd;
    int bd_count;

    if (engine->pending_rx_bufs > 0) {
        ring = XAxiDma_GetRxRing(&engine->axi_dma);

        bd_count = XAxiDma_BdRingFromHw(ring, XAXIDMA_ALL_BDS, &bd);
        if (bd_count) {
            Xil_AssertNonvoid(bd_count <= engine->pending_rx_bufs);
            XAxiDma_BdRingFree(ring, bd_count, bd);
            engine->pending_rx_bufs -= bd_count;
        }
    }

    if (engine->pending_tx_bufs > 0) {
        ring = XAxiDma_GetTxRing(&engine->axi_dma);

        bd_count = XAxiDma_BdRingFromHw(ring, XAXIDMA_ALL_BDS, &bd);
        if (bd_count) {
            Xil_AssertNonvoid(bd_count <= engine->pending_tx_bufs);
            XAxiDma_BdRingFree(ring, bd_count, bd);
            engine->pending_tx_bufs -= bd_count;
        }
    }

    return (engine->pending_rx_bufs == 0) && (engine->pending_tx_bufs == 0);
}

static const struct bch_ops bch_hard_ops = {
    .correct = bch_hard_correct,
    .is_ready = bch_hard_is_ready,
    .cleanup = bch_hard_cleanup,
};

static int setup_bdring(XAxiDma_BdRing* ring, void* bd_buf, size_t bd_size)
{
    XAxiDma_Bd bd_template;
    int bd_count;
    int status;

    XAxiDma_BdRingIntDisable(ring, XAXIDMA_IRQ_ALL_MASK);

    bd_count = XAxiDma_BdRingCntCalc(XAXIDMA_BD_MINIMUM_ALIGNMENT, bd_size);

    status = XAxiDma_BdRingCreate(ring, (UINTPTR)bd_buf, (UINTPTR)bd_buf,
                                  XAXIDMA_BD_MINIMUM_ALIGNMENT, bd_count);
    if (status != XST_SUCCESS) return status;

    XAxiDma_BdClear(&bd_template);
    status = XAxiDma_BdRingClone(ring, &bd_template);
    if (status != XST_SUCCESS) return status;

    status = XAxiDma_BdRingSetCoalesce(ring, 1, 0);
    if (status != XST_SUCCESS) return XST_FAILURE;

    status = XAxiDma_BdRingStart(ring);
    return status;
}

struct bch_engine* bch_engine_init_hard(unsigned int ecc_size,
                                        unsigned int ecc_bytes, u16 dma_id,
                                        void* rx_bdbuf, void* tx_bdbuf,
                                        size_t bd_size)
{
    XAxiDma_Config* dma_config;
    struct bch_hard_engine* engine = &hard_engine;
    int status;

    dma_config = XAxiDma_LookupConfig(dma_id);
    if (!dma_config) {
        xil_printf("No config found for %d\r\n", dma_id);
        return NULL;
    }

    status = XAxiDma_CfgInitialize(&engine->axi_dma, dma_config);
    if (status != XST_SUCCESS) {
        xil_printf("Initialization failed %d\r\n", status);
        return NULL;
    }

    if (!XAxiDma_HasSg(&engine->axi_dma)) {
        xil_printf("Device not configured as SG mode \r\n");
        return NULL;
    }

    status =
        setup_bdring(XAxiDma_GetRxRing(&engine->axi_dma), rx_bdbuf, bd_size);
    if (status != XST_SUCCESS) {
        xil_printf("RX initialization failed %d\r\n", status);
        return NULL;
    }

    status =
        setup_bdring(XAxiDma_GetTxRing(&engine->axi_dma), tx_bdbuf, bd_size);
    if (status != XST_SUCCESS) {
        xil_printf("TX initialization failed %d\r\n", status);
        return NULL;
    }

    engine->step_size = ecc_size;
    engine->code_size = ecc_bytes;

    engine->pending_rx_bufs = 0;
    engine->pending_tx_bufs = 0;

    engine->base.ops = &bch_hard_ops;

    return &engine->base;
}
