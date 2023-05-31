/* Flash Interface Layer */

#include "xdebug.h"
#include "xil_assert.h"
#include "xparameters.h"
#include "xgpiops.h"
#include <sleep.h>
#include <stddef.h>
#include <errno.h>

#include <config.h>
#include <flash_config.h>
#include <ecc_config.h>
#include "fil.h"
#include "nfc.h"

#include <avl.h>
#include <proto.h>
#include <const.h>
#include <flash.h>
#include <fil.h>
#include <dma.h>
#include <profile.h>

/* #define USE_VOLUME_ADDRESS */

#define INPUT_DELAY_MAX  512
#define INPUT_DELAY_STEP 2

#define TRAINING_BLOCK 12
#define TRAINING_PAGE  1

struct chip_data;
struct channel_data;

struct die_data {
    /* Index within chip */
    unsigned int index;
    /* Unique ID for use of AVL keys */
    unsigned int unique_id;
    struct list_head list;
    /* In case of multiplane commands, there can be multiple active transactions
     * on one die. */
    struct list_head active_txns;
    /* Outstanding die list. */
    struct list_head completion;
    struct avl_node avl;

    struct flash_command cmd_buf;
    /* Active command dispatched to this die (including transfer and execution
     * phase). */
    struct flash_command* active_cmd;
    /* Command being _executed_ on this die. */
    struct flash_command* current_cmd;
    u64 cmd_finish_time;
    int cmd_error;

    struct chip_data* chip;

    struct fil_task* active_xfer;

    u64 exec_start_cycle;
};

enum chip_status {
    CS_IDLE,
    CS_CMD_DATA_IN,
    CS_WAIT_FOR_DATA_OUT,
    CS_DATA_OUT,
    CS_READING,
    CS_WRITING,
    CS_ERASING,
};

struct chip_data {
    int index;

    int ce_pin;
    u8 ce_gpio_bank;
    u8 ce_gpio_pin;

    enum chip_status status;
    /* Outstanding chip transfer list. */
    struct list_head completion;

    struct channel_data* channel;
    struct die_data dies[DIES_PER_CHIP];
    unsigned int active_dies;

    struct list_head cmd_xfer_queue;
    struct die_data* current_xfer;
    unsigned int nr_waiting_read_xfers;
    u64 last_xfer_start;
};

struct channel_data {
    int index;

    struct nf_controller nfc;

    int last_selected_chip;
    struct chip_data chips[CHIPS_PER_CHANNEL];

    struct list_head waiting_read_xfer;
};

static struct channel_data channel_data[NR_CHANNELS];
enum channel_status channel_status[NR_CHANNELS];

static int ce_pins[] = {CE_PINS};
static int wp_pins[] = {WP_PINS};

static XGpioPs ps_gpio_inst;

static struct nfc_config {
    uintptr_t base_addr;
    unsigned int sub_index;
    int dma_dev_id;
    unsigned int odt_config[CHIPS_PER_CHANNEL];
} nfc_configs[] = {
    {XPAR_ONFI_BCH_TOP_0_BASEADDR, 0, XPAR_AXI_DMA_2_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_TOP_0_BASEADDR, 1, XPAR_AXI_DMA_3_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_TOP_0_BASEADDR, 2, XPAR_AXI_DMA_4_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_TOP_0_BASEADDR, 3, XPAR_AXI_DMA_5_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_TOP_1_BASEADDR, 0, XPAR_AXI_DMA_6_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_TOP_1_BASEADDR, 1, XPAR_AXI_DMA_7_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_TOP_1_BASEADDR, 2, XPAR_AXI_DMA_8_DEVICE_ID, {0}},
    {XPAR_ONFI_BCH_TOP_1_BASEADDR, 3, XPAR_AXI_DMA_9_DEVICE_ID, {0}},
};

static struct list_head chip_in_transfer_list;
static struct avl_root die_command_avl;
static struct list_head chip_out_transfer_list;

static void complete_chip_transfer(struct chip_data* chip, u64 timestamp);
static void complete_data_out_transfer(struct chip_data* chip,
                                       struct die_data* die, u64 timestamp);

static int die_command_comp(struct die_data* r1, struct die_data* r2)
{
    if (r1->cmd_finish_time < r2->cmd_finish_time)
        return -1;
    else if (r1->cmd_finish_time > r2->cmd_finish_time)
        return 1;

    if (r1->unique_id < r2->unique_id)
        return -1;
    else if (r1->unique_id > r2->unique_id)
        return 1;

    return 0;
}

static int die_command_key_node_comp(void* key, struct avl_node* node)
{
    struct die_data* r1 = (struct die_data*)key;
    struct die_data* r2 = avl_entry(node, struct die_data, avl);

    return die_command_comp(r1, r2);
}

static int die_command_node_node_comp(struct avl_node* node1,
                                      struct avl_node* node2)
{
    struct die_data* r1 = avl_entry(node1, struct die_data, avl);
    struct die_data* r2 = avl_entry(node2, struct die_data, avl);

    return die_command_comp(r1, r2);
}

