#include <bitmap.h>
#include <const.h>

int bitmap_equal(const bitchunk_t* bitmap1, const bitchunk_t* bitmap2,
                 unsigned int bits)
{
    unsigned int k, lim = bits / BITCHUNK_BITS;
    for (k = 0; k < lim; ++k)
        if (bitmap1[k] != bitmap2[k]) return 0;

    if (bits % BITCHUNK_BITS)
        if ((bitmap1[k] ^ bitmap2[k]) & BITMAP_LAST_WORD_MASK(bits)) return 0;

    return 1;
}

int __bitmap_and(bitchunk_t* dst, const bitchunk_t* bitmap1,
                 const bitchunk_t* bitmap2, unsigned int bits)
{
    unsigned int k;
    unsigned int lim = bits / BITCHUNK_BITS;
    unsigned long result = 0;

    for (k = 0; k < lim; k++)
        result |= (dst[k] = bitmap1[k] & bitmap2[k]);
    if (bits % BITCHUNK_BITS)
        result |=
            (dst[k] = bitmap1[k] & bitmap2[k] & BITMAP_LAST_WORD_MASK(bits));
    return result != 0;
}

void __bitmap_or(bitchunk_t* dst, const bitchunk_t* bitmap1,
                 const bitchunk_t* bitmap2, unsigned int bits)
{
    unsigned int k;
    unsigned int nr = BITCHUNKS(bits);

    for (k = 0; k < nr; k++)
        dst[k] = bitmap1[k] | bitmap2[k];
}

unsigned long _find_next_bit(const unsigned long* addr1,
                             const unsigned long* addr2, unsigned long nbits,
                             unsigned long start, unsigned long invert)
{
    unsigned long tmp, mask;

    if (start >= nbits) return nbits;

    tmp = addr1[start / BITCHUNK_BITS];
    if (addr2) tmp &= addr2[start / BITCHUNK_BITS];
    tmp ^= invert;

    mask = BITMAP_FIRST_WORD_MASK(start);

    tmp &= mask;

    start = rounddown(start, BITCHUNK_BITS);

    while (!tmp) {
        start += BITCHUNK_BITS;
        if (start >= nbits) return nbits;

        tmp = addr1[start / BITCHUNK_BITS];
        if (addr2) tmp &= addr2[start / BITCHUNK_BITS];
        tmp ^= invert;
    }

    return min(start + __builtin_ctzl(tmp), nbits);
}
