#ifndef _CATALOG_DATA_TYPE_H_
#define _CATALOG_DATA_TYPE_H_

#include "postgres.h"
#include "types.h"
#include "fmgr.h"

typedef struct FormData_pg_type {
    int16_t typlen;
    bool typbyval;
    PGFunction cmp_proc;
} FormData_pg_type;

typedef FormData_pg_type* Form_pg_type;

extern FormData_pg_type type_int2;
extern FormData_pg_type type_int4;
extern FormData_pg_type type_int8;
extern FormData_pg_type type_char;
extern FormData_pg_type type_text;
extern FormData_pg_type type_date;
extern FormData_pg_type type_decimal;
extern FormData_pg_type type_timestamp;
extern FormData_pg_type type_internal;

typedef struct varlena text;

struct NumericData;
typedef struct NumericData* Numeric;

__BEGIN_DECLS

char* text_to_cstring(const text* t);

Numeric int64_to_numeric(int64_t val);

__END_DECLS

#endif