static inline u32 get_command_latency_us(struct chip_data* chip,
                                         enum flash_command_code cmd_code,
                                         unsigned int page_id)
{
    switch (cmd_code) {
    case FCMD_READ_PAGE:
    case FCMD_READ_PAGE_MULTIPLANE:
        return FLASH_READ_LATENCY_US;
    case FCMD_PROGRAM_PAGE:
    case FCMD_PROGRAM_PAGE_MULTIPLANE:
        return FLASH_PROGRAM_LATENCY_US;
    case FCMD_ERASE_BLOCK:
    case FCMD_ERASE_BLOCK_MULTIPLANE:
        return FLASH_ERASE_LATENCY_US;
    default:
        Xil_AssertNonvoid(FALSE);
        break;
    }

    return 0;
}

int fil_is_die_busy(unsigned int channel_nr, unsigned int chip_nr,
                    unsigned int die_nr, int is_program)
{
    struct chip_data* chip;

    /* Two rules for submitting multi-LUN (interleaved) operations:
     *
     * 1) A PROGRAM-series operation must be issued before the READ-series
     *    operation for multi-LUN operations.
     *
     * 2) After data is inputted to the first LUN addressed in that multi-LUN
     *    sequence, before addressing the next LUN with a PROGRAM-series
     *    operation, a program confirm command (via 80h-10h) must be issued to
     *    begin the array programming of the previous LUN.
     *
     * We use this function to mark a LUN busy so that any command violating the
     * above rules will be rejected. */

    Xil_AssertNonvoid(channel_nr < NR_CHANNELS);
    Xil_AssertNonvoid(chip_nr < CHIPS_PER_CHANNEL);
    Xil_AssertNonvoid(die_nr < DIES_PER_CHIP);

    chip = &channel_data[channel_nr].chips[chip_nr];

    if (is_program) {
        int i;

        for (i = 0; i < DIES_PER_CHIP; i++) {
            struct die_data* die = &chip->dies[i];

            /* If there is an ongoing read command on another LUN of this chip,
             * or there is a program command that has not entered array
             * programming state (die->current_cmd == NULL), then reject this
             * program command. */
            if (die->active_cmd &&
                (die->active_cmd->cmd_code == FCMD_READ_PAGE ||
                 die->active_cmd->cmd_code == FCMD_READ_PAGE_MULTIPLANE ||
                 die->current_cmd == NULL))
                return TRUE;
        }
    }

    return chip->dies[die_nr].active_cmd != NULL;
}

static inline enum channel_status
get_channel_status(struct channel_data* channel)
{
    return channel_status[channel->index];
}

static inline void set_channel_status(struct channel_data* channel,
                                      enum channel_status status)
{
    channel_status[channel->index] = status;
}

static inline void set_chip_status(struct chip_data* chip,
                                   enum chip_status status)
{
    chip->status = status;
}

static inline struct die_data* get_die(unsigned int channel, unsigned int chip,
                                       unsigned int die)
{
    Xil_AssertNonvoid(channel < NR_CHANNELS);
    Xil_AssertNonvoid(chip < CHIPS_PER_CHANNEL);
    Xil_AssertNonvoid(die < DIES_PER_CHIP);
    return &channel_data[channel].chips[chip].dies[die];
}

static inline void XGpioPs_WriteBankPin(const XGpioPs* InstancePtr, u8 Bank,
                                        u8 PinNumber, u32 Data)
{
    u32 RegOffset;
    u32 Value;
    u32 DataVar = Data;

    /* Hacky. I don't like branch prediction. */
    RegOffset = ((PinNumber >> 4) & 1) * XGPIOPS_DATA_MSW_OFFSET;
    PinNumber &= 0xf;

    /*
     * Get the 32 bit value to be written to the Mask/Data register where
     * the upper 16 bits is the mask and lower 16 bits is the data.
     */
    DataVar &= (u32)0x01;
    Value =
        ~((u32)1 << (PinNumber + 16U)) & ((DataVar << PinNumber) | 0xFFFF0000U);
    XGpioPs_WriteReg(InstancePtr->GpioConfig.BaseAddr,
                     ((u32)(Bank)*XGPIOPS_DATA_MASK_OFFSET) + RegOffset, Value);
}

static inline void write_ce(struct chip_data* chip, int enable)
{
    XGpioPs_WriteBankPin(&ps_gpio_inst, chip->ce_gpio_bank, chip->ce_gpio_pin,
                         !enable);
}

static inline void select_volume(struct chip_data* chip, int enable)
{
#ifdef USE_VOLUME_ADDRESS
    struct channel_data* channel = chip->channel;
    int i;

    /* CE# shall have remained HIGH for t_CEH in order for the VOLUME SELECT
     * command to be properly received. We do so by pulling CE# HIGH when
     * disabling the volume and pulling it back to LOW when a volume in the same
     * channel is enabled.  */
    for (i = 0; i < CHIPS_PER_CHANNEL; i++) {
        write_ce(&channel->chips[i], enable);
    }

    /* We only need to issue a VOLUME SELECT command if the volume to be
     * selected is different from the previous one; otherwise we can just use
     * volume reversion to re-select it. */
    if (enable && (channel->last_selected_chip != chip->index)) {
        nfc_cmd_volume_select(&channel->nfc, chip->index);
        channel->last_selected_chip = chip->index;
    }
#else
    write_ce(chip, enable);
#endif
}

static void init_die(struct die_data* die)
{
    INIT_LIST_HEAD(&die->active_txns);
    die->active_cmd = NULL;
}

