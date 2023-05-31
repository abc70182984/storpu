/* Transaction Scheduling Unit */

#include "xil_types.h"
#include "xil_assert.h"

#include <flash_config.h>
#include <flash.h>
#include "fil.h"

struct txn_queues {
    struct list_head read_queue;
    struct list_head write_queue;
    struct list_head mapping_read_queue;
    struct list_head mapping_write_queue;
    struct list_head gc_read_queue;
    struct list_head gc_write_queue;
    struct list_head gc_erase_queue;
};

struct queue_stats {
    unsigned int enqueued_requests;
};

static struct txn_queues chip_queues[NR_CHANNELS][CHIPS_ENABLED_PER_CHANNEL];
static int request_count[NR_CHANNELS][CHIPS_ENABLED_PER_CHANNEL];

static struct queue_stats queue_stats[NR_CHANNELS][CHIPS_ENABLED_PER_CHANNEL];

/* Which chip should be processed next following round-robin order. */
static unsigned int channel_rr_index[NR_CHANNELS];

void tsu_process_task(struct fil_task* txn)
{
    struct txn_queues* chip = &chip_queues[txn->addr.channel][txn->addr.chip];
    struct list_head* queue = NULL;

    if (txn->addr.channel >= NR_CHANNELS ||
        txn->addr.chip >= CHIPS_ENABLED_PER_CHANNEL) {
        /* Out-of-range address. */
        notify_task_complete(txn, TRUE);
        return;
    }

    switch (txn->type) {
    case TXN_READ:
        switch (txn->source) {
        case TS_USER_IO:
            queue = &chip->read_queue;
            break;
        case TS_MAPPING:
            queue = &chip->mapping_read_queue;
            break;
        case TS_GC:
            queue = &chip->gc_read_queue;
            break;
        }
        break;
    case TXN_WRITE:
        switch (txn->source) {
        case TS_USER_IO:
            queue = &chip->write_queue;
            break;
        case TS_MAPPING:
            queue = &chip->mapping_write_queue;
            break;
        case TS_GC:
            queue = &chip->gc_write_queue;
            break;
        }
        break;
    case TXN_ERASE:
        queue = &chip->gc_erase_queue;
        break;
    }

    Xil_AssertVoid(queue);

    list_add_tail(&txn->queue, queue);
    request_count[txn->addr.channel][txn->addr.chip]++;
    queue_stats[txn->addr.channel][txn->addr.chip].enqueued_requests++;
}

void tsu_init(void)
{
    int i, j;

    for (i = 0; i < NR_CHANNELS; i++) {
        for (j = 0; j < CHIPS_ENABLED_PER_CHANNEL; j++) {
            struct txn_queues* qs = &chip_queues[i][j];
            INIT_LIST_HEAD(&qs->read_queue);
            INIT_LIST_HEAD(&qs->write_queue);
            INIT_LIST_HEAD(&qs->mapping_read_queue);
            INIT_LIST_HEAD(&qs->mapping_write_queue);
            INIT_LIST_HEAD(&qs->gc_read_queue);
            INIT_LIST_HEAD(&qs->gc_write_queue);
            INIT_LIST_HEAD(&qs->gc_erase_queue);
        }
    }
}

static inline int task_ready(struct fil_task* txn) { return TRUE; }

static int dispatch_queue_request(struct list_head* q_prim,
                                  struct list_head* q_sec, enum txn_type type)
{
    struct fil_task* head[DIES_PER_CHIP] = {
        [0 ... DIES_PER_CHIP - 1] = NULL,
    };
    int die;
    int max_batch_nr = 1;
    struct fil_task *txn, *tmp;
    uint64_t die_bitmap = 0;
    int found = 0;

#ifdef ENABLE_MULTIPLANE
    max_batch_nr = PLANES_PER_DIE;
#endif

