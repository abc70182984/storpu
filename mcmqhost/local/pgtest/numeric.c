#include "postgres.h"
#include "data_types.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define Max(x, y) ((x) > (y) ? (x) : (y))

#define NBASE            10000
#define HALF_NBASE       5000
#define DEC_DIGITS       4 /* decimal digits per NBASE digit */
#define MUL_GUARD_DIGITS 2 /* these are measured in NBASE digits */
#define DIV_GUARD_DIGITS 4

typedef int16_t NumericDigit;

struct NumericShort {
    uint16_t n_header;     /* Sign + display scale + weight */
    NumericDigit n_data[]; /* Digits */
};

struct NumericLong {
    uint16_t n_sign_dscale; /* Sign + display scale */
    int16_t n_weight;       /* Weight of 1st digit	*/
    NumericDigit n_data[];  /* Digits */
};

union NumericChoice {
    uint16_t n_header;           /* Header word */
    struct NumericLong n_long;   /* Long form (4-byte header) */
    struct NumericShort n_short; /* Short form (2-byte header) */
};

struct NumericData {
    int32_t vl_len_;            /* varlena header (do not touch directly!) */
    union NumericChoice choice; /* choice of format */
};

/*
 * Interpretation of high bits.
 */

#define NUMERIC_SIGN_MASK 0xC000
#define NUMERIC_POS       0x0000
#define NUMERIC_NEG       0x4000
#define NUMERIC_SHORT     0x8000
#define NUMERIC_SPECIAL   0xC000

#define NUMERIC_FLAGBITS(n)   ((n)->choice.n_header & NUMERIC_SIGN_MASK)
#define NUMERIC_IS_SHORT(n)   (NUMERIC_FLAGBITS(n) == NUMERIC_SHORT)
#define NUMERIC_IS_SPECIAL(n) (NUMERIC_FLAGBITS(n) == NUMERIC_SPECIAL)

#define NUMERIC_HDRSZ       (VARHDRSZ + sizeof(uint16_t) + sizeof(int16_t))
#define NUMERIC_HDRSZ_SHORT (VARHDRSZ + sizeof(uint16_t))

/*
 * If the flag bits are NUMERIC_SHORT or NUMERIC_SPECIAL, we want the short
 * header; otherwise, we want the long one.  Instead of testing against each
 * value, we can just look at the high bit, for a slight efficiency gain.
 */
#define NUMERIC_HEADER_IS_SHORT(n) (((n)->choice.n_header & 0x8000) != 0)
#define NUMERIC_HEADER_SIZE(n)     \
    (VARHDRSZ + sizeof(uint16_t) + \
     (NUMERIC_HEADER_IS_SHORT(n) ? 0 : sizeof(int16_t)))

/*
 * Definitions for special values (NaN, positive infinity, negative infinity).
 *
 * The two bits after the NUMERIC_SPECIAL bits are 00 for NaN, 01 for positive
 * infinity, 11 for negative infinity.  (This makes the sign bit match where
 * it is in a short-format value, though we make no use of that at present.)
 * We could mask off the remaining bits before testing the active bits, but
 * currently those bits must be zeroes, so masking would just add cycles.
 */
#define NUMERIC_EXT_SIGN_MASK 0xF000 /* high bits plus NaN/Inf flag bits */
#define NUMERIC_NAN           0xC000
#define NUMERIC_PINF          0xD000
#define NUMERIC_NINF          0xF000
#define NUMERIC_INF_SIGN_MASK 0x2000

#define NUMERIC_EXT_FLAGBITS(n) ((n)->choice.n_header & NUMERIC_EXT_SIGN_MASK)
#define NUMERIC_IS_NAN(n)       ((n)->choice.n_header == NUMERIC_NAN)
#define NUMERIC_IS_PINF(n)      ((n)->choice.n_header == NUMERIC_PINF)
#define NUMERIC_IS_NINF(n)      ((n)->choice.n_header == NUMERIC_NINF)
#define NUMERIC_IS_INF(n) \
    (((n)->choice.n_header & ~NUMERIC_INF_SIGN_MASK) == NUMERIC_PINF)

/*
 * Short format definitions.
 */

#define NUMERIC_SHORT_SIGN_MASK    0x2000
#define NUMERIC_SHORT_DSCALE_MASK  0x1F80
#define NUMERIC_SHORT_DSCALE_SHIFT 7
#define NUMERIC_SHORT_DSCALE_MAX \
    (NUMERIC_SHORT_DSCALE_MASK >> NUMERIC_SHORT_DSCALE_SHIFT)
#define NUMERIC_SHORT_WEIGHT_SIGN_MASK 0x0040
#define NUMERIC_SHORT_WEIGHT_MASK      0x003F
#define NUMERIC_SHORT_WEIGHT_MAX       NUMERIC_SHORT_WEIGHT_MASK
#define NUMERIC_SHORT_WEIGHT_MIN       (-(NUMERIC_SHORT_WEIGHT_MASK + 1))

/*
 * Extract sign, display scale, weight.  These macros extract field values
 * suitable for the NumericVar format from the Numeric (on-disk) format.
 *
 * Note that we don't trouble to ensure that dscale and weight read as zero
 * for an infinity; however, that doesn't matter since we never convert
 * "special" numerics to NumericVar form.  Only the constants defined below
 * (const_nan, etc) ever represent a non-finite value as a NumericVar.
 */