static void init_chip(struct chip_data* chip, int ce_pin)
{
    int i;

    chip->ce_pin = ce_pin;

    /* Too damn slow, better memoize it. */
    XGpioPs_GetBankPin((u8)chip->ce_pin, &chip->ce_gpio_bank,
                       &chip->ce_gpio_pin);

    chip->nr_waiting_read_xfers = 0;
    INIT_LIST_HEAD(&chip->cmd_xfer_queue);

    for (i = 0; i < DIES_PER_CHIP; i++) {
        struct die_data* die = &chip->dies[i];
        die->index = i;
        die->chip = chip;

        die->unique_id =
            (chip->channel->index * CHIPS_PER_CHANNEL + chip->index) *
                DIES_PER_CHIP +
            i;

        init_die(die);
    }
}

static void init_channel(struct channel_data* channel)
{
    set_channel_status(channel, BUS_IDLE);
    channel->last_selected_chip = -1;
    INIT_LIST_HEAD(&channel->waiting_read_xfer);
}

static void alloc_flash_data(void)
{
    int ch_idx, chip_idx;
    int* ce_pp = ce_pins;
    struct channel_data* channel;
    struct chip_data* chip;

    for (ch_idx = 0; ch_idx < NR_CHANNELS; ch_idx++) {
        channel = &channel_data[ch_idx];

        /* Initialize channels first */
        channel->index = ch_idx;
        init_channel(channel);

        /* Match CEs to channels */
        for (chip_idx = 0; chip_idx < CHIPS_PER_CHANNEL; chip_idx++) {
            int ce_pin = *ce_pp++;

            chip = &channel->chips[chip_idx];
            chip->index = chip_idx;
            chip->channel = channel;
            init_chip(chip, ce_pin);
        }
    }
}

static void init_gpio(void)
{
    XGpioPs_Config* gpio_config_ptr;
    int i, status;

    gpio_config_ptr = XGpioPs_LookupConfig(XPAR_PSU_GPIO_0_DEVICE_ID);
    if (gpio_config_ptr == NULL) {
        panic("PS GPIO not found\n\r");
    }

    status = XGpioPs_CfgInitialize(&ps_gpio_inst, gpio_config_ptr,
                                   gpio_config_ptr->BaseAddr);
    if (status != XST_SUCCESS) {
        panic("PS GPIO init failed\n\r");
    }

    for (i = 0; i < sizeof(ce_pins) / sizeof(ce_pins[0]); i++) {
        XGpioPs_SetDirectionPin(&ps_gpio_inst, ce_pins[i], 1);
        XGpioPs_SetOutputEnablePin(&ps_gpio_inst, ce_pins[i], 1);
        XGpioPs_WritePin(&ps_gpio_inst, ce_pins[i], 1);
    }

    for (i = 0; i < sizeof(wp_pins) / sizeof(wp_pins[0]); i++) {
        XGpioPs_SetDirectionPin(&ps_gpio_inst, wp_pins[i], 1);
        XGpioPs_SetOutputEnablePin(&ps_gpio_inst, wp_pins[i], 1);
        XGpioPs_WritePin(&ps_gpio_inst, wp_pins[i], 1);
    }
}

static int erase_block_simple(struct nf_controller* nfc, struct chip_data* chip,
                              unsigned int die, unsigned int plane,
                              unsigned int block)
{
    int ready, error;

    select_volume(chip, TRUE);
    nfc_cmd_erase_block(nfc, die, plane, block);

    do {
        ready = nfc_is_ready(nfc, die, plane, &error);
    } while (!(ready || error));

    select_volume(chip, FALSE);

    return error ? EIO : 0;
}

static int program_page_simple(struct nf_controller* nfc,
                               struct chip_data* chip, unsigned int die,
                               unsigned int plane, unsigned int block,
                               unsigned int page, unsigned int col,
                               const u8* buffer, size_t count)
{
    int ready, error;

    select_volume(chip, TRUE);
    nfc_cmd_program_transfer(nfc, die, plane, block, page, col,
                             (u64)(UINTPTR)buffer, count);
    while (!nfc_transfer_done(nfc, NFC_TO_NAND))
        ;
    nfc_complete_transfer(nfc, NFC_TO_NAND, count, NULL);
    nfc_cmd_program_page(nfc);

    do {
        ready = nfc_is_ready(nfc, die, plane, &error);
    } while (!(ready || error));

    select_volume(chip, FALSE);

    return error ? EIO : 0;
}

static int read_page_simple(struct nf_controller* nfc, struct chip_data* chip,
                            unsigned int die, unsigned int plane,
                            unsigned int block, unsigned int page,
                            unsigned int col, u8* buffer, size_t count,
                            u8* code_buffer, u64* err_bitmap)
{
    int ready, error;

    select_volume(chip, TRUE);
    nfc_cmd_read_page_addr(nfc, die, plane, block, page, col);
    nfc_cmd_read_page(nfc);

    do {
        ready = nfc_is_ready(nfc, die, plane, &error);
    } while (!(ready || error));

    if (error) goto out;

    dma_sync_single_for_device(buffer, count, DMA_FROM_DEVICE);

    nfc_cmd_read_transfer(nfc, die, plane, (u64)(UINTPTR)buffer, count,
                          (u64)(UINTPTR)code_buffer);
    while (!nfc_transfer_done(nfc, NFC_FROM_NAND))
        ;
    nfc_complete_transfer(nfc, NFC_FROM_NAND, count, err_bitmap);

    dma_sync_single_for_cpu(buffer, count, DMA_FROM_DEVICE);

out:
    select_volume(chip, FALSE);

    return error ? EIO : 0;
}

