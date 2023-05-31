#include "bufpage.h"
#include "relation.h"
#include "heap.h"
#include "tupdesc.h"
#include "buffer.h"
#include "index.h"
#include "fmgr.h"
#include "btree.h"
#include "catalog.h"
#include "aggregate.h"
#include "fmgrprotos.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Datum scan_func_int(PG_FUNCTION_ARGS)
{
    printf("Datum %d\n", (unsigned int)PG_GETARG_DATUM(0));
    return 1;
}

static Datum scan_func1(PG_FUNCTION_ARGS)
{
    char* str = text_to_cstring((text*)PG_GETARG_DATUM(0));
    printf("Datum %s\n", str);
    free(str);

    return 1;
}

static Datum scan_func2(PG_FUNCTION_ARGS)
{
    printf("Datum %d\n", VARSIZE_ANY_EXHDR(PG_GETARG_DATUM(0)));
    return 1;
}

static Datum F_int2_gt(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);

    return (int16_t)lhs > (int16_t)rhs;
}

static Datum F_int4_gt(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);

    return (int32_t)lhs > (int32_t)rhs;
}

static Datum F_int2_ge(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);

    return (int16_t)lhs >= (int16_t)rhs;
}

static Datum F_int4_ge(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);

    return (int32_t)lhs >= (int32_t)rhs;
}

int main()
{
    RelationData rfp;
    RelationData rind;

    bool do_scan = false;
    bool do_agg = false;
    bool do_index_lookup = true;

    if (do_scan) {
        rel_open_relation(&rfp, REL_OID_TABLE_TEST);
        rel_open_relation(&rind, REL_OID_INDEX_TEST);

        TableScanDesc scan;

        ScanKeyData skey[1];
        ScanKeyInit(&skey[0], 3, 0, scan_func1, (Datum)0);

        SnapshotData snapshot;

        scan = heap_beginscan(&rfp, &snapshot, 1, skey);
        heap_rescan(scan, NULL);

        for (int i = 0; i < 2; i++)
            heap_getnext(scan, ForwardScanDirection);

        heap_endscan(scan);
    }

    if (do_index_lookup) {
        rel_open_relation(&rfp, REL_OID_TABLE_TPCC_ORDERS);
        rel_open_relation(&rind, REL_OID_INDEX_TPCC_ORDERS);

        SnapshotData snapshot;
        IndexScanDesc scan_ind = index_beginscan(&rfp, &rind, &snapshot, 4);

        ScanKeyData skey[4];
        /* clang-format off */
        ScanKeyInit(&skey[0], 1, BTGreaterEqualStrategyNumber, F_int2_ge, (Datum)1);
        ScanKeyInit(&skey[1], 2, BTGreaterEqualStrategyNumber, F_int2_ge, (Datum)1);
        ScanKeyInit(&skey[2], 3, BTGreaterEqualStrategyNumber, F_int4_ge, (Datum)5);
        ScanKeyInit(&skey[3], 4, BTGreaterStrategyNumber, F_int4_gt, (Datum)0);
        /* clang-format on */

        index_rescan(scan_ind, skey, 4);

        for (int i = 0; i < 200; i++) {
            Datum values[8];
            bool isnull[8];

            HeapTuple htup = index_getnext_slot(scan_ind, ForwardScanDirection);
            printf("%p %d\n", htup->t_data, htup->t_len);
            heap_deform_tuple(htup, &tpcc_orders1_schema, values, isnull);
            printf("%ld %ld %ld %ld\n", values[0], values[1], values[2],
                   values[3]);
        }

        index_endscan(scan_ind);
    }

    if (do_agg) {
        rel_open_relation(&rfp, REL_OID_TABLE_TPCH_LINEITEM);

        TableScanDesc scan;

        ScanKeyData skey[1];
        ScanKeyInit(&skey[0], 6, 0, scan_func1, (Datum)20018);

        SnapshotData snapshot;

        /* scan = heap_beginscan(&rfp, &snapshot, 1, skey); */
        scan = heap_beginscan(&rfp, &snapshot, 0, NULL);
        heap_rescan(scan, NULL);

        AggState* agg;
        AggregateDescData agg_desc = {.agg_id = 2803, .attnum = 6};

        agg = agg_init(scan, &agg_desc, 1, (size_t)-1);

        TupleTableSlot* slot;

        slot = agg_getnext(agg);

        /* FmgrInfo f; */
        /* f.fn_addr = numeric_out; */
        /* Datum s = FunctionCall1Coll(&f, 0, slot->tts_values[0]); */
        /* printf("%s\n", (void*)s); */
        printf("%ld\n", (int64_t)slot->tts_values[0]);

        agg_end(agg);

        heap_endscan(scan);
    }

    return 0;
}