#define NUMERIC_DSCALE_MASK 0x3FFF
#define NUMERIC_DSCALE_MAX  NUMERIC_DSCALE_MASK

#define NUMERIC_SIGN(n)                                              \
    (NUMERIC_IS_SHORT(n)                                             \
         ? (((n)->choice.n_short.n_header & NUMERIC_SHORT_SIGN_MASK) \
                ? NUMERIC_NEG                                        \
                : NUMERIC_POS)                                       \
         : (NUMERIC_IS_SPECIAL(n) ? NUMERIC_EXT_FLAGBITS(n)          \
                                  : NUMERIC_FLAGBITS(n)))
#define NUMERIC_DSCALE(n)                                                \
    (NUMERIC_HEADER_IS_SHORT((n))                                        \
         ? ((n)->choice.n_short.n_header & NUMERIC_SHORT_DSCALE_MASK) >> \
               NUMERIC_SHORT_DSCALE_SHIFT                                \
         : ((n)->choice.n_long.n_sign_dscale & NUMERIC_DSCALE_MASK))
#define NUMERIC_WEIGHT(n)                                                  \
    (NUMERIC_HEADER_IS_SHORT((n))                                          \
         ? (((n)->choice.n_short.n_header & NUMERIC_SHORT_WEIGHT_SIGN_MASK \
                 ? ~NUMERIC_SHORT_WEIGHT_MASK                              \
                 : 0) |                                                    \
            ((n)->choice.n_short.n_header & NUMERIC_SHORT_WEIGHT_MASK))    \
         : ((n)->choice.n_long.n_weight))

#define NUMERIC_DIGITS(num)                                      \
    (NUMERIC_HEADER_IS_SHORT(num) ? (num)->choice.n_short.n_data \
                                  : (num)->choice.n_long.n_data)
#define NUMERIC_NDIGITS(num) \
    ((VARSIZE(num) - NUMERIC_HEADER_SIZE(num)) / sizeof(NumericDigit))
#define NUMERIC_CAN_BE_SHORT(scale, weight)  \
    ((scale) <= NUMERIC_SHORT_DSCALE_MAX &&  \
     (weight) <= NUMERIC_SHORT_WEIGHT_MAX && \
     (weight) >= NUMERIC_SHORT_WEIGHT_MIN)

typedef struct NumericVar {
    int ndigits;          /* # of digits in digits[] - can be 0! */
    int weight;           /* weight of first digit */
    int sign;             /* NUMERIC_POS, _NEG, _NAN, _PINF, or _NINF */
    int dscale;           /* display scale */
    NumericDigit* buf;    /* start of palloc'd space for digits[] */
    NumericDigit* digits; /* base-NBASE digits */
} NumericVar;

static const NumericDigit const_zero_data[1] = {0};
static const NumericVar const_zero = {0, 0,    NUMERIC_POS,
                                      0, NULL, (NumericDigit*)const_zero_data};

static const NumericDigit const_one_data[1] = {1};
static const NumericVar const_one = {1, 0,    NUMERIC_POS,
                                     0, NULL, (NumericDigit*)const_one_data};

static const NumericVar const_minus_one = {
    1, 0, NUMERIC_NEG, 0, NULL, (NumericDigit*)const_one_data};

static const NumericDigit const_two_data[1] = {2};
static const NumericVar const_two = {1, 0,    NUMERIC_POS,
                                     0, NULL, (NumericDigit*)const_two_data};

static const NumericVar const_nan = {0, 0, NUMERIC_NAN, 0, NULL, NULL};

static const NumericVar const_pinf = {0, 0, NUMERIC_PINF, 0, NULL, NULL};

static const NumericVar const_ninf = {0, 0, NUMERIC_NINF, 0, NULL, NULL};

typedef struct NumericSumAccum {
    int ndigits;
    int weight;
    int dscale;
    int num_uncarried;
    bool have_carry_space;
    int32_t* pos_digits;
    int32_t* neg_digits;
} NumericSumAccum;

#define digitbuf_alloc(ndigits) \
    ((NumericDigit*)malloc((ndigits) * sizeof(NumericDigit)))
#define digitbuf_free(buf)            \
    do {                              \
        if ((buf) != NULL) free(buf); \
    } while (0)

#define init_var(v) memset(v, 0, sizeof(NumericVar))

static void add_abs(const NumericVar* var1, const NumericVar* var2,
                    NumericVar* result);
static void sub_abs(const NumericVar* var1, const NumericVar* var2,
                    NumericVar* result);
static int cmp_abs(const NumericVar* var1, const NumericVar* var2);
static int cmp_abs_common(const NumericDigit* var1digits, int var1ndigits,
                          int var1weight, const NumericDigit* var2digits,
                          int var2ndigits, int var2weight);

static void alloc_var(NumericVar* var, int ndigits)
{
    digitbuf_free(var->buf);
    var->buf = digitbuf_alloc(ndigits + 1);
    var->buf[0] = 0; /* spare digit for rounding */
    var->digits = var->buf + 1;
    var->ndigits = ndigits;
}

static void free_var(NumericVar* var)
{
    digitbuf_free(var->buf);
    var->buf = NULL;
    var->digits = NULL;
    var->sign = NUMERIC_NAN;
}