static size_t read_page_test(struct nf_controller* nfc, struct chip_data* chip,
                             unsigned int die, unsigned int plane,
                             unsigned int block, unsigned int page,
                             unsigned int col, u8* buffer, size_t count,
                             u8* code_buffer, u64* err_bitmap,
                             const u8* gt_data, int bit)
{
    size_t err_count = 0;
    int i;

    read_page_simple(nfc, chip, die, plane, block, page, col, buffer, count,
                     code_buffer, err_bitmap);

    for (i = 0; i < count; i++) {
        if (bit == -1) {
            if (buffer[i] != gt_data[i]) err_count++;
        } else {
            if ((buffer[i] & (1 << bit)) != (gt_data[i] & (1 << bit)))
                err_count++;
        }
    }

    return err_count;
}

static int channel_selftest(struct channel_data* channel)
{
    static u8 buffer[2 * FLASH_PG_SIZE + FLASH_PG_OOB_SIZE]
        __attribute__((aligned(0x1000)));
    u8* tx_buffer;
    u8* rx_buffer;
    u8* code_buffer;
    struct nf_controller* nfc = &channel->nfc;
    int i;
    u64 err_bitmap[CHIPS_PER_CHANNEL];
    size_t err_count[CHIPS_PER_CHANNEL];

    tx_buffer = buffer;
    rx_buffer = &buffer[FLASH_PG_SIZE];
    code_buffer = &buffer[2 * FLASH_PG_SIZE];

    for (i = 0; i < FLASH_PG_SIZE; i++)
        tx_buffer[i] = i & 0xff;
    memset(rx_buffer, 0, FLASH_PG_SIZE);

    dma_sync_single_for_device(tx_buffer, FLASH_PG_SIZE, DMA_TO_DEVICE);
    dma_sync_single_for_device(rx_buffer, FLASH_PG_SIZE, DMA_BIDIRECTIONAL);
    dma_sync_single_for_device(code_buffer, FLASH_PG_OOB_SIZE,
                               DMA_BIDIRECTIONAL);

    for (i = 0; i < CHIPS_PER_CHANNEL; i++) {
        struct chip_data* chip = &channel->chips[i];

        // Erase block.
        erase_block_simple(nfc, chip, 0, 0, TRAINING_BLOCK);

        // Program page.
        program_page_simple(nfc, chip, 0, 0, TRAINING_BLOCK, TRAINING_PAGE, 0,
                            tx_buffer, FLASH_PG_SIZE);
    }

    int k;
    for (k = 0; k < 10; k++) {
        for (i = 0; i < CHIPS_PER_CHANNEL; i++) {
            struct chip_data* chip = &channel->chips[i];

            err_count[i] = read_page_test(
                nfc, chip, 0, 0, TRAINING_BLOCK, TRAINING_PAGE, 0, rx_buffer,
                FLASH_PG_SIZE, code_buffer, &err_bitmap[i], tx_buffer, -1);
        }

        xil_printf("%d err (", k);
        for (i = 0; i < CHIPS_PER_CHANNEL; i++) {
            if (i) xil_printf(" ");
            xil_printf("%d", err_count[i]);
        }
        xil_printf(")\n");
    }

    for (i = 0; i < CHIPS_PER_CHANNEL; i++) {
        struct chip_data* chip = &channel->chips[i];
        erase_block_simple(nfc, chip, 0, 0, TRAINING_BLOCK);
    }

    return TRUE;
}

static void reset_flash(void)
{
    int i, j;

    for (i = 0; i < NR_CHANNELS; i++) {
        struct nf_controller* nfc = &channel_data[i].nfc;

        for (j = 0; j < CHIPS_PER_CHANNEL; j++) {
            struct chip_data* chip = &channel_data[i].chips[j];
            write_ce(chip, TRUE);
            nfc_cmd_reset(nfc);
            write_ce(chip, FALSE);
        }
    }

    usleep(5000);

#ifdef USE_VOLUME_ADDRESS
    /* Appoint volume addresses. */
    for (i = 0; i < NR_CHANNELS; i++) {
        struct nf_controller* nfc = &channel_data[i].nfc;

        for (j = 0; j < CHIPS_PER_CHANNEL; j++) {
            struct chip_data* chip = &channel_data[i].chips[j];
            write_ce(chip, TRUE);
            nfc_cmd_set_feature(nfc, 0x58, chip->index);
            usleep(1);
            write_ce(chip, FALSE);
        }
    }

    usleep(1);

    for (i = 0; i < NR_CHANNELS; i++) {
        struct nf_controller* nfc = &channel_data[i].nfc;
        struct nfc_config* config = &nfc_configs[i];
        struct chip_data* chip;

        for (j = 0; j < CHIPS_PER_CHANNEL; j++) {
            if (config->odt_config[j] == 0) continue;

            chip = &channel_data[i].chips[j];
            write_ce(chip, TRUE);
            nfc_cmd_volume_select(nfc, chip->index);
            usleep(1);
            nfc_cmd_odt_configure(nfc, 0, config->odt_config[j]);
            usleep(1);
            write_ce(chip, FALSE);
        }
    }
#endif

    for (i = 0; i < NR_CHANNELS; i++) {
        struct nf_controller* nfc = &channel_data[i].nfc;

        for (j = 0; j < CHIPS_PER_CHANNEL; j++) {
            struct chip_data* chip = &channel_data[i].chips[j];
            int nvddr2_feat = 0x47;

            write_ce(chip, TRUE);
#ifdef USE_VOLUME_ADDRESS
            nfc_cmd_volume_select(nfc, chip->index);
            usleep(1);
#endif

            /* Configure RE/DQS differential signaling. */
            nfc_cmd_set_feature(nfc, 2, nvddr2_feat);
            usleep(1);
            /* Configure NV-DDR2 interface. */
            nfc_cmd_set_feature(nfc, 1, 0x67);
            usleep(1);

            write_ce(chip, FALSE);
        }

        nfc_cmd_enable_nvddr2(nfc);
    }

    usleep(10);

#ifdef NFC_SELFTEST
    usleep(1000000);

    for (i = 0; i < NR_CHANNELS; i++) {
        xil_printf("Channel %d: ", i);
        channel_selftest(&channel_data[i]);
    }
#endif
}

