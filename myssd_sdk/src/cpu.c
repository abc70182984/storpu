#include <types.h>
#include <cpumask.h>

#define MASK_DECLARE_1(x) [x + 1][0] = (1UL << (x))
#define MASK_DECLARE_2(x) MASK_DECLARE_1(x), MASK_DECLARE_1(x + 1)
#define MASK_DECLARE_4(x) MASK_DECLARE_2(x), MASK_DECLARE_2(x + 2)
#define MASK_DECLARE_8(x) MASK_DECLARE_4(x), MASK_DECLARE_4(x + 4)

const bitchunk_t cpu_bit_bitmap[BITCHUNK_BITS + 1][BITCHUNKS(NR_CPUS)] = {

    MASK_DECLARE_8(0),  MASK_DECLARE_8(8),
    MASK_DECLARE_8(16), MASK_DECLARE_8(24),
#if BITCHUNKS_BITS > 32
    MASK_DECLARE_8(32), MASK_DECLARE_8(40),
    MASK_DECLARE_8(48), MASK_DECLARE_8(56),
#endif
};

struct cpumask __cpu_possible_mask = {CPU_BITS_ALL};
struct cpumask __cpu_online_mask;