static void zero_var(NumericVar* var)
{
    digitbuf_free(var->buf);
    var->buf = NULL;
    var->digits = NULL;
    var->ndigits = 0;
    var->weight = 0;         /* by convention; doesn't really matter */
    var->sign = NUMERIC_POS; /* anything but NAN... */
}

static void init_var_from_num(Numeric num, NumericVar* dest)
{
    dest->ndigits = NUMERIC_NDIGITS(num);
    dest->weight = NUMERIC_WEIGHT(num);
    dest->sign = NUMERIC_SIGN(num);
    dest->dscale = NUMERIC_DSCALE(num);
    dest->digits = NUMERIC_DIGITS(num);
    dest->buf = NULL; /* digits array is not palloc'd */
}

static void set_var_from_var(const NumericVar* value, NumericVar* dest)
{
    NumericDigit* newbuf;

    newbuf = digitbuf_alloc(value->ndigits + 1);
    newbuf[0] = 0;          /* spare digit for rounding */
    if (value->ndigits > 0) /* else value->digits might be null */
        memcpy(newbuf + 1, value->digits,
               value->ndigits * sizeof(NumericDigit));

    digitbuf_free(dest->buf);

    memmove(dest, value, sizeof(NumericVar));
    dest->buf = newbuf;
    dest->digits = newbuf + 1;
}

static void strip_var(NumericVar* var)
{
    NumericDigit* digits = var->digits;
    int ndigits = var->ndigits;

    while (ndigits > 0 && *digits == 0) {
        digits++;
        var->weight--;
        ndigits--;
    }

    while (ndigits > 0 && digits[ndigits - 1] == 0)
        ndigits--;

    if (ndigits == 0) {
        var->sign = NUMERIC_POS;
        var->weight = 0;
    }

    var->digits = digits;
    var->ndigits = ndigits;
}

static void add_var(const NumericVar* var1, const NumericVar* var2,
                    NumericVar* result)
{
    /*
     * Decide on the signs of the two variables what to do
     */
    if (var1->sign == NUMERIC_POS) {
        if (var2->sign == NUMERIC_POS) {
            /*
             * Both are positive result = +(ABS(var1) + ABS(var2))
             */
            add_abs(var1, var2, result);
            result->sign = NUMERIC_POS;
        } else {
            /*
             * var1 is positive, var2 is negative Must compare absolute values
             */
            switch (cmp_abs(var1, var2)) {
            case 0:
                /* ----------
                 * ABS(var1) == ABS(var2)
                 * result = ZERO
                 * ----------
                 */
                zero_var(result);
                result->dscale = Max(var1->dscale, var2->dscale);
                break;

            case 1:
                /* ----------
                 * ABS(var1) > ABS(var2)
                 * result = +(ABS(var1) - ABS(var2))
                 * ----------
                 */
                sub_abs(var1, var2, result);
                result->sign = NUMERIC_POS;
                break;

            case -1:
                /* ----------
                 * ABS(var1) < ABS(var2)
                 * result = -(ABS(var2) - ABS(var1))
                 * ----------
                 */
                sub_abs(var2, var1, result);
                result->sign = NUMERIC_NEG;
                break;
            }
        }
    } else {
        if (var2->sign == NUMERIC_POS) {
            /* ----------
             * var1 is negative, var2 is positive
             * Must compare absolute values
             * ----------
             */
            switch (cmp_abs(var1, var2)) {
            case 0:
                /* ----------
                 * ABS(var1) == ABS(var2)
                 * result = ZERO
                 * ----------
                 */
                zero_var(result);
                result->dscale = Max(var1->dscale, var2->dscale);
                break;

            case 1:
                /* ----------
                 * ABS(var1) > ABS(var2)
                 * result = -(ABS(var1) - ABS(var2))
                 * ----------
                 */
                sub_abs(var1, var2, result);
                result->sign = NUMERIC_NEG;
                break;

            case -1:
                /* ----------
                 * ABS(var1) < ABS(var2)
                 * result = +(ABS(var2) - ABS(var1))
                 * ----------
                 */
                sub_abs(var2, var1, result);
                result->sign = NUMERIC_POS;
                break;
            }
        } else {
            /* ----------
             * Both are negative
             * result = -(ABS(var1) + ABS(var2))
             * ----------
             */
            add_abs(var1, var2, result);
            result->sign = NUMERIC_NEG;
        }
    }
}