void fil_init(void)
{
    int i;

    INIT_AVL_ROOT(&die_command_avl, die_command_key_node_comp,
                  die_command_node_node_comp);
    INIT_LIST_HEAD(&chip_in_transfer_list);
    INIT_LIST_HEAD(&chip_out_transfer_list);

    alloc_flash_data();

    init_gpio();

    for (i = 0; i < NR_CHANNELS; i++) {
        struct nf_controller* nfc = &channel_data[i].nfc;
        struct nfc_config* config = &nfc_configs[i];

        nfc_init(nfc, (void*)config->base_addr, config->sub_index,
                 config->dma_dev_id, BCH_BLOCK_SIZE, BCH_CODE_SIZE);
    }

    reset_flash();
}

static int start_data_out_transfer(struct channel_data* channel)
{
    struct fil_task* txn = NULL;
    struct chip_data* chip;
    struct nf_controller* nfc;
    struct die_data* die;
    unsigned int start_step;
    size_t nr_steps;

    if (get_channel_status(channel) != BUS_IDLE) return FALSE;

    if (!list_empty(&channel->waiting_read_xfer))
        txn = list_entry(channel->waiting_read_xfer.next, struct fil_task,
                         waiting_list);

    if (!txn) return FALSE;

    list_del(&txn->waiting_list);

    chip = &channel->chips[txn->addr.chip];
    nfc = &channel->nfc;
    die = &chip->dies[txn->addr.die];
    Xil_AssertNonvoid(!die->active_xfer);

    set_chip_status(chip, CS_DATA_OUT);
    die->active_xfer = txn;
    set_channel_status(channel, BUS_BUSY);

    /* Only read transactions need data out transfer */
    Xil_AssertNonvoid(txn->type == TXN_READ);

    start_step = txn->offset / nfc->step_size;
    nr_steps = (txn->length + nfc->step_size - 1) / nfc->step_size;

    select_volume(chip, TRUE);

    chip->last_xfer_start = timer_get_cycles();

    if (txn->code_length) {
        nfc_cmd_read_transfer(&channel->nfc, txn->addr.die, txn->addr.plane,
                              txn->data + start_step * nfc->step_size,
                              nr_steps * nfc->step_size,
                              txn->code_buf + start_step * nfc->code_size);
    } else {
        nfc_cmd_read_transfer(&channel->nfc, txn->addr.die, txn->addr.plane,
                              txn->data, nr_steps * nfc->step_size, 0);
    }

    /* Add to outstanding die command list. */
    list_add_tail(&die->completion, &chip_out_transfer_list);

    return TRUE;
}

static int start_die_command(struct chip_data* chip, struct flash_command* cmd)
{
    struct die_data* die = &chip->dies[cmd->addrs[0].die];
    u64 now = timer_get_cycles();
    u32 cmd_latency_us;

    cmd_latency_us =
        get_command_latency_us(chip, cmd->cmd_code, cmd->addrs[0].page);
    die->exec_start_cycle = now;
    die->cmd_finish_time = now + timer_us_to_cycles(cmd_latency_us);
    die->current_cmd = cmd;
    die->cmd_error = FALSE;

    switch (cmd->cmd_code) {
    case FCMD_READ_PAGE:
        nfc_cmd_read_page(&chip->channel->nfc);
        break;
    case FCMD_PROGRAM_PAGE:
        nfc_cmd_program_page(&chip->channel->nfc);
        break;
    default:
        break;
    }

    /* Add to outstanding die command list. */
    avl_insert(&die->avl, &die_command_avl);

    chip->active_dies++;

    return TRUE;
}

