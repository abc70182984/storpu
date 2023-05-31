#ifndef _HOSTIF_NVME_PCIE_H_
#define _HOSTIF_NVME_PCIE_H_

/* Registers */
#define NVME_PCIE_STATUS_REG       0x00
#define NVME_PCIE_ADMINQ_REG       0x04
#define NVME_PCIE_INT_STATUS_REG   0x08
#define NVME_PCIE_INT_ENABLE_REG   0x0c
#define NVME_PCIE_MM_BASE_ADDR_REG 0x10

#define NVME_PCIE_RCDMA_BD_ADDR_REG    0x100
#define NVME_PCIE_RCDMA_BD_TAGLEN_REG  0x108
#define NVME_PCIE_RCDMA_IOC_ACK_REG    0x110
#define NVME_PCIE_RCDMA_IOC_BITMAP_REG 0x100
#define NVME_PCIE_RCDMA_ERR_BITMAP_REG 0x120

#define NVME_PCIE_SQ_FIFO_STATUS_REG 0x200
#define NVME_PCIE_SQ_FIFO_ACK_REG    0x200
#define NVME_PCIE_SQ_FIFO_SQE_REG    0x208

#define NVME_PCIE_CQ_FIFO_STATUS_REG 0x400
#define NVME_PCIE_CQ_FIFO_SQID_REG   0x404
#define NVME_PCIE_CQ_FIFO_RESULT_REG 0x408

#define NVME_PCIE_RX_CMD_ADDR_REG   0x800
#define NVME_PCIE_RX_CMD_TAGLEN_REG 0x808

#define NVME_PCIE_SQ_REG_BASE    0x1000
#define NVME_PCIE_SQ_BS_ADDR_REG 0x0
#define NVME_PCIE_SQ_SIZE_REG    0x8
#define NVME_PCIE_SQ_VALID_REG   0xc
#define NVME_PCIE_SQ_REG_SIZE    16

#define NVME_PCIE_CQ_REG_BASE    0x2000
#define NVME_PCIE_CQ_BS_ADDR_REG 0x0
#define NVME_PCIE_CQ_SIZE_REG    0x8
#define NVME_PCIE_CQ_VALID_REG   0xc
#define NVME_PCIE_CQ_REG_SIZE    16

/* NVMe controller status */
#define NVME_PCIE_SR_EN         (1 << 0)
#define NVME_PCIE_SR_SHN_SHIFT  1
#define NVME_PCIE_SR_SHN_MASK   (3 << NVME_PCIE_SR_SHN_SHIFT)
#define NVME_PCIE_SR_RDY        (1 << 4)
#define NVME_PCIE_SR_SHST_SHIFT 5
#define NVME_PCIE_SR_SHST_MASK  (3 << NVME_PCIE_SR_SHST_SHIFT)
#define NVME_PCIE_SR_LINK_UP    (1 << 31)

/* Admin queue status */
#define NVME_PCIE_ADMINQ_SQ_VALID (1 << 0)
#define NVME_PCIE_ADMINQ_CQ_VALID (1 << 1)
#define NVME_PCIE_ADMINQ_IRQ_EN   (1 << 2)

/* Interrupt register */
#define NVME_PCIE_INT_CC_EN   (1 << 0)
#define NVME_PCIE_INT_CC_SHN  (1 << 1)
#define NVME_PCIE_INT_SQ_FIFO (1 << 2)
#define NVME_PCIE_INT_LNK_CHG (1 << 5)

/* RC DMA buffer descriptor */
#define NVME_PCIE_RCDMA_LENGTH_SHIFT 0
#define NVME_PCIE_RCDMA_LENGTH_WIDTH 16
#define NVME_PCIE_RCDMA_TAG_SHIFT    16
#define NVME_PCIE_RCDMA_TAG_WIDTH    6

/* SQ FIFO status */
#define NVME_PCIE_SQF_SR_RDY       (1 << 31)
#define NVME_PCIE_SQF_SR_SEQ_SHIFT 16
#define NVME_PCIE_SQF_SR_SEQ_MASK  (0x7fffUL << NVME_PCIE_SQF_SR_SEQ_SHIFT)
#define NVME_PCIE_SQF_SR_QID_MASK  (0xffff)

/* RX command registers */
#define NVME_PCIE_RC_LENGTH_SHIFT 0
#define NVME_PCIE_RC_LENGTH_WIDTH 13
#define NVME_PCIE_RC_TAG_SHIFT    16
#define NVME_PCIE_RC_TAG_WIDTH    8

int nvme_pcie_init(u64 mm_base);

void nvme_pcie_config_sq(u16 qid, u64 bs_addr, u16 size, u16 cq_vec, int valid);
void nvme_pcie_config_cq(u16 qid, u64 bs_addr, u16 size, u16 irq_vec, int valid,
                         int irqen);

int nvme_pcie_get_sqe(u16* qidp, struct nvme_command* sqe);
void nvme_pcie_pop_sqe(void);

void nvme_pcie_post_cqe(u16 sqid, u16 status, u16 cmdid, u64 result);

#endif