static char* get_str_from_var(const NumericVar* var)
{
    int dscale;
    char* str;
    char* cp;
    char* endcp;
    int i;
    int d;
    NumericDigit dig;

#if DEC_DIGITS > 1
    NumericDigit d1;
#endif

    dscale = var->dscale;

    /*
     * Allocate space for the result.
     *
     * i is set to the # of decimal digits before decimal point. dscale is the
     * # of decimal digits we will print after decimal point. We may generate
     * as many as DEC_DIGITS-1 excess digits at the end, and in addition we
     * need room for sign, decimal point, null terminator.
     */
    i = (var->weight + 1) * DEC_DIGITS;
    if (i <= 0) i = 1;

    str = malloc(i + dscale + DEC_DIGITS + 2);
    cp = str;

    /*
     * Output a dash for negative values
     */
    if (var->sign == NUMERIC_NEG) *cp++ = '-';

    /*
     * Output all digits before the decimal point
     */
    if (var->weight < 0) {
        d = var->weight + 1;
        *cp++ = '0';
    } else {
        for (d = 0; d <= var->weight; d++) {
            dig = (d < var->ndigits) ? var->digits[d] : 0;
            /* In the first digit, suppress extra leading decimal zeroes */
#if DEC_DIGITS == 4
            {
                bool putit = (d > 0);

                d1 = dig / 1000;
                dig -= d1 * 1000;
                putit |= (d1 > 0);
                if (putit) *cp++ = d1 + '0';
                d1 = dig / 100;
                dig -= d1 * 100;
                putit |= (d1 > 0);
                if (putit) *cp++ = d1 + '0';
                d1 = dig / 10;
                dig -= d1 * 10;
                putit |= (d1 > 0);
                if (putit) *cp++ = d1 + '0';
                *cp++ = dig + '0';
            }
#elif DEC_DIGITS == 2
            d1 = dig / 10;
            dig -= d1 * 10;
            if (d1 > 0 || d > 0) *cp++ = d1 + '0';
            *cp++ = dig + '0';
#elif DEC_DIGITS == 1
            *cp++ = dig + '0';
#else
#error unsupported NBASE
#endif
        }
    }

    /*
     * If requested, output a decimal point and all the digits that follow it.
     * We initially put out a multiple of DEC_DIGITS digits, then truncate if
     * needed.
     */
    if (dscale > 0) {
        *cp++ = '.';
        endcp = cp + dscale;
        for (i = 0; i < dscale; d++, i += DEC_DIGITS) {
            dig = (d >= 0 && d < var->ndigits) ? var->digits[d] : 0;
#if DEC_DIGITS == 4
            d1 = dig / 1000;
            dig -= d1 * 1000;
            *cp++ = d1 + '0';
            d1 = dig / 100;
            dig -= d1 * 100;
            *cp++ = d1 + '0';
            d1 = dig / 10;
            dig -= d1 * 10;
            *cp++ = d1 + '0';
            *cp++ = dig + '0';
#elif DEC_DIGITS == 2
            d1 = dig / 10;
            dig -= d1 * 10;
            *cp++ = d1 + '0';
            *cp++ = dig + '0';
#elif DEC_DIGITS == 1
            *cp++ = dig + '0';
#else
#error unsupported NBASE
#endif
        }
        cp = endcp;
    }

    /*
     * terminate the string and return it
     */
    *cp = '\0';
    return str;
}

Datum numeric_out(PG_FUNCTION_ARGS)
{
    Numeric num = (Numeric)PG_GETARG_DATUM(0);
    NumericVar x;
    char* str;

    /*
     * Handle NaN and infinities
     */
    if (NUMERIC_IS_SPECIAL(num)) {
        if (NUMERIC_IS_PINF(num))
            return (Datum)strdup("Infinity");
        else if (NUMERIC_IS_NINF(num))
            return (Datum)strdup("-Infinity");
        else
            return (Datum)strdup("NaN");
    }

    /*
     * Get the number in the variable format.
     */
    init_var_from_num(num, &x);

    str = get_str_from_var(&x);

    return (Datum)str;
}

static Numeric make_result(const NumericVar* var)
{
    Numeric result;
    NumericDigit* digits = var->digits;
    int weight = var->weight;
    int sign = var->sign;
    int n;
    size_t len;

    if ((sign & NUMERIC_SIGN_MASK) == NUMERIC_SPECIAL) {
        /*
         * Verify valid special value.  This could be just an Assert, perhaps,
         * but it seems worthwhile to expend a few cycles to ensure that we
         * never write any nonzero reserved bits to disk.
         */

        result = (Numeric)malloc(NUMERIC_HDRSZ_SHORT);

        SET_VARSIZE(result, NUMERIC_HDRSZ_SHORT);
        result->choice.n_header = sign;

        return result;
    }

    n = var->ndigits;

    /* truncate leading zeroes */
    while (n > 0 && *digits == 0) {
        digits++;
        weight--;
        n--;
    }
    /* truncate trailing zeroes */
    while (n > 0 && digits[n - 1] == 0)
        n--;

    /* If zero result, force to weight=0 and positive sign */
    if (n == 0) {
        weight = 0;
        sign = NUMERIC_POS;
    }

    /* Build the result */
    if (NUMERIC_CAN_BE_SHORT(var->dscale, weight)) {
        len = NUMERIC_HDRSZ_SHORT + n * sizeof(NumericDigit);
        result = (Numeric)malloc(len);
        SET_VARSIZE(result, len);
        result->choice.n_short.n_header =
            (sign == NUMERIC_NEG ? (NUMERIC_SHORT | NUMERIC_SHORT_SIGN_MASK)
                                 : NUMERIC_SHORT) |
            (var->dscale << NUMERIC_SHORT_DSCALE_SHIFT) |
            (weight < 0 ? NUMERIC_SHORT_WEIGHT_SIGN_MASK : 0) |
            (weight & NUMERIC_SHORT_WEIGHT_MASK);
    } else {
        len = NUMERIC_HDRSZ + n * sizeof(NumericDigit);
        result = (Numeric)malloc(len);
        SET_VARSIZE(result, len);
        result->choice.n_long.n_sign_dscale =
            sign | (var->dscale & NUMERIC_DSCALE_MASK);
        result->choice.n_long.n_weight = weight;
    }

    if (n > 0) memcpy(NUMERIC_DIGITS(result), digits, n * sizeof(NumericDigit));

    return result;
}

