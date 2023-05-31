#ifndef _CPULOCALS_H_
#define _CPULOCALS_H_

/* This is much like the TLS variables defined in tls.h. The difference is that
 * TLS is used by the FTL internally for its own thread scheduling but this file
 * is for local variables for physical CPU cores. */

#include <config.h>

#define CPULOCAL_BASE_SECTION ".cpulocals"

extern unsigned long __cpulocals_offset[NR_CPUS];
#define cpulocals_offset(cpu) __cpulocals_offset[cpu]

#define __get_cpu_var_ptr_offset(offset, name) \
    ({                                         \
        unsigned long __ptr;                   \
        __ptr = (unsigned long)(&(name));      \
        (typeof(name)*)(__ptr + offset);       \
    })

#define get_cpu_var_ptr(cpu, name) \
    __get_cpu_var_ptr_offset(cpulocals_offset(cpu), name)
#define get_cpu_var(cpu, name) (*get_cpu_var_ptr(cpu, name))
#define get_cpulocal_var_ptr(name) \
    __get_cpu_var_ptr_offset(__my_cpu_offset, name)
#define get_cpulocal_var(name) (*get_cpulocal_var_ptr(name))

#define DECLARE_CPULOCAL(type, name) \
    extern __attribute__((section(CPULOCAL_BASE_SECTION))) __typeof__(type) name

#define DEFINE_CPULOCAL(type, name) \
    __attribute__((section(CPULOCAL_BASE_SECTION))) __typeof__(type) name

static inline void set_my_cpu_offset(unsigned long off)
{
    asm volatile("msr tpidr_el1, %0" ::"r"(off) : "memory");
}

static inline unsigned long ___my_cpu_offset(void)
{
    unsigned long off;
    register unsigned long current_sp asm("sp");

    /*
     * We want to allow caching the value, so avoid using volatile and
     * instead use a fake stack read to hazard against barrier().
     */
    asm("mrs %0, tpidr_el1"
        : "=r"(off)
        : "Q"(*(const unsigned long*)current_sp));

    return off;
}

#define __my_cpu_offset ___my_cpu_offset()

DECLARE_CPULOCAL(unsigned int, cpu_number);

#define cpuid (get_cpulocal_var(cpu_number))

#endif
