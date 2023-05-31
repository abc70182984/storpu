#include <string.h>

#include <types.h>
#include <const.h>
#include <cpulocals.h>
#include <page.h>
#include <memalloc.h>
#include <utils.h>

unsigned long __cpulocals_offset[NR_CPUS];

/* We use the core with hardware ID 0 (FTL core) to boot and initialize the
 * entire system. However, we want to assign the highest cpuid (NR_CPUS - 1) to
 * it so that the other SMP cores can use contiguous cpuid starting from zero.
 */
DEFINE_CPULOCAL(unsigned int, cpu_number) = FTL_CPU_ID;

#define BOOT_CPULOCALS_OFFSET 0
unsigned long __cpulocals_offset[NR_CPUS] = {
    [0 ... NR_CPUS - 1] = BOOT_CPULOCALS_OFFSET,
};

void cpulocals_init(void)
{
    size_t size;
    char* ptr;
    int cpu;
    extern char __cpulocals_start[], __cpulocals_end[];

    size = roundup(__cpulocals_end - __cpulocals_start, ARCH_PG_SIZE);
    ptr = alloc_vmpages((size >> ARCH_PG_SHIFT) * NR_CPUS, ZONE_PS_DDR);
    if (!ptr) panic("failed to allocate cpulocals storage");

    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        cpulocals_offset(cpu) = ptr - (char*)__cpulocals_start;
        memcpy(ptr, (void*)__cpulocals_start,
               __cpulocals_end - __cpulocals_start);

        get_cpu_var(cpu, cpu_number) = cpu;

        ptr += size;
    }

    set_my_cpu_offset(cpulocals_offset(FTL_CPU_ID));
}