static void int64_to_numericvar(int64_t val, NumericVar* var)
{
    uint64_t uval, newuval;
    NumericDigit* ptr;
    int ndigits;

    /* int64 can require at most 19 decimal digits; add one for safety */
    alloc_var(var, 20 / DEC_DIGITS);
    if (val < 0) {
        var->sign = NUMERIC_NEG;
        uval = -val;
    } else {
        var->sign = NUMERIC_POS;
        uval = val;
    }
    var->dscale = 0;
    if (val == 0) {
        var->ndigits = 0;
        var->weight = 0;
        return;
    }
    ptr = var->digits + var->ndigits;
    ndigits = 0;
    do {
        ptr--;
        ndigits++;
        newuval = uval / NBASE;
        *ptr = uval - newuval * NBASE;
        uval = newuval;
    } while (uval);
    var->digits = ptr;
    var->ndigits = ndigits;
    var->weight = ndigits - 1;
}

Numeric int64_to_numeric(int64_t val)
{
    Numeric res;
    NumericVar result;

    memset(&result, 0, sizeof(result));

    int64_to_numericvar(val, &result);

    res = make_result(&result);

    free_var(&result);

    return res;
}

static void add_abs(const NumericVar* var1, const NumericVar* var2,
                    NumericVar* result)
{
    NumericDigit* res_buf;
    NumericDigit* res_digits;
    int res_ndigits;
    int res_weight;
    int res_rscale, rscale1, rscale2;
    int res_dscale;
    int i, i1, i2;
    int carry = 0;

    /* copy these values into local vars for speed in inner loop */
    int var1ndigits = var1->ndigits;
    int var2ndigits = var2->ndigits;
    NumericDigit* var1digits = var1->digits;
    NumericDigit* var2digits = var2->digits;

    res_weight = Max(var1->weight, var2->weight) + 1;

    res_dscale = Max(var1->dscale, var2->dscale);

    /* Note: here we are figuring rscale in base-NBASE digits */
    rscale1 = var1->ndigits - var1->weight - 1;
    rscale2 = var2->ndigits - var2->weight - 1;
    res_rscale = Max(rscale1, rscale2);

    res_ndigits = res_rscale + res_weight + 1;
    if (res_ndigits <= 0) res_ndigits = 1;

    res_buf = digitbuf_alloc(res_ndigits + 1);
    res_buf[0] = 0; /* spare digit for later rounding */
    res_digits = res_buf + 1;

    i1 = res_rscale + var1->weight + 1;
    i2 = res_rscale + var2->weight + 1;
    for (i = res_ndigits - 1; i >= 0; i--) {
        i1--;
        i2--;
        if (i1 >= 0 && i1 < var1ndigits) carry += var1digits[i1];
        if (i2 >= 0 && i2 < var2ndigits) carry += var2digits[i2];

        if (carry >= NBASE) {
            res_digits[i] = carry - NBASE;
            carry = 1;
        } else {
            res_digits[i] = carry;
            carry = 0;
        }
    }

    digitbuf_free(result->buf);
    result->ndigits = res_ndigits;
    result->buf = res_buf;
    result->digits = res_digits;
    result->weight = res_weight;
    result->dscale = res_dscale;

    /* Remove leading/trailing zeroes */
    strip_var(result);
}

static void sub_abs(const NumericVar* var1, const NumericVar* var2,
                    NumericVar* result)
{
    NumericDigit* res_buf;
    NumericDigit* res_digits;
    int res_ndigits;
    int res_weight;
    int res_rscale, rscale1, rscale2;
    int res_dscale;
    int i, i1, i2;
    int borrow = 0;

    /* copy these values into local vars for speed in inner loop */
    int var1ndigits = var1->ndigits;
    int var2ndigits = var2->ndigits;
    NumericDigit* var1digits = var1->digits;
    NumericDigit* var2digits = var2->digits;

    res_weight = var1->weight;

    res_dscale = Max(var1->dscale, var2->dscale);

    /* Note: here we are figuring rscale in base-NBASE digits */
    rscale1 = var1->ndigits - var1->weight - 1;
    rscale2 = var2->ndigits - var2->weight - 1;
    res_rscale = Max(rscale1, rscale2);

    res_ndigits = res_rscale + res_weight + 1;
    if (res_ndigits <= 0) res_ndigits = 1;

    res_buf = digitbuf_alloc(res_ndigits + 1);
    res_buf[0] = 0; /* spare digit for later rounding */
    res_digits = res_buf + 1;

    i1 = res_rscale + var1->weight + 1;
    i2 = res_rscale + var2->weight + 1;
    for (i = res_ndigits - 1; i >= 0; i--) {
        i1--;
        i2--;
        if (i1 >= 0 && i1 < var1ndigits) borrow += var1digits[i1];
        if (i2 >= 0 && i2 < var2ndigits) borrow -= var2digits[i2];

        if (borrow < 0) {
            res_digits[i] = borrow + NBASE;
            borrow = -1;
        } else {
            res_digits[i] = borrow;
            borrow = 0;
        }
    }

    digitbuf_free(result->buf);
    result->ndigits = res_ndigits;
    result->buf = res_buf;
    result->digits = res_digits;
    result->weight = res_weight;
    result->dscale = res_dscale;

    /* Remove leading/trailing zeroes */
    strip_var(result);
}

static int cmp_abs(const NumericVar* var1, const NumericVar* var2)
{
    return cmp_abs_common(var1->digits, var1->ndigits, var1->weight,
                          var2->digits, var2->ndigits, var2->weight);
}

