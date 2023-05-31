#include <xil_assert.h>
#include <xil_printf.h>

#include <types.h>
#include <const.h>
#include <page.h>
#include <profile.h>

static struct prof_sample_buf* prof_sample_buf;

void profile_init(void)
{
    prof_sample_buf =
        ioremap_nc((phys_addr_t)PROFILE_BUF_START, PROFILE_BUF_SIZE);
}

void profile_dump(int argc, const char** argv)
{
    unsigned int mem_used = prof_sample_buf->mem_used;
    int i;

    Xil_AssertVoid(mem_used <= PROFILE_BUF_SIZE);

    xil_printf("========== BEGIN PROFILE SAMPLES ==========\n");
    xil_printf("Mem used: %u\n", mem_used);

    mem_used = (mem_used + 3) & ~0x3;
    for (i = 0; i < mem_used; i += 4) {
        xil_printf("%d: %08x\n", i,
                   *(unsigned int*)&prof_sample_buf->sample_buf[i]);
    }

    xil_printf("========== END PROFILE SAMPLES ==========\n");
}
