#ifndef _TIMER_H_
#define _TIMER_H_

#define MSEC_PER_SEC 1000UL
#define USEC_PER_SEC (MSEC_PER_SEC * 1000UL)
#define NSEC_PER_SEC (USEC_PER_SEC * 1000UL)

typedef void (*tick_handler_t)(void);

int timer_setup(void);
u64 timer_get_cycles(void);
u64 timer_cycles_to_ns(u64 cycles);

void set_tick_handler(tick_handler_t handler);

#endif
