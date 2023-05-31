#ifndef _STORPU_SCHED_H_
#define _STORPU_SCHED_H_

typedef unsigned long cpu_set_t;

#define CPU_ZERO(set) __cpu_zero(set)

#define CPU_SET(cpu, set)   __cpu_set((cpu), (set))
#define CPU_CLR(cpu, set)   __cpu_clear((cpu), (set))
#define CPU_ISSET(cpu, set) __cpu_isset((cpu), (set))

#ifdef __cplusplus
extern "C"
{
#endif

    static inline void __cpu_zero(cpu_set_t* set) { *set = 0UL; }

    static inline void __cpu_set(int cpu, cpu_set_t* set)
    {
        *set |= 1UL << cpu;
    }

    static inline void __cpu_clear(int cpu, cpu_set_t* set)
    {
        *set &= ~(1UL << cpu);
    }

    static inline int __cpu_isset(int cpu, cpu_set_t* set)
    {
        return !!(*set & (1UL << cpu));
    }

    int spu_sched_setaffinity(spu_thread_t thread, size_t cpusetsize,
                              const cpu_set_t* mask);

#ifdef __cplusplus
}
#endif

#endif
