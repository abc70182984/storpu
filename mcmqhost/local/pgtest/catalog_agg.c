#include "catalog.h"
#include "fmgrprotos.h"

static FormData_pg_aggregate pg_aggregate[] = {
    {
        /* sum(int4) */
        .aggfnoid = 2108,
        .aggtransfn = int4_sum,
        .aggcombinefn = int4_sum,
        .aggtranstype = &type_int4,
        .aggrestype = &type_int8,
    },

    {
        /* sum(numeric) */
        .aggfnoid = 2114,
        .aggtransfn = numeric_avg_accum,
        .aggfinalfn = numeric_sum,
        .aggtranstype = &type_internal,
        .aggrestype = &type_decimal,
    },

    {
        /* max(int4) */
        .aggfnoid = 2116,
        .aggtransfn = int4larger,
        .aggtransfnstrict = true,
        .aggcombinefn = int4larger,
        .aggtranstype = &type_int4,
        .aggrestype = &type_int4,
    },

    {
        /* max(numeric) */
        .aggfnoid = 2130,
        .aggtransfn = numeric_larger,
        .aggtransfnstrict = true,
        .aggcombinefn = numeric_larger,
        .aggtranstype = &type_decimal,
        .aggrestype = &type_decimal,
    },

    {
        /* min(int4) */
        .aggfnoid = 2132,
        .aggtransfn = int4smaller,
        .aggtransfnstrict = true,
        .aggcombinefn = int4smaller,
        .aggtranstype = &type_int4,
        .aggrestype = &type_int4,
    },

    {
        /* min(numeric) */
        .aggfnoid = 2146,
        .aggtransfn = numeric_smaller,
        .aggtransfnstrict = true,
        .aggcombinefn = numeric_smaller,
        .aggtranstype = &type_decimal,
        .aggrestype = &type_decimal,
    },

    {
        /* count(any) */
        .aggfnoid = 2147,
        .aggtransfn = int8inc_any,
        .aggtransfnstrict = true,
        .aggtranstype = &type_int8,
        .aggrestype = &type_int8,
        .agginitval = "0",
    },

    {
        /* count(*) */
        .aggfnoid = 2803,
        .aggtransfn = int8inc,
        .aggtranstype = &type_int8,
        .aggrestype = &type_int8,
        .agginitval = "0",
    },
};

Form_pg_aggregate catalog_get_aggregate(Oid id)
{
    int i;

    for (i = 0; i < sizeof(pg_aggregate) / sizeof(pg_aggregate[0]); i++) {
        Form_pg_aggregate agg = &pg_aggregate[i];

        if (agg->aggfnoid == id) return agg;
    }

    return NULL;
}
