#include <xil_types.h>
#include <time.h>
#include "timer.h"

u64 timer_get_cycles(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

u64 timer_cycles_to_ns(u64 cycles) { return cycles; }