static int start_cmd_data_transfer(struct chip_data* chip)
{
    struct die_data* die;
    struct fil_task* head;
    struct nf_controller* nfc;
    unsigned int start_step;
    size_t nr_steps;
    int completed = FALSE;
    u64 now = timer_get_cycles();

    Xil_AssertNonvoid(!chip->current_xfer);
    if (list_empty(&chip->cmd_xfer_queue)) return FALSE;

    nfc = &chip->channel->nfc;
    die = list_entry(chip->cmd_xfer_queue.next, struct die_data, list);
    list_del(&die->list);

    set_chip_status(chip, CS_CMD_DATA_IN);
    chip->current_xfer = die;

    /* TODO: handle multi-plane commands */
    head = list_entry(die->active_txns.next, struct fil_task, queue);

    select_volume(chip, TRUE);

    chip->last_xfer_start = now;

    start_step = head->offset / nfc->step_size;
    nr_steps = (head->length + nfc->step_size - 1) / nfc->step_size;

    switch (die->active_cmd->cmd_code) {
    case FCMD_READ_PAGE:
        /* If code is requested, we re-adjust the offset to point to the
         * real beginning of the ECC block. Otherwise the raw offset is
         * used. */
        nfc_cmd_read_page_addr(
            nfc, head->addr.die, head->addr.plane, head->addr.block,
            head->addr.page,
            (head->code_length > 0)
                ? (start_step * (nfc->step_size + nfc->code_size))
                : head->offset);
        completed = TRUE;
        break;
    case FCMD_PROGRAM_PAGE:
        nfc_cmd_program_transfer(nfc, head->addr.die, head->addr.plane,
                                 head->addr.block, head->addr.page,
                                 start_step * (nfc->step_size + nfc->code_size),
                                 head->data + start_step * nfc->step_size,
                                 nr_steps * nfc->step_size);
        break;
    case FCMD_ERASE_BLOCK:
        nfc_cmd_erase_block(&chip->channel->nfc, head->addr.die,
                            head->addr.plane, head->addr.block);
        completed = TRUE;
        break;
    default:
        xil_printf("Unsupported flash command (%d)\n",
                   die->active_cmd->cmd_code);
        break;
    }

    if (completed)
        complete_chip_transfer(chip, now);
    else
        list_add_tail(&chip->completion, &chip_in_transfer_list);

    return TRUE;
}

static void complete_chip_transfer(struct chip_data* chip, u64 timestamp)
{
    struct die_data* die = chip->current_xfer;
    struct fil_task* head =
        list_entry(die->active_txns.next, struct fil_task, queue);
    struct channel_data* channel = &channel_data[head->addr.channel];
    struct fil_task* txn;
    unsigned int xfer_time;

    xfer_time = timestamp - chip->last_xfer_start;

    Xil_AssertVoid(!list_empty(&die->active_txns));

    chip->current_xfer = NULL;

    list_for_each_entry(txn, &die->active_txns, queue)
    {
        txn->total_xfer_us += xfer_time;
    }

    start_die_command(chip, die->active_cmd);

    select_volume(chip, FALSE);

    if (!list_empty(&chip->cmd_xfer_queue)) {
        /* Try to interleave command execution & data transfer whenever we
         * can.
         */
        start_cmd_data_transfer(chip);
        return;
    }

    switch (head->type) {
    case TXN_READ:
        set_chip_status(chip, CS_READING);
        break;
    case TXN_WRITE:
        set_chip_status(chip, CS_WRITING);
        break;
    case TXN_ERASE:
        set_chip_status(chip, CS_ERASING);
        break;
    }

    set_channel_status(channel, BUS_IDLE);
    tsu_notify_channel_idle(channel->index);
}

static void complete_die_command(struct chip_data* chip, struct die_data* die,
                                 int error, u64 timestamp)
{
    struct flash_command* cmd = die->current_cmd;
    struct fil_task *txn, *tmp;
    unsigned int exec_cycles;
    int txn_completed = TRUE;

    exec_cycles = timestamp - die->exec_start_cycle;
    chip->active_dies--;
    die->current_cmd = NULL;
    die->cmd_finish_time = UINT64_MAX;

    switch (cmd->cmd_code) {
    case FCMD_READ_PAGE:
    case FCMD_READ_PAGE_MULTIPLANE:
        if (!error) {
            if (!chip->active_dies) set_chip_status(chip, CS_WAIT_FOR_DATA_OUT);

            list_for_each_entry(txn, &die->active_txns, queue)
            {
                txn->total_exec_us += exec_cycles;
                chip->nr_waiting_read_xfers++;
                list_add_tail(&txn->waiting_list,
                              &chip->channel->waiting_read_xfer);
            }

            start_data_out_transfer(chip->channel);

            txn_completed = FALSE;
        }
        break;
    default:
        break;
    }

    if (txn_completed) {
        list_for_each_entry_safe(txn, tmp, &die->active_txns, queue)
        {
            txn->total_exec_us += exec_cycles;
            /* In notify_transaction_complete(), the thread that produced
               this txn gets immediately notified and it may release txn
               before we continue to the next txn in the list so
               list_for_each_entry_safe is needed. */
            notify_task_complete(txn, error);
        }
        INIT_LIST_HEAD(&die->active_txns);
        die->active_cmd = NULL;

        if (!chip->active_dies) set_chip_status(chip, CS_IDLE);
    }

    if (get_channel_status(chip->channel) == BUS_IDLE)
        tsu_notify_channel_idle(chip->channel->index);
    if (chip->status == CS_IDLE)
        tsu_notify_chip_idle(chip->channel->index, chip->index);
}

static void complete_data_out_transfer(struct chip_data* chip,
                                       struct die_data* die, u64 timestamp)
{
    struct fil_task* txn = die->active_xfer;
    struct flash_command* cmd = die->active_cmd;
    unsigned int xfer_time;
    uint64_t bitmap;
    int i;

