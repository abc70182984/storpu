#ifndef _BITMAP_H_
#define _BITMAP_H_

#include <string.h>

typedef unsigned long bitchunk_t;

#define CHAR_BITS       8
#define BITCHUNK_BITS   (sizeof(bitchunk_t) * CHAR_BITS)
#define BITCHUNKS(bits) (((bits) + BITCHUNK_BITS - 1) / BITCHUNK_BITS)

#define MAP_CHUNK(map, bit) (map)[((bit) / BITCHUNK_BITS)]
#define CHUNK_OFFSET(bit)   ((bit) % BITCHUNK_BITS)
#define GET_BIT(map, bit)   (MAP_CHUNK(map, bit) & (1UL << CHUNK_OFFSET(bit)))
#define SET_BIT(map, bit)                                               \
    __atomic_fetch_or(&MAP_CHUNK(map, bit), (1UL << CHUNK_OFFSET(bit)), \
                      __ATOMIC_RELAXED)
#define UNSET_BIT(map, bit)                                               \
    __atomic_fetch_and(&MAP_CHUNK(map, bit), ~(1UL << CHUNK_OFFSET(bit)), \
                       __ATOMIC_RELAXED)

#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITCHUNK_BITS - 1)))
#define BITMAP_LAST_WORD_MASK(nbits)  (~0UL >> (-(nbits) & (BITCHUNK_BITS - 1)))

static inline void bitmap_zero(bitchunk_t* dst, unsigned int nbits)
{
    unsigned int len = BITCHUNKS(nbits) * sizeof(bitchunk_t);
    memset(dst, 0, len);
}

static inline void bitmap_copy(bitchunk_t* dst, const bitchunk_t* src,
                               unsigned int nbits)
{
    unsigned int len = BITCHUNKS(nbits) * sizeof(bitchunk_t);
    memcpy(dst, src, len);
}

int bitmap_equal(const bitchunk_t* bitmap1, const bitchunk_t* bitmap2,
                 unsigned int bits);

#define __small_const_nbits(nbits) \
    (__builtin_constant_p(nbits) && (nbits) <= BITCHUNK_BITS && (nbits) > 0)

#define __GENMASK(h, l) \
    (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITCHUNK_BITS - 1 - (h))))

int __bitmap_and(bitchunk_t* dst, const bitchunk_t* src1,
                 const bitchunk_t* src2, unsigned int nbits);
void __bitmap_or(bitchunk_t* dst, const bitchunk_t* src1,
                 const bitchunk_t* src2, unsigned int nbits);

static inline int bitmap_and(bitchunk_t* dst, const bitchunk_t* src1,
                             const bitchunk_t* src2, unsigned int nbits)
{
    if (__small_const_nbits(nbits))
        return (*dst = *src1 & *src2 & BITMAP_LAST_WORD_MASK(nbits)) != 0;
    return __bitmap_and(dst, src1, src2, nbits);
}

static inline void bitmap_or(bitchunk_t* dst, const bitchunk_t* src1,
                             const bitchunk_t* src2, unsigned int nbits)
{
    if (__small_const_nbits(nbits))
        *dst = *src1 | *src2;
    else
        __bitmap_or(dst, src1, src2, nbits);
}

extern unsigned long _find_next_bit(const unsigned long* addr1,
                                    const unsigned long* addr2,
                                    unsigned long nbits, unsigned long start,
                                    unsigned long invert);

static inline unsigned long find_next_bit(const unsigned long* addr,
                                          unsigned long size,
                                          unsigned long offset)
{
    if (__small_const_nbits(size)) {
        unsigned long val;

        if (offset >= size) return size;

        val = *addr & __GENMASK(size - 1, offset);
        return val ? __builtin_ctzl(val) : size;
    }

    return _find_next_bit(addr, NULL, size, offset, 0UL);
}

static inline unsigned long find_next_and_bit(const unsigned long* addr1,
                                              const unsigned long* addr2,
                                              unsigned long size,
                                              unsigned long offset)
{
    if (__small_const_nbits(size)) {
        unsigned long val;

        if (offset >= size) return size;

        val = *addr1 & *addr2 & __GENMASK(size - 1, offset);
        return val ? __builtin_ctzl(val) : size;
    }

    return _find_next_bit(addr1, addr2, size, offset, 0UL);
}

static inline unsigned long find_next_zero_bit(const unsigned long* addr,
                                               unsigned long size,
                                               unsigned long offset)
{
    if (__small_const_nbits(size)) {
        unsigned long val;

        if (offset >= size) return size;

        val = *addr | ~__GENMASK(size - 1, offset);
        return val == ~0UL ? size : __builtin_ctzl(~val);
    }

    return _find_next_bit(addr, NULL, size, offset, ~0UL);
}

#define find_first_bit(addr, size) find_next_bit((addr), (size), 0)

#endif
