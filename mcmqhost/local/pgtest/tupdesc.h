#ifndef _TUPDESC_H_
#define _TUPDESC_H_

#include "postgres.h"
#include "types.h"
#include "data_types.h"

typedef struct FormData_pg_attribute {
    const char* attname;
    FormData_pg_type* atttypid;
    int16_t attlen;
    int16_t attnum;
    int32_t attcacheoff;
    int32_t atttypmod;
    bool attbyval;
    char attalign;
} FormData_pg_attribute;

typedef FormData_pg_attribute* Form_pg_attribute;

typedef struct TupleDescData {
    int natts;
    unsigned int relpages;
    FormData_pg_attribute attrs[];
} TupleDescData;
typedef struct TupleDescData* TupleDesc;

#define TupleDescAttr(tupdesc, i) (&(tupdesc)->attrs[(i)])

#define att_isnull(ATT, BITS) (!((BITS)[(ATT) >> 3] & (1 << ((ATT)&0x07))))

#define fetchatt(A, T) fetch_att(T, (A)->attbyval, (A)->attlen)

#define fetch_att(T, attbyval, attlen)                            \
    ((attbyval) ? ((attlen) == (int)sizeof(Datum)                 \
                       ? *((Datum*)(T))                           \
                       : ((attlen) == (int)sizeof(int32_t)        \
                              ? (Datum)(*((int32_t*)(T)))         \
                              : ((attlen) == (int)sizeof(int16_t) \
                                     ? (Datum)(*((int16_t*)(T)))  \
                                     : (Datum)(*((char*)(T))))))  \
                : (Datum)((char*)(T)))

#define att_addlength_pointer(cur_offset, attlen, attptr)           \
    (((attlen) > 0)                                                 \
         ? ((cur_offset) + (attlen))                                \
         : (((attlen) == -1) ? ((cur_offset) + VARSIZE_ANY(attptr)) \
                             : (cur_offset) + (strlen((char*)(attptr)) + 1)))

#define att_addlength_datum(cur_offset, attlen, attdatum) \
    att_addlength_pointer(cur_offset, attlen, (void*)(attdatum))

#define att_align_pointer(cur_offset, attalign, attlen, attptr) \
    (((attlen) == -1 && VARATT_NOT_PAD_BYTE(attptr))            \
         ? (uintptr_t)(cur_offset)                              \
         : att_align_nominal(cur_offset, attalign))

#define att_align_nominal(cur_offset, attalign)                  \
    (((attalign) == 'i')                                         \
         ? INTALIGN(cur_offset)                                  \
         : (((attalign) == 'c')                                  \
                ? (uintptr_t)(cur_offset)                        \
                : (((attalign) == 'd') ? DOUBLEALIGN(cur_offset) \
                                       : SHORTALIGN(cur_offset))))

#endif
