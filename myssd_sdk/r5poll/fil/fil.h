#ifndef _FIL_FIL_H_
#define _FIL_FIL_H_

#include <list.h>
#include <flash_config.h>
#include <flash.h>
#include <fil.h>

enum channel_status {
    BUS_IDLE = 0,
    BUS_BUSY,
};

/* fil.c */
void fil_init(void);
/* int fil_is_channel_busy(unsigned int channel); */
int fil_is_die_busy(unsigned int channel, unsigned int chip, unsigned int die,
                    int is_program);
void fil_dispatch(struct list_head* txn_list);
void fil_tick(void);
void fil_report_stats(void);

static inline int fil_is_channel_busy(unsigned int channel)
{
    extern enum channel_status channel_status[];
    Xil_AssertNonvoid(channel < NR_CHANNELS);
    return channel_status[channel] != BUS_IDLE;
}

/* tsu.c */
void tsu_init(void);
void tsu_process_task(struct fil_task* task);
void tsu_notify_channel_idle(unsigned int channel);
void tsu_notify_chip_idle(unsigned int channel, unsigned int chip);
void tsu_flush_queues(void);
void tsu_report_stats(void);

#endif