static int cmp_abs_common(const NumericDigit* var1digits, int var1ndigits,
                          int var1weight, const NumericDigit* var2digits,
                          int var2ndigits, int var2weight)
{
    int i1 = 0;
    int i2 = 0;

    while (var1weight > var2weight && i1 < var1ndigits) {
        if (var1digits[i1++] != 0) return 1;
        var1weight--;
    }
    while (var2weight > var1weight && i2 < var2ndigits) {
        if (var2digits[i2++] != 0) return -1;
        var2weight--;
    }

    if (var1weight == var2weight) {
        while (i1 < var1ndigits && i2 < var2ndigits) {
            int stat = var1digits[i1++] - var2digits[i2++];

            if (stat) {
                if (stat > 0) return 1;
                return -1;
            }
        }
    }

    while (i1 < var1ndigits) {
        if (var1digits[i1++] != 0) return 1;
    }
    while (i2 < var2ndigits) {
        if (var2digits[i2++] != 0) return -1;
    }

    return 0;
}

static int cmp_var_common(const NumericDigit* var1digits, int var1ndigits,
                          int var1weight, int var1sign,
                          const NumericDigit* var2digits, int var2ndigits,
                          int var2weight, int var2sign)
{
    if (var1ndigits == 0) {
        if (var2ndigits == 0) return 0;
        if (var2sign == NUMERIC_NEG) return 1;
        return -1;
    }
    if (var2ndigits == 0) {
        if (var1sign == NUMERIC_POS) return 1;
        return -1;
    }

    if (var1sign == NUMERIC_POS) {
        if (var2sign == NUMERIC_NEG) return 1;
        return cmp_abs_common(var1digits, var1ndigits, var1weight, var2digits,
                              var2ndigits, var2weight);
    }

    if (var2sign == NUMERIC_POS) return -1;

    return cmp_abs_common(var2digits, var2ndigits, var2weight, var1digits,
                          var1ndigits, var1weight);
}

static int cmp_numerics(Numeric num1, Numeric num2)
{
    int result;

    if (NUMERIC_IS_SPECIAL(num1)) {
        if (NUMERIC_IS_NAN(num1)) {
            if (NUMERIC_IS_NAN(num2))
                result = 0; /* NAN = NAN */
            else
                result = 1; /* NAN > non-NAN */
        } else if (NUMERIC_IS_PINF(num1)) {
            if (NUMERIC_IS_NAN(num2))
                result = -1; /* PINF < NAN */
            else if (NUMERIC_IS_PINF(num2))
                result = 0; /* PINF = PINF */
            else
                result = 1; /* PINF > anything else */
        } else              /* num1 must be NINF */
        {
            if (NUMERIC_IS_NINF(num2))
                result = 0; /* NINF = NINF */
            else
                result = -1; /* NINF < anything else */
        }
    } else if (NUMERIC_IS_SPECIAL(num2)) {
        if (NUMERIC_IS_NINF(num2))
            result = 1; /* normal > NINF */
        else
            result = -1; /* normal < NAN or PINF */
    } else {
        result = cmp_var_common(NUMERIC_DIGITS(num1), NUMERIC_NDIGITS(num1),
                                NUMERIC_WEIGHT(num1), NUMERIC_SIGN(num1),
                                NUMERIC_DIGITS(num2), NUMERIC_NDIGITS(num2),
                                NUMERIC_WEIGHT(num2), NUMERIC_SIGN(num2));
    }

    return result;
}

Datum numeric_cmp(PG_FUNCTION_ARGS)
{
    Datum x = PG_GETARG_DATUM(0);
    Datum y = PG_GETARG_DATUM(1);
    Numeric nx = (Numeric)PG_DETOAST_DATUM(x);
    Numeric ny = (Numeric)PG_DETOAST_DATUM(y);
    int result;

    result = cmp_numerics(nx, ny);

    if ((Datum)nx != x) free(nx);
    if ((Datum)ny != y) free(ny);

    return result;
}

Datum numeric_lt(PG_FUNCTION_ARGS)
{
    Datum x = PG_GETARG_DATUM(0);
    Datum y = PG_GETARG_DATUM(1);
    Numeric nx = (Numeric)PG_DETOAST_DATUM(x);
    Numeric ny = (Numeric)PG_DETOAST_DATUM(y);
    bool result;

    result = cmp_numerics(nx, ny) < 0;

    if ((Datum)nx != x) free(nx);
    if ((Datum)ny != y) free(ny);

    return (Datum)result;
}

Datum numeric_le(PG_FUNCTION_ARGS)
{
    Datum x = PG_GETARG_DATUM(0);
    Datum y = PG_GETARG_DATUM(1);
    Numeric nx = (Numeric)PG_DETOAST_DATUM(x);
    Numeric ny = (Numeric)PG_DETOAST_DATUM(y);
    bool result;

    result = cmp_numerics(nx, ny) <= 0;

    if ((Datum)nx != x) free(nx);
    if ((Datum)ny != y) free(ny);

    return (Datum)result;
}