    /* Commands can be concurrently submitted to LUNs on the same chip subject
     * subject to the multi-LUN operation rules (see fil_is_die_busy@fil.c).
     * Here, we first select one candidate command for each die, then try to
     * submit them until the channel or the target LUN is busy. */
    list_for_each_entry(txn, q_prim, queue)
    {
        if (die_bitmap & (1UL << txn->addr.die)) continue;

        if (!fil_is_die_busy(txn->addr.channel, txn->addr.chip, txn->addr.die,
                             txn->type == TXN_WRITE)) {
            head[txn->addr.die] = txn;

            /* Admitting a write transaction immediately makes the channel busy
             * so no more transactions can be admitted. */
            if (txn->type == TXN_WRITE) break;
        }

        die_bitmap |= 1UL << txn->addr.die;

        if (die_bitmap == (1UL << DIES_PER_CHIP) - 1) break;
    }

    for (die = 0; die < DIES_PER_CHIP; die++) {
        /* For each LUN, we either submit the candidate command or pack other
         * commands targetting different planes of the same page into a
         * multi-plane command. */
        int found_die = 0;
        uint64_t plane_bitmap = 0;
        struct list_head dispatch_list;
        unsigned int page;

        if (!head[die]) continue;

        /* Die is idle at first but becomes busy after previous commands in the
         * same chip are submitted. */
        if (fil_is_die_busy(head[die]->addr.channel, head[die]->addr.chip, die,
                            head[die]->type == TXN_WRITE))
            continue;

        INIT_LIST_HEAD(&dispatch_list);
        page = head[die]->addr.page;

        list_for_each_entry_safe(txn, tmp, q_prim, queue)
        {
            /* Submit the command if:
             * 1) The command is ready.
             * 2) It targets the selected LUN.
             * 3) The plane is not occupied in case of a multi-plane command.
             * 4) It targets the same page as the candidate command (in order to
             *    form a multi-plane command). */
            if (task_ready(txn) && txn->addr.die == die &&
                !(plane_bitmap & (1 << txn->addr.plane)) &&
                (!plane_bitmap || txn->addr.page == page)) {
                found_die++;
                plane_bitmap |= 1 << txn->addr.plane;
                list_del(&txn->queue);
                list_add_tail(&txn->queue, &dispatch_list);
            }

            if (found_die >= max_batch_nr) break;
        }

        if (q_sec && found_die < max_batch_nr) {
            list_for_each_entry_safe(txn, tmp, q_sec, queue)
            {
                if (task_ready(txn) && txn->addr.die == die &&
                    !(plane_bitmap & (1 << txn->addr.plane)) &&
                    (!plane_bitmap || txn->addr.page == page)) {
                    plane_bitmap |= 1 << txn->addr.plane;
                    found_die++;
                    list_del(&txn->queue);
                    list_add_tail(&txn->queue, &dispatch_list);
                }

                if (found_die >= max_batch_nr) break;
            }
        }

        if (!list_empty(&dispatch_list)) {
            fil_dispatch(&dispatch_list);
        }

        found += found_die;

        if (fil_is_channel_busy(head[die]->addr.channel)) break;
    }

    return found;
}

static int dispatch_read_request(unsigned int channel, unsigned int chip)
{
    struct list_head *q_prim = NULL, *q_sec = NULL;
    struct txn_queues* queues = &chip_queues[channel][chip];
    int found;

    if (!list_empty(&queues->mapping_read_queue)) {
        /* Prioritize read txns for mapping entries. */
        q_prim = &queues->mapping_read_queue;

        if (!list_empty(&queues->read_queue))
            q_sec = &queues->read_queue;
        else if (!list_empty(&queues->gc_read_queue))
            q_sec = &queues->gc_read_queue;
    } else {
        if (!list_empty(&queues->read_queue)) {
            q_prim = &queues->read_queue;
            if (!list_empty(&queues->gc_read_queue)) {
                q_sec = &queues->gc_read_queue;
            }
        } else if (!list_empty(&queues->write_queue))
            return FALSE;
        else if (!list_empty(&queues->gc_read_queue))
            q_prim = &queues->gc_read_queue;
        else
            return FALSE;
    }

    found = dispatch_queue_request(q_prim, q_sec, TXN_READ);
    request_count[channel][chip] -= found;

    return found > 0;
}