    nfc_complete_transfer(&chip->channel->nfc, NFC_FROM_NAND, txn->length,
                          &bitmap);
    select_volume(chip, FALSE);

    xfer_time = timestamp - chip->last_xfer_start;
    txn->total_xfer_us += xfer_time;

    txn->err_bitmap = bitmap << (txn->offset / chip->channel->nfc.step_size);

    die->active_xfer = NULL;

    for (i = 0; i < cmd->nr_addrs; i++) {
        if (cmd->addrs[i].plane == txn->addr.plane)
            txn->lpa = cmd->metadata[i].lpa;
    }

    list_del(&txn->queue);
    notify_task_complete(txn, FALSE);

    if (list_empty(&die->active_txns)) {
        die->active_cmd = NULL;
    }

    if (!chip->active_dies) {
        if (!--chip->nr_waiting_read_xfers) {
            set_chip_status(chip, CS_IDLE);
        } else {
            set_chip_status(chip, CS_WAIT_FOR_DATA_OUT);
        }
    }

    set_channel_status(chip->channel, BUS_IDLE);
    tsu_notify_channel_idle(chip->channel->index);
}

void fil_dispatch(struct list_head* txn_list)
{
    struct fil_task* head;
    struct fil_task* txn;
    struct channel_data* channel;
    struct chip_data* chip;
    struct die_data* die;
    unsigned int txn_count = 0;

    if (list_empty(txn_list)) return;

    head = list_entry(txn_list->next, struct fil_task, queue);
    channel = &channel_data[head->addr.channel];
    chip = &channel->chips[head->addr.chip];
    die = &chip->dies[head->addr.die];

    Xil_AssertVoid(!die->active_cmd);
    Xil_AssertVoid(get_channel_status(channel) == BUS_IDLE ||
                   chip->current_xfer);

    /* Arm die command. */
    die->active_cmd = &die->cmd_buf;
    /* Copy transaction addresses and metadata. */
    list_for_each_entry(txn, txn_list, queue)
    {
        Xil_AssertVoid(txn_count < MAX_CMD_ADDRS);

        struct flash_address* addr = &die->active_cmd->addrs[txn_count];
        struct page_metadata* metadata =
            &die->active_cmd->metadata[txn_count++];
        *addr = txn->addr;
        metadata->lpa = txn->lpa;
    }
    die->active_cmd->nr_addrs = txn_count;

    list_splice_init(txn_list, &die->active_txns);

    /* Set channel busy for command transfer. */
    set_channel_status(channel, BUS_BUSY);

    switch (head->type) {
    case TXN_READ:
        if (txn_count == 1)
            die->active_cmd->cmd_code = FCMD_READ_PAGE;
        else
            die->active_cmd->cmd_code = FCMD_READ_PAGE_MULTIPLANE;
        break;
    case TXN_WRITE:
        if (txn_count == 1)
            die->active_cmd->cmd_code = FCMD_PROGRAM_PAGE;
        else
            die->active_cmd->cmd_code = FCMD_PROGRAM_PAGE_MULTIPLANE;
        break;
    case TXN_ERASE:
        if (txn_count == 1)
            die->active_cmd->cmd_code = FCMD_ERASE_BLOCK;
        else
            die->active_cmd->cmd_code = FCMD_ERASE_BLOCK_MULTIPLANE;
        break;
    }

    list_add_tail(&die->list, &chip->cmd_xfer_queue);
    start_cmd_data_transfer(chip);
}

static void die_command_get_completion(struct avl_root* root, u64 now,
                                       struct list_head* list)
{
    struct die_data key;
    struct avl_iter iter;
    struct avl_node* node;
    struct list_head channels[NR_CHANNELS];
    unsigned long status_busy = 0;
    int i;

    /* First collect all dies that are expected to have finished their commands
     * (based on predicted finish time) into per-channel lists and then issue
     * READ STATUS commands to multiple channels concurrently. */

    INIT_LIST_HEAD(list);

    key.unique_id = 0;
    key.cmd_finish_time = 0;
    avl_start_iter(root, &iter, &key, AVL_GREATER);
    for (; (node = avl_get_iter(&iter)); avl_inc_iter(&iter)) {
        struct die_data* die = avl_entry(node, struct die_data, avl);
        struct chip_data* chip = die->chip;
        unsigned int channel_idx = chip->channel->index;

        if (die->cmd_finish_time > now) break;

        if (get_channel_status(chip->channel) != BUS_IDLE) continue;

        if (!(status_busy & (1UL << channel_idx))) {
            INIT_LIST_HEAD(&channels[channel_idx]);

            select_volume(chip, TRUE);
            nfc_read_status_async(&chip->channel->nfc, die->index,
                                  die->current_cmd->addrs[0].plane);

            status_busy |= 1UL << channel_idx;
        }

        list_add_tail(&die->completion, &channels[channel_idx]);
    }

