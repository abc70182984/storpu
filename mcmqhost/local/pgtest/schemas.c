#include "data_types.h"
#include "schemas.h"

#include <stdbool.h>

TupleDescData table_test_schema = {
    .natts = 3,
    .relpages = 10,
    .attrs =
        {
            {
                .attname = "id",
                .attnum = 1,
                .atttypmod = -1,
                .attlen = 4,
                .attcacheoff = -1,
                .attbyval = true,
                .attalign = 'i',
                .atttypid = &type_int4,
            },
            {
                .attname = "num",
                .attnum = 2,
                .atttypmod = -1,
                .attlen = 4,
                .attcacheoff = -1,
                .attbyval = true,
                .attalign = 'i',
                .atttypid = &type_int4,
            },
            {
                .attname = "data",
                .attnum = 3,
                .atttypmod = -1,
                .attlen = -1,
                .attcacheoff = -1,
                .attbyval = false,
                .attalign = 'i',
                .atttypid = &type_text,
            },
        },
};

TupleDescData index_test_schema = {
    .natts = 1,
    .relpages = 10,
    .attrs =
        {
            {
                .attname = "id",
                .attnum = 1,
                .atttypmod = -1,
                .attlen = 4,
                .attcacheoff = -1,
                .attbyval = true,
                .attalign = 'i',
                .atttypid = &type_int4,
            },
        },
};

#include "tpch_schemas.inc"

#include "tpcc_schemas.inc"