static int dispatch_write_request(unsigned int channel, unsigned int chip)
{
    struct list_head *q_prim = NULL, *q_sec = NULL;
    struct txn_queues* queues = &chip_queues[channel][chip];
    int found;

    if (!list_empty(&queues->mapping_write_queue)) {
        /* Prioritize write txns for mapping entries. */
        q_prim = &queues->mapping_write_queue;

        if (!list_empty(&queues->write_queue))
            q_sec = &queues->write_queue;
        else if (!list_empty(&queues->gc_write_queue))
            q_sec = &queues->gc_write_queue;
    } else {
        if (!list_empty(&queues->write_queue)) {
            q_prim = &queues->write_queue;
            if (!list_empty(&queues->gc_write_queue)) {
                q_sec = &queues->gc_write_queue;
            }
        } else if (!list_empty(&queues->gc_write_queue))
            q_prim = &queues->gc_write_queue;
        else
            return FALSE;
    }

    found = dispatch_queue_request(q_prim, q_sec, TXN_WRITE);
    request_count[channel][chip] -= found;

    return found > 0;
}

static int dispatch_erase_request(unsigned int channel, unsigned int chip)
{
    struct txn_queues* queues = &chip_queues[channel][chip];
    struct list_head* q_prim = &queues->gc_erase_queue;
    int found;

    if (list_empty(q_prim)) return FALSE;

    found = dispatch_queue_request(q_prim, NULL, TXN_ERASE);
    request_count[channel][chip] -= found;

    return found > 0;
}

static void dispatch_request(unsigned int channel, unsigned int chip)
{
    if (dispatch_read_request(channel, chip)) return;
    if (dispatch_write_request(channel, chip)) return;
    dispatch_erase_request(channel, chip);
}

static void tsu_flush_channel(unsigned int channel)
{
    int i;

    for (i = 0; i < CHIPS_ENABLED_PER_CHANNEL; i++) {
        unsigned int chip_id = channel_rr_index[channel];

        if (request_count[channel][chip_id]) dispatch_request(channel, chip_id);

        channel_rr_index[channel] =
            (channel_rr_index[channel] + 1) % CHIPS_ENABLED_PER_CHANNEL;

        if (fil_is_channel_busy(channel)) break;
    }
}

void tsu_flush_queues(void)
{
    int i;

    for (i = 0; i < NR_CHANNELS; i++) {
        if (fil_is_channel_busy(i)) continue;

        tsu_flush_channel(i);
    }
}

void tsu_notify_channel_idle(unsigned int channel)
{
    tsu_flush_channel(channel);
}

void tsu_notify_chip_idle(unsigned int channel, unsigned int chip)
{
    if (fil_is_channel_busy(channel)) return;
    dispatch_request(channel, chip);
}

static void print_txn_queues(const char* name, struct list_head* queue_head)
{
    struct fil_task* txn;

    if (list_empty(queue_head)) return;

    xil_printf("%s: ", name);

    list_for_each_entry(txn, queue_head, queue)
    {
        xil_printf("(t%d s%d ch%d w%d d%d pl%d b%d p%d) ", txn->type,
                   txn->source, txn->addr.channel, txn->addr.chip,
                   txn->addr.die, txn->addr.plane, txn->addr.block,
                   txn->addr.page);
    }
    xil_printf("\n");
}

void tsu_report_stats(void)
{
    int channel, chip;

    for (channel = 0; channel < NR_CHANNELS; channel++) {
        xil_printf("Channel %d\n", channel);

        for (chip = 0; chip < CHIPS_ENABLED_PER_CHANNEL; chip++) {
            struct txn_queues* queues = &chip_queues[channel][chip];

            xil_printf("  Chip %d\n", chip);
            xil_printf("    Enqueued request: %u\n",
                       queue_stats[channel][chip].enqueued_requests);

            print_txn_queues("    [RD]", &queues->read_queue);
            print_txn_queues("    [WR]", &queues->write_queue);
            print_txn_queues("    [MR]", &queues->mapping_read_queue);
            print_txn_queues("    [MW]", &queues->mapping_write_queue);
            print_txn_queues("    [GR]", &queues->gc_read_queue);
            print_txn_queues("    [GW]", &queues->gc_write_queue);
            print_txn_queues("    [GE]", &queues->gc_erase_queue);
        }

        xil_printf("\n");
    }
}
