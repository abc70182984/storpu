#ifndef _CPUMASK_H_
#define _CPUMASK_H_

#include <config.h>
#include <bitmap.h>

#define NR_CPUMASK_BITS NR_CPUS

typedef struct cpumask {
    bitchunk_t bits[BITCHUNKS(NR_CPUS)];
} cpumask_t;

#define cpumask_bits(maskp) ((maskp)->bits)

#define to_cpumask(bitmap) ((struct cpumask*)(bitmap))

extern struct cpumask __cpu_possible_mask;
extern struct cpumask __cpu_online_mask;

#define cpu_possible_mask ((const struct cpumask*)&__cpu_possible_mask)
#define cpu_online_mask   ((const struct cpumask*)&__cpu_online_mask)

static inline void cpumask_clear(struct cpumask* cpumask)
{
    bitmap_zero(cpumask_bits(cpumask), NR_CPUMASK_BITS);
}

static inline int cpumask_test_cpu(const struct cpumask* cpumask, int cpu)
{
    return !!GET_BIT(cpumask_bits(cpumask), cpu);
}

static inline void cpumask_set_cpu(struct cpumask* cpumask, int cpu)
{
    SET_BIT(cpumask_bits(cpumask), cpu);
}

static inline void cpumask_clear_cpu(struct cpumask* cpumask, int cpu)
{
    UNSET_BIT(cpumask_bits(cpumask), cpu);
}

static inline void cpumask_copy(struct cpumask* dstp,
                                const struct cpumask* srcp)
{
    bitmap_copy(cpumask_bits(dstp), cpumask_bits(srcp), NR_CPUMASK_BITS);
}

static inline int cpumask_equal(const struct cpumask* mask1,
                                const struct cpumask* mask2)
{
    return bitmap_equal(cpumask_bits(mask1), cpumask_bits(mask2),
                        NR_CPUMASK_BITS);
}

static inline int cpumask_and(struct cpumask* dstp, const struct cpumask* src1p,
                              const struct cpumask* src2p)
{
    return bitmap_and(cpumask_bits(dstp), cpumask_bits(src1p),
                      cpumask_bits(src2p), NR_CPUMASK_BITS);
}

static inline void cpumask_or(struct cpumask* dstp, const struct cpumask* src1p,
                              const struct cpumask* src2p)
{
    bitmap_or(cpumask_bits(dstp), cpumask_bits(src1p), cpumask_bits(src2p),
              NR_CPUMASK_BITS);
}

static inline unsigned int cpumask_size(void)
{
    return BITCHUNKS(NR_CPUMASK_BITS) * sizeof(bitchunk_t);
}

extern const bitchunk_t cpu_bit_bitmap[BITCHUNK_BITS + 1][BITCHUNKS(NR_CPUS)];

static inline const struct cpumask* get_cpu_mask(unsigned int cpu)
{
    const bitchunk_t* p = cpu_bit_bitmap[1 + cpu % BITCHUNK_BITS];
    p -= cpu / BITCHUNK_BITS;
    return to_cpumask(p);
}

#define cpumask_of(cpu) (get_cpu_mask(cpu))

#if NR_CPUS <= 64
#define CPU_BITS_ALL                                              \
    {                                                             \
        [BITCHUNKS(NR_CPUS) - 1] = BITMAP_LAST_WORD_MASK(NR_CPUS) \
    }

#else /* NR_CPUS > BITCHUNK_BITS */

#define CPU_BITS_ALL                                                   \
    {                                                                  \
        [0 ... BITCHUNKS(NR_CPUS) - 2] = ~0UL,                         \
                                    [BITCHUNKS(NR_CPUS) - 1] =         \
                                        BITMAP_LAST_WORD_MASK(NR_CPUS) \
    }
#endif /* NR_CPUS > BITCHUNK_BITS */

static inline void set_cpu_possible(unsigned int cpu, int possible)
{
    if (possible)
        cpumask_set_cpu(&__cpu_possible_mask, cpu);
    else
        cpumask_clear_cpu(&__cpu_possible_mask, cpu);
}

static inline void set_cpu_online(unsigned int cpu, int online)
{
    if (online)
        cpumask_set_cpu(&__cpu_online_mask, cpu);
    else
        cpumask_clear_cpu(&__cpu_online_mask, cpu);
}

static inline unsigned int cpumask_first(const struct cpumask* srcp)
{
    return find_first_bit(cpumask_bits(srcp), NR_CPUMASK_BITS);
}

#define cpumask_any(srcp) cpumask_first(srcp)

#endif
