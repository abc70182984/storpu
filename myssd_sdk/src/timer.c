#include "xparameters.h"
#include "xtmrctr.h"
#include "xil_exception.h"

#include <config.h>
#include <const.h>
#include <utils.h>
#include <intr.h>
#include "proto.h"
#include <barrier.h>
#include <sysreg.h>
#include <cpulocals.h>
#include <timer.h>

#define IOU_SCNTRS_BASEFREQ 0xFF260020UL

#define SYS_COUNTER_MASK ((1UL << 56) - 1)

static u32 arch_timer_rate;
static u32 freq_mult, freq_shift;

#define ARCH_TIMER_VIRT_PPI 27U

#define ARCH_TIMER_CTRL_ENABLE  (1 << 0)
#define ARCH_TIMER_CTRL_IT_MASK (1 << 1)
#define ARCH_TIMER_CTRL_IT_STAT (1 << 2)

static DEFINE_CPULOCAL(u32, tick_delta);

static DEFINE_CPULOCAL(tick_handler_t, tick_handler);

u64 timer_get_cycles(void)
{
    isb();
    return read_sysreg(cntvct_el0);
}

u64 timer_cycles_to_ns(u64 cycles)
{
    return ((u64)cycles * freq_mult) >> freq_shift;
}

static void timer_set_next_event(unsigned long event)
{
    unsigned long ctrl;

    ctrl = read_sysreg(cntv_ctl_el0);
    ctrl |= ARCH_TIMER_CTRL_ENABLE;
    ctrl &= ~ARCH_TIMER_CTRL_IT_MASK;
    write_sysreg(event, cntv_tval_el0);
    write_sysreg(ctrl, cntv_ctl_el0);
}

void set_tick_handler(tick_handler_t handler)
{
    get_cpulocal_var(tick_handler) = handler;
}

static void timer_handler(void* callback)
{
    unsigned long ctrl;
    tick_handler_t handler = get_cpulocal_var(tick_handler);

    ctrl = read_sysreg(cntv_ctl_el0);
    if (ctrl & ARCH_TIMER_CTRL_IT_STAT) {
        ctrl |= ARCH_TIMER_CTRL_IT_MASK;
        write_sysreg(ctrl, cntv_ctl_el0);

        if (handler) (handler)();

        timer_set_next_event(get_cpulocal_var(tick_delta));
    }
}

static void clocks_calc_mult_shift(u32* mult, u32* shift, u32 from, u32 to,
                                   u32 maxsec)
{
    u64 tmp;
    u32 sft, sftacc = 32;

    tmp = ((u64)maxsec * from) >> 32;
    while (tmp) {
        tmp >>= 1;
        sftacc--;
    }

    for (sft = 32; sft > 0; sft--) {
        tmp = (u64)to << sft;
        tmp += from / 2;
        tmp /= from;
        if ((tmp >> sftacc) == 0) break;
    }
    *mult = tmp;
    *shift = sft;
}

static void update_freq_scale(u32 scale, u32 freq)
{
    u64 sec;
    u32 maxadj;

    if (freq) {
        sec = SYS_COUNTER_MASK;
        sec /= freq;
        sec /= scale;

        if (!sec)
            sec = 1;
        else if (sec > 600)
            sec = 600;

        clocks_calc_mult_shift(&freq_mult, &freq_shift, freq,
                               NSEC_PER_SEC / scale, sec * scale);
    }

    maxadj = (u32)((freq_mult * 11) / 100);
    while (freq && ((freq_mult + maxadj < freq_mult) ||
                    (freq_mult - maxadj > freq_mult))) {
        freq_mult >>= 1;
        freq_shift--;
        maxadj = (u32)((freq_mult * 11) / 100);
    }
}

int timer_setup(void)
{
    int status;
    u32 tick_val;

    if (!arch_timer_rate) {
        arch_timer_rate = *(volatile u32*)IOU_SCNTRS_BASEFREQ;

        update_freq_scale(1, arch_timer_rate);
    }

    status =
        intr_setup_irq(ARCH_TIMER_VIRT_PPI, 0x1, (irq_handler_t)timer_handler,
                       (void*)(unsigned long)cpuid);
    if (status != XST_SUCCESS) {
        xil_printf("Failed to setup arch timer IRQ\r\n");
        return XST_FAILURE;
    }

    intr_enable_irq(ARCH_TIMER_VIRT_PPI);

    tick_val = arch_timer_rate / SYSTEM_HZ;
    if (tick_val < 1) tick_val = 1;

    get_cpulocal_var(tick_delta) = tick_val;

    timer_set_next_event(get_cpulocal_var(tick_delta));

    return XST_SUCCESS;
}
