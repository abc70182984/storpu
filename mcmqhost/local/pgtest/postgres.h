#ifndef _POSTGRES_H_
#define _POSTGRES_H_

#include <stdint.h>
#include <stddef.h>

#define TYPEALIGN(ALIGNVAL, LEN) \
    (((uintptr_t)(LEN) + ((ALIGNVAL)-1)) & ~((uintptr_t)((ALIGNVAL)-1)))

#define SHORTALIGN(LEN)  TYPEALIGN(2, (LEN))
#define INTALIGN(LEN)    TYPEALIGN(4, (LEN))
#define LONGALIGN(LEN)   TYPEALIGN(8, (LEN))
#define DOUBLEALIGN(LEN) TYPEALIGN(8, (LEN))
#define MAXALIGN(LEN)    TYPEALIGN(8, (LEN))

typedef union {
    struct /* Normal varlena (4-byte length) */
    {
        uint32_t va_header;
        char va_data[];
    } va_4byte;
    struct /* Compressed-in-line format */
    {
        uint32_t va_header;
        uint32_t va_tcinfo; /* Original data size (excludes header) and
                             * compression method; see va_extinfo */
        char va_data[];     /* Compressed data */
    } va_compressed;
} varattrib_4b;

typedef struct {
    uint8_t va_header;
    char va_data[]; /* Data begins here */
} varattrib_1b;

/* TOAST pointers are a subset of varattrib_1b with an identifying tag byte */
typedef struct {
    uint8_t va_header; /* Always 0x80 or 0x01 */
    uint8_t va_tag;    /* Type of datum */
    char va_data[];    /* Type-specific data */
} varattrib_1b_e;

#define VARATT_IS_4B(PTR)         ((((varattrib_1b*)(PTR))->va_header & 0x01) == 0x00)
#define VARATT_IS_4B_U(PTR)       ((((varattrib_1b*)(PTR))->va_header & 0x03) == 0x00)
#define VARATT_IS_4B_C(PTR)       ((((varattrib_1b*)(PTR))->va_header & 0x03) == 0x02)
#define VARATT_IS_1B(PTR)         ((((varattrib_1b*)(PTR))->va_header & 0x01) == 0x01)
#define VARATT_IS_1B_E(PTR)       ((((varattrib_1b*)(PTR))->va_header) == 0x01)
#define VARATT_NOT_PAD_BYTE(PTR)  (*((uint8_t*)(PTR)) != 0)
#define VARATT_IS_COMPRESSED(PTR) VARATT_IS_4B_C(PTR)
#define VARATT_IS_EXTERNAL(PTR)   VARATT_IS_1B_E(PTR)
#define VARATT_IS_SHORT(PTR)      VARATT_IS_1B(PTR)
#define VARATT_IS_EXTENDED(PTR)   (!VARATT_IS_4B_U(PTR))

#define SET_VARSIZE_4B(PTR, len) \
    (((varattrib_4b*)(PTR))->va_4byte.va_header = (((uint32_t)(len)) << 2))
#define SET_VARSIZE_4B_C(PTR, len)                \
    (((varattrib_4b*)(PTR))->va_4byte.va_header = \
         (((uint32_t)(len)) << 2) | 0x02)
#define SET_VARSIZE_1B(PTR, len) \
    (((varattrib_1b*)(PTR))->va_header = (((uint8_t)(len)) << 1) | 0x01)
#define SET_VARTAG_1B_E(PTR, tag)                \
    (((varattrib_1b_e*)(PTR))->va_header = 0x01, \
     ((varattrib_1b_e*)(PTR))->va_tag = (tag))

#define VARHDRSZ            sizeof(uint32_t)
#define VARHDRSZ_EXTERNAL   offsetof(varattrib_1b_e, va_data)
#define VARHDRSZ_COMPRESSED offsetof(varattrib_4b, va_compressed.va_data)
#define VARHDRSZ_SHORT      offsetof(varattrib_1b, va_data)

#define VARSIZE_4B(PTR) \
    ((((varattrib_4b*)(PTR))->va_4byte.va_header >> 2) & 0x3FFFFFFF)
#define VARSIZE_1B(PTR)  ((((varattrib_1b*)(PTR))->va_header >> 1) & 0x7F)
#define VARTAG_1B_E(PTR) (((varattrib_1b_e*)(PTR))->va_tag)

#define VARDATA_4B(PTR)   (((varattrib_4b*)(PTR))->va_4byte.va_data)
#define VARDATA_4B_C(PTR) (((varattrib_4b*)(PTR))->va_compressed.va_data)
#define VARDATA_1B(PTR)   (((varattrib_1b*)(PTR))->va_data)
#define VARDATA_1B_E(PTR) (((varattrib_1b_e*)(PTR))->va_data)

#define VARDATA(PTR) VARDATA_4B(PTR)
#define VARSIZE(PTR) VARSIZE_4B(PTR)

#define VARSIZE_SHORT(PTR) VARSIZE_1B(PTR)
#define VARDATA_SHORT(PTR) VARDATA_1B(PTR)

#define VARSIZE_EXTERNAL(PTR) 0

#define VARSIZE_ANY(PTR)         \
    (VARATT_IS_1B_E(PTR)         \
         ? VARSIZE_EXTERNAL(PTR) \
         : (VARATT_IS_1B(PTR) ? VARSIZE_1B(PTR) : VARSIZE_4B(PTR)))

#define VARSIZE_ANY_EXHDR(PTR)                                   \
    (VARATT_IS_1B_E(PTR)                                         \
         ? VARSIZE_EXTERNAL(PTR) - VARHDRSZ_EXTERNAL             \
         : (VARATT_IS_1B(PTR) ? VARSIZE_1B(PTR) - VARHDRSZ_SHORT \
                              : VARSIZE_4B(PTR) - VARHDRSZ))

#define SET_VARSIZE(PTR, len)            SET_VARSIZE_4B(PTR, len)
#define SET_VARSIZE_SHORT(PTR, len)      SET_VARSIZE_1B(PTR, len)
#define SET_VARSIZE_COMPRESSED(PTR, len) SET_VARSIZE_4B_C(PTR, len)

#define VARDATA_ANY(PTR) (VARATT_IS_1B(PTR) ? VARDATA_1B(PTR) : VARDATA_4B(PTR))

#define C_COLLATION_OID 950

__BEGIN_DECLS

struct varlena* pg_detoast_datum(struct varlena* datum);
#define PG_DETOAST_DATUM(datum) pg_detoast_datum((struct varlena*)(datum))

__END_DECLS

#endif