Datum numeric_gt(PG_FUNCTION_ARGS)
{
    Datum x = PG_GETARG_DATUM(0);
    Datum y = PG_GETARG_DATUM(1);
    Numeric nx = (Numeric)PG_DETOAST_DATUM(x);
    Numeric ny = (Numeric)PG_DETOAST_DATUM(y);
    bool result;

    result = cmp_numerics(nx, ny) > 0;

    if ((Datum)nx != x) free(nx);
    if ((Datum)ny != y) free(ny);

    return (Datum)result;
}

Datum numeric_ge(PG_FUNCTION_ARGS)
{
    Datum x = PG_GETARG_DATUM(0);
    Datum y = PG_GETARG_DATUM(1);
    Numeric nx = (Numeric)PG_DETOAST_DATUM(x);
    Numeric ny = (Numeric)PG_DETOAST_DATUM(y);
    bool result;

    result = cmp_numerics(nx, ny) >= 0;

    if ((Datum)nx != x) free(nx);
    if ((Datum)ny != y) free(ny);

    return (Datum)result;
}

Datum numeric_smaller(PG_FUNCTION_ARGS)
{
    Datum x = PG_GETARG_DATUM(0);
    Datum y = PG_GETARG_DATUM(1);
    Numeric nx = (Numeric)PG_DETOAST_DATUM(x);
    Numeric ny = (Numeric)PG_DETOAST_DATUM(y);

    if (cmp_numerics(nx, ny) < 0) {
        if ((Datum)ny != y) free(ny);
        return (Datum)nx;
    } else {
        if ((Datum)nx != x) free(nx);
        return (Datum)ny;
    }
}

Datum numeric_larger(PG_FUNCTION_ARGS)
{
    Datum x = PG_GETARG_DATUM(0);
    Datum y = PG_GETARG_DATUM(1);
    Numeric nx = (Numeric)PG_DETOAST_DATUM(x);
    Numeric ny = (Numeric)PG_DETOAST_DATUM(y);

    if (cmp_numerics(nx, ny) > 0) {
        if ((Datum)ny != y) free(ny);
        return (Datum)nx;
    } else {
        if ((Datum)nx != x) free(nx);
        return (Datum)ny;
    }
}

static void accum_sum_rescale(NumericSumAccum* accum, const NumericVar* val)
{
    int old_weight = accum->weight;
    int old_ndigits = accum->ndigits;
    int accum_ndigits;
    int accum_weight;
    int accum_rscale;
    int val_rscale;

    accum_weight = old_weight;
    accum_ndigits = old_ndigits;

    if (val->weight >= accum_weight) {
        accum_weight = val->weight + 1;
        accum_ndigits = accum_ndigits + (accum_weight - old_weight);
    }

    else if (!accum->have_carry_space) {
        accum_weight++;
        accum_ndigits++;
    }

    accum_rscale = accum_ndigits - accum_weight - 1;
    val_rscale = val->ndigits - val->weight - 1;
    if (val_rscale > accum_rscale)
        accum_ndigits = accum_ndigits + (val_rscale - accum_rscale);

    if (accum_ndigits != old_ndigits || accum_weight != old_weight) {
        int32_t* new_pos_digits;
        int32_t* new_neg_digits;
        int weightdiff;

        weightdiff = accum_weight - old_weight;

        new_pos_digits = malloc(accum_ndigits * sizeof(int32_t));
        new_neg_digits = malloc(accum_ndigits * sizeof(int32_t));
        memset(new_pos_digits, 0, accum_ndigits * sizeof(int32_t));
        memset(new_neg_digits, 0, accum_ndigits * sizeof(int32_t));

        if (accum->pos_digits) {
            memcpy(&new_pos_digits[weightdiff], accum->pos_digits,
                   old_ndigits * sizeof(int32_t));
            free(accum->pos_digits);

            memcpy(&new_neg_digits[weightdiff], accum->neg_digits,
                   old_ndigits * sizeof(int32_t));
            free(accum->neg_digits);
        }

        accum->pos_digits = new_pos_digits;
        accum->neg_digits = new_neg_digits;

        accum->weight = accum_weight;
        accum->ndigits = accum_ndigits;

        accum->have_carry_space = true;
    }

    if (val->dscale > accum->dscale) accum->dscale = val->dscale;
}

static void accum_sum_carry(NumericSumAccum* accum)
{
    int i;
    int ndigits;
    int32_t* dig;
    int32_t carry;
    int32_t newdig = 0;

    if (accum->num_uncarried == 0) return;

    ndigits = accum->ndigits;

    dig = accum->pos_digits;
    carry = 0;
    for (i = ndigits - 1; i >= 0; i--) {
        newdig = dig[i] + carry;
        if (newdig >= NBASE) {
            carry = newdig / NBASE;
            newdig -= carry * NBASE;
        } else
            carry = 0;
        dig[i] = newdig;
    }

    if (newdig > 0) accum->have_carry_space = false;

    dig = accum->neg_digits;
    carry = 0;
    for (i = ndigits - 1; i >= 0; i--) {
        newdig = dig[i] + carry;
        if (newdig >= NBASE) {
            carry = newdig / NBASE;
            newdig -= carry * NBASE;
        } else
            carry = 0;
        dig[i] = newdig;
    }
    if (newdig > 0) accum->have_carry_space = false;

    accum->num_uncarried = 0;
}

