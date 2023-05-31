#include <xparameters.h>
#include <xscugic.h>
#include <xil_cache.h>
#include <errno.h>

#include <const.h>
#include <intr.h>
#include <profile.h>

/* BSP uses cycle counter for usleep(). Don't make it confused. */
#define WATCHDOG_COUNTER_MASK (1 << 0)

#define PMU_INT_ID 40U

#define WATCHDOG_FREQ (XPAR_CPU_CORTEXR5_0_CPU_CLK_FREQ_HZ)

void* prof_pc;

static unsigned int reset_value;
static unsigned int watchdog_reset_value;
static unsigned int profile_reset_value;

static int profiling;

static struct prof_sample_buf* prof_sample_buf =
    (struct prof_sample_buf*)PROFILE_BUF_START;

void fil_sample_flash(struct prof_sample_flash* sample);

static void profile_record_pc(void* pc)
{
    if (prof_sample_buf->mem_used + 1 + 4 > PROFILE_BUF_SIZE) return;

    prof_sample_buf->sample_buf[prof_sample_buf->mem_used] = PROF_TYPE_PC;
    *(u32*)&prof_sample_buf->sample_buf[prof_sample_buf->mem_used + 1] =
        (u32)pc;

    prof_sample_buf->mem_used += 1 + 4;
}

static void profile_record_flash(void)
{
    struct prof_sample_flash s;
    u64 die_status;

    if (prof_sample_buf->mem_used + 1 + 9 > PROFILE_BUF_SIZE) return;

    fil_sample_flash(&s);
    die_status = s.die_status;

    prof_sample_buf->sample_buf[prof_sample_buf->mem_used] = PROF_TYPE_FLASH;
    prof_sample_buf->sample_buf[prof_sample_buf->mem_used + 1] =
        s.channel_status;
    *(u32*)&prof_sample_buf->sample_buf[prof_sample_buf->mem_used + 2] =
        (u32)die_status;
    *(u32*)&prof_sample_buf->sample_buf[prof_sample_buf->mem_used + 6] =
        (u32)(die_status >> 32);

    prof_sample_buf->mem_used += 1 + 9;
}

static void profile_sample(void* pc)
{
    if (!profiling) return;

    profile_record_pc(pc);
    profile_record_flash();
}

static void profile_handler(void) { profile_sample(prof_pc); }

static void watchdog_reset(void)
{
    asm volatile("mcr    p15, 0, %2, c9, c12, 5\n" /* Select watchdog counter */
                 "mcr    p15, 0, %1, c9, c13, 2\n" /* Reset counter */
                 "mcr    p15, 0, %0, c9, c12, 1\n" /* Enable watchdog counter */
                 :
                 : "r"(WATCHDOG_COUNTER_MASK), "r"(reset_value), "r"(0)
                 : "cc");
}

static void watchdog_interrupt_handler(void* callback)
{
    asm volatile(
        "mcr    p15, 0, %0, c9, c12, 2\n" /* Disable watchdog counter */
        "mcr    p15, 0, %0, c9, c12, 3\n" /* Clear overflow */
        :
        : "r"(WATCHDOG_COUNTER_MASK)
        : "cc");

    if (profiling) {
        profile_handler();

        watchdog_reset();
    }
}

int watchdog_init(void)
{
    int status;

    asm volatile(
        "mcr    p15, 0, %0, c9, c12, 2\n" /* Disable watchdog counter */
        "mcr    p15, 0, %0, c9, c12, 3\n" /* Clear overflow */
        "mcr    p15, 0, %2, c9, c12, 5\n" /* Select watchdog counter */
        "mcr    p15, 0, %1, c9, c13, 1\n" /* Set watchdog counter type, 0x11 =
                                             cycle count */
        "mcr    p15, 0, %0, c9, c14, 1\n" /* Enable watchdog interrupt */
        :
        : "r"(WATCHDOG_COUNTER_MASK), "r"(0x11), "r"(0)
        : "cc");

    status = intr_setup_irq(PMU_INT_ID, 0x1,
                            (irq_handler_t)watchdog_interrupt_handler, NULL);
    if (status != XST_SUCCESS) return status;

    intr_enable_irq(PMU_INT_ID);

    reset_value = watchdog_reset_value = -WATCHDOG_FREQ;

    return XST_SUCCESS;
}

int profile_start(int freq)
{
    if (profiling) return EBUSY;

    profile_reset_value = -(WATCHDOG_FREQ / freq);
    reset_value = profile_reset_value;

    prof_sample_buf->mem_used = 0;

    watchdog_reset();

    profiling = TRUE;

    return 0;
}

static void flush_samples(void)
{
    unsigned int mem_used = prof_sample_buf->mem_used + 4;

    mem_used = (mem_used + 3) & ~0x3;
    Xil_DCacheFlushRange((unsigned long)prof_sample_buf, mem_used);
}

int profile_stop(void)
{
    if (!profiling) return EBUSY;

    flush_samples();

    reset_value = watchdog_reset_value;

    profiling = FALSE;

    return 0;
}
