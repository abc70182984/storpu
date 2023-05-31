#include "types.h"
#include "fmgr.h"

Datum int4larger(PG_FUNCTION_ARGS)
{
    int32_t arg1 = (int32_t)PG_GETARG_DATUM(0);
    int32_t arg2 = (int32_t)PG_GETARG_DATUM(1);

    return (Datum)((arg1 > arg2) ? arg1 : arg2);
}

Datum int4smaller(PG_FUNCTION_ARGS)
{
    int32_t arg1 = (int32_t)PG_GETARG_DATUM(0);
    int32_t arg2 = (int32_t)PG_GETARG_DATUM(1);

    return (Datum)((arg1 < arg2) ? arg1 : arg2);
}

Datum int4_sum(PG_FUNCTION_ARGS)
{
    int64_t newval;

    if (PG_ARGISNULL(0)) {
        if (PG_ARGISNULL(1)) PG_RETURN_NULL();
        newval = (int64_t)PG_GETARG_DATUM(1);
        return (Datum)newval;
    }

    int64_t oldsum = (int64_t)PG_GETARG_DATUM(0);

    if (PG_ARGISNULL(1)) return (Datum)oldsum;

    newval = oldsum + (int64_t)PG_GETARG_DATUM(1);

    return (Datum)newval;
}

Datum int8inc(PG_FUNCTION_ARGS)
{
    int64_t arg = (int64_t)PG_GETARG_DATUM(0);
    int64_t result;

    result = arg + 1;

    return (Datum)result;
}

Datum int8inc_any(PG_FUNCTION_ARGS) { return int8inc(fcinfo); }