    while (status_busy != 0) {
        for (i = 0; i < NR_CHANNELS; i++) {
            struct die_data* die;
            struct chip_data* chip;
            int ready, error = FALSE;

            if (!(status_busy & (1UL << i))) continue;

            die = list_entry(channels[i].next, struct die_data, completion);
            chip = die->chip;

            ready = nfc_check_status(&chip->channel->nfc, &error);
            if (ready == -EAGAIN) continue;

            Xil_AssertVoid(ready >= 0);

            select_volume(chip, FALSE);
            list_del(&die->completion);
            status_busy &= ~(1UL << i);

            if (die->current_cmd->cmd_code == FCMD_READ_PAGE ||
                die->current_cmd->cmd_code == FCMD_READ_PAGE_MULTIPLANE) {
                /* Error bits should be ignore for read commands. */
                error = 0;
            }

            if (ready || error) {
                die->cmd_error = error;
                list_add_tail(&die->completion, list);
            }

            if (!list_empty(&channels[i])) {
                die = list_entry(channels[i].next, struct die_data, completion);
                chip = die->chip;
                select_volume(chip, TRUE);
                nfc_read_status_async(&chip->channel->nfc, die->index,
                                      die->current_cmd->addrs[0].plane);

                status_busy |= 1UL << i;
            }
        }
    }
}

static void check_completion(void)
{
    struct die_data *die, *tmp_die;
    struct chip_data *chip, *tmp_chip;
    struct list_head list;
    u64 now = timer_get_cycles();
    int i;

    list_for_each_entry_safe(chip, tmp_chip, &chip_in_transfer_list, completion)
    {
        if (nfc_transfer_done(&chip->channel->nfc, NFC_TO_NAND)) {
            list_del(&chip->completion);
            nfc_complete_transfer(&chip->channel->nfc, NFC_TO_NAND, 0, NULL);
            complete_chip_transfer(chip, now);
        }
    }

    die_command_get_completion(&die_command_avl, now, &list);
    list_for_each_entry_safe(die, tmp_die, &list, completion)
    {
        avl_erase(&die->avl, &die_command_avl);
        list_del(&die->completion);
        complete_die_command(die->chip, die, die->cmd_error, now);
    }

    list_for_each_entry_safe(die, tmp_die, &chip_out_transfer_list, completion)
    {
        struct chip_data* chip = die->chip;

        if (nfc_transfer_done(&chip->channel->nfc, NFC_FROM_NAND)) {
            list_del(&die->completion);
            complete_data_out_transfer(die->chip, die, now);
        }
    }

    for (i = 0; i < NR_CHANNELS; i++) {
        struct channel_data* channel = &channel_data[i];

        start_data_out_transfer(channel);
        if (get_channel_status(channel) == BUS_IDLE) tsu_notify_channel_idle(i);
    }
}

void fil_tick(void) { check_completion(); }

void fil_sample_flash(struct prof_sample_flash* sample)
{
    int chan, chip, die;
    int die_index = 0;

    sample->channel_status = 0;
    sample->die_status = 0;

    for (chan = 0; chan < NR_CHANNELS; chan++) {
        if (fil_is_channel_busy(chan)) sample->channel_status |= 1 << chan;

        for (chip = 0; chip < CHIPS_PER_CHANNEL; chip++) {
            for (die = 0; die < DIES_PER_CHIP; die++) {
                if (channel_data[chan].chips[chip].dies[die].current_cmd)
                    sample->die_status |= (u64)1 << die_index;
                die_index++;
            }
        }
    }
}

void fil_report_stats(void)
{
    int chan, chip, die;

    for (chan = 0; chan < NR_CHANNELS; chan++) {
        struct channel_data* chan_data = &channel_data[chan];

        xil_printf("-Channel %d\n", chan);

        for (chip = 0; chip < CHIPS_PER_CHANNEL; chip++) {
            struct chip_data* chip_data = &chan_data->chips[chip];

            xil_printf("  -Chip %d\n", chip);
            xil_printf("    Status: %d\n", chip_data->status);
            xil_printf("    Active die(s): %d\n", chip_data->active_dies);

            if (chip_data->current_xfer) {
                xil_printf("    Current transfer: die %d\n",
                           chip_data->current_xfer->index);
            }

            for (die = 0; die < DIES_PER_CHIP; die++) {
                struct die_data* die_data = &chip_data->dies[die];

                xil_printf("    -Die %d\n", die);

                if (die_data->current_cmd) {
                    struct flash_address* addr =
                        &die_data->current_cmd->addrs[0];
                    xil_printf(
                        "      Current command: (t%d ch%d w%d d%d pl%d b%d p%d)\n",
                        die_data->current_cmd->cmd_code, addr->channel,
                        addr->chip, addr->die, addr->plane, addr->block,
                        addr->page);
                }

                if (die_data->active_cmd) {
                    struct flash_address* addr =
                        &die_data->active_cmd->addrs[0];

                    xil_printf(
                        "      Active command: (t%d ch%d w%d d%d pl%d b%d p%d)\n",
                        die_data->active_cmd->cmd_code, addr->channel,
                        addr->chip, addr->die, addr->plane, addr->block,
                        addr->page);
                }

                if (die_data->active_xfer) {
                    struct fil_task* txn = die_data->active_xfer;

                    xil_printf(
                        "      Active transfer: (t%d s%d ch%d w%d d%d pl%d b%d p%d)\n",
                        txn->type, txn->source, txn->addr.channel,
                        txn->addr.chip, txn->addr.die, txn->addr.plane,
                        txn->addr.block, txn->addr.page);
                }
            }
        }
    }
}