static void accum_sum_add(NumericSumAccum* accum, const NumericVar* val)
{
    int32_t* accum_digits;
    int i, val_i;
    int val_ndigits;
    NumericDigit* val_digits;

    if (accum->num_uncarried == NBASE - 1) accum_sum_carry(accum);

    accum_sum_rescale(accum, val);

    if (val->sign == NUMERIC_POS)
        accum_digits = accum->pos_digits;
    else
        accum_digits = accum->neg_digits;

    val_ndigits = val->ndigits;
    val_digits = val->digits;

    i = accum->weight - val->weight;
    for (val_i = 0; val_i < val_ndigits; val_i++) {
        accum_digits[i] += (int32_t)val_digits[val_i];
        i++;
    }

    accum->num_uncarried++;
}

static void accum_sum_final(NumericSumAccum* accum, NumericVar* result)
{
    int i;
    NumericVar pos_var;
    NumericVar neg_var;

    if (accum->ndigits == 0) {
        set_var_from_var(&const_zero, result);
        return;
    }

    /* Perform final carry */
    accum_sum_carry(accum);

    /* Create NumericVars representing the positive and negative sums */
    init_var(&pos_var);
    init_var(&neg_var);

    pos_var.ndigits = neg_var.ndigits = accum->ndigits;
    pos_var.weight = neg_var.weight = accum->weight;
    pos_var.dscale = neg_var.dscale = accum->dscale;
    pos_var.sign = NUMERIC_POS;
    neg_var.sign = NUMERIC_NEG;

    pos_var.buf = pos_var.digits = digitbuf_alloc(accum->ndigits);
    neg_var.buf = neg_var.digits = digitbuf_alloc(accum->ndigits);

    for (i = 0; i < accum->ndigits; i++) {
        pos_var.digits[i] = (int16_t)accum->pos_digits[i];

        neg_var.digits[i] = (int16_t)accum->neg_digits[i];
    }

    /* And add them together */
    add_var(&pos_var, &neg_var, result);

    /* Remove leading/trailing zeroes */
    strip_var(result);
}

typedef struct NumericAggState {
    bool calcSumX2;
    int64_t N;
    NumericSumAccum sumX;  /* sum of processed numbers */
    NumericSumAccum sumX2; /* sum of squares of processed numbers */
    int maxScale;          /* maximum scale seen so far */
    int64_t maxScaleCount; /* number of values seen with maximum scale */
    /* These counts are *not* included in N!  Use NA_TOTAL_COUNT() as needed */
    int64_t NaNcount;  /* count of NaN values */
    int64_t pInfcount; /* count of +Inf values */
    int64_t nInfcount; /* count of -Inf values */
} NumericAggState;

#define NA_TOTAL_COUNT(na) \
    ((na)->N + (na)->NaNcount + (na)->pInfcount + (na)->nInfcount)

static NumericAggState* makeNumericAggState(FunctionCallInfo fcinfo,
                                            bool calcSumX2)
{
    NumericAggState* state;

    state = (NumericAggState*)malloc(sizeof(NumericAggState));
    memset(state, 0, sizeof(*state));
    state->calcSumX2 = calcSumX2;

    return state;
}

static void do_numeric_accum(NumericAggState* state, Numeric newval)
{
    NumericVar X;
    NumericVar X2;

    if (NUMERIC_IS_SPECIAL(newval)) {
        if (NUMERIC_IS_PINF(newval))
            state->pInfcount++;
        else if (NUMERIC_IS_NINF(newval))
            state->nInfcount++;
        else
            state->NaNcount++;
        return;
    }

    init_var_from_num(newval, &X);

    if (X.dscale > state->maxScale) {
        state->maxScale = X.dscale;
        state->maxScaleCount = 1;
    } else if (X.dscale == state->maxScale)
        state->maxScaleCount++;

    /* if (state->calcSumX2) { */
    /*     init_var(&X2); */
    /*     mul_var(&X, &X, &X2, X.dscale * 2); */
    /* } */

    state->N++;

    accum_sum_add(&(state->sumX), &X);

    /* if (state->calcSumX2) accum_sum_add(&(state->sumX2), &X2); */
}

Datum numeric_avg_accum(PG_FUNCTION_ARGS)
{
    NumericAggState* state;
    Datum x = PG_GETARG_DATUM(1);
    Numeric nx = (Numeric)PG_DETOAST_DATUM(x);

    state = PG_ARGISNULL(0) ? NULL : (NumericAggState*)PG_GETARG_DATUM(0);

    if (state == NULL) state = makeNumericAggState(fcinfo, false);

    if (!PG_ARGISNULL(1)) do_numeric_accum(state, nx);

    if ((Datum)nx != x) free(nx);

    return (Datum)state;
}

Datum numeric_sum(PG_FUNCTION_ARGS)
{
    NumericAggState* state;
    NumericVar sumX_var;
    Numeric result;

    state = PG_ARGISNULL(0) ? NULL : (NumericAggState*)PG_GETARG_DATUM(0);

    if (state == NULL || NA_TOTAL_COUNT(state) == 0) PG_RETURN_NULL();

    if (state->NaNcount > 0) return (Datum)make_result(&const_nan);

    if (state->pInfcount > 0 && state->nInfcount > 0)
        return (Datum)make_result(&const_nan);
    if (state->pInfcount > 0) return (Datum)make_result(&const_pinf);
    if (state->nInfcount > 0) return (Datum)make_result(&const_ninf);

    init_var(&sumX_var);
    accum_sum_final(&state->sumX, &sumX_var);
    result = make_result(&sumX_var);
    free_var(&sumX_var);

    return (Datum)result;
}
