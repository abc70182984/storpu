#include <xil_cache.h>

#include <types.h>
#include <const.h>
#include <page.h>
#include <barrier.h>
#include <cpulocals.h>
#include <cpumask.h>
#include <intr.h>
#include <cpulocals.h>
#include <utils.h>
#include <smp.h>
#include <cpu_type.h>

#include "proto.h"

struct {
    void* stack_pointer;
    unsigned long cpu_offset;
} secondary_data;

static unsigned int cpu_nr;
static unsigned int bsp_cpu_id;
extern phys_addr_t cpu_release_addr[];

static int __cpu_ready;

DEFINE_CPULOCAL(unsigned long, cpu_phys_id) = 0;

extern char k_stacks_start, k_stacks_end;
#define get_k_stack_top(cpu) \
    ((void*)(&k_stacks_start + 2 * ((cpu) + 1) * K_STACK_SIZE))

extern void secondary_entry();

extern void smp_bsp_main(void);
extern void smp_ap_main(void);

extern void finish_bsp_booting(void);

unsigned int smp_start_cpu(unsigned int hwid)
{
    unsigned int cpu = cpu_nr++;
    phys_addr_t pa_entry = __pa(secondary_entry);
    void* release_addr = &cpu_release_addr[hwid];

    __cpu_ready = -1;

    secondary_data.stack_pointer = get_k_stack_top(cpu);
    secondary_data.cpu_offset = cpulocals_offset(cpu);

    *(volatile phys_addr_t*)release_addr = pa_entry;
    Xil_DCacheFlushRange((unsigned long)release_addr, sizeof(phys_addr_t));
    sev();

    while (__atomic_load_n(&__cpu_ready, __ATOMIC_RELAXED) != cpu)
        isb();
    smp_rmb();

    return cpu;
}

void smp_init(void)
{
    bsp_cpu_id = 0;
    set_my_cpu_offset(cpulocals_offset(bsp_cpu_id));

    set_cpu_online(bsp_cpu_id, TRUE);

    /* smp_start_cpu(1); */

    finish_bsp_booting();
}

void smp_boot_ap(void)
{
    u64 mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;

    get_cpulocal_var(cpu_phys_id) = mpidr;

    set_cpu_online(cpuid, TRUE);
    __atomic_store_n(&__cpu_ready, cpuid, __ATOMIC_RELEASE);

    printk("CPU %d (physical %ld) is up\n", cpuid,
           get_cpulocal_var(cpu_phys_id));

    if (cpuid == bsp_cpu_id)
        smp_bsp_main();
    else
        smp_ap_main();
}
