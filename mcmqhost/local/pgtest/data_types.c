#include "data_types.h"
#include "fmgr.h"
#include "fmgrprotos.h"

#include <string.h>

static Datum F_int2_cmp(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);

    if ((int16_t)lhs > (int16_t)rhs)
        return 1;
    else if ((int16_t)lhs < (int16_t)rhs)
        return -1;
    return 0;
}

static Datum F_int4_cmp(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);

    if ((int32_t)lhs > (int32_t)rhs)
        return 1;
    else if ((int32_t)lhs < (int32_t)rhs)
        return -1;
    return 0;
}

static Datum F_int8_cmp(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);

    if ((int64_t)lhs > (int64_t)rhs)
        return 1;
    else if ((int64_t)lhs < (int64_t)rhs)
        return -1;
    return 0;
}

static Datum F_int4_gt(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);

    return (int32_t)lhs > (int32_t)rhs;
}

static Datum F_text_cmp(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);

    return strcmp(VARDATA_ANY(lhs), VARDATA_ANY(rhs));
}

FormData_pg_type type_int2 = {
    .typlen = 2,
    .typbyval = true,
    .cmp_proc = F_int2_cmp,
};

FormData_pg_type type_int4 = {
    .typlen = 4,
    .typbyval = true,
    .cmp_proc = F_int4_cmp,
};

FormData_pg_type type_int8 = {
    .typlen = 8,
    .typbyval = true,
    .cmp_proc = F_int8_cmp,
};

FormData_pg_type type_char = {
    .typlen = -1,
    .typbyval = false,
    .cmp_proc = F_text_cmp,
};

FormData_pg_type type_text = {
    .typlen = -1,
    .typbyval = false,
    .cmp_proc = F_text_cmp,
};

FormData_pg_type type_date = {
    .typlen = 4,
    .typbyval = true,
    .cmp_proc = F_int4_cmp,
};

FormData_pg_type type_decimal = {
    .typlen = -1,
    .typbyval = false,
    .cmp_proc = numeric_cmp,
};

FormData_pg_type type_timestamp = {
    .typlen = 8,
    .typbyval = true,
    .cmp_proc = F_int8_cmp,
};

FormData_pg_type type_internal = {
    .typlen = 8,
    .typbyval = true,
};
