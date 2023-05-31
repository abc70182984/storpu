#ifndef _AGGREGATE_H_
#define _AGGREGATE_H_

#include "types.h"
#include "relation.h"
#include "heap.h"
#include "fmgr.h"
#include "tuptable.h"

typedef struct {
    AttrNumber attnum;
    Oid agg_id;
} AggregateDescData;

typedef AggregateDescData* AggregateDesc;

typedef struct AggStatePerTransData {
    int numTransInputs;
    uint16_t input_attnum;

    FmgrInfo transfn;
    FmgrInfo serialfn;
    FmgrInfo deserialfn;

    Form_pg_type aggtranstype;

    Datum initValue;
    bool initValueIsNull;

    int16_t transtypeLen;
    bool transtypeByVal;

    FunctionCallInfo transfn_fcinfo;
} AggStatePerTransData;

typedef struct AggStatePerAggData {
    int numFinalArgs;
    int transno;

    PGFunction finalfn_ptr;

    FmgrInfo finalfn;

    int16_t resulttypeLen;
    bool resulttypeByVal;
} AggStatePerAggData;

typedef struct AggStatePerGroupData {
    Datum transValue; /* current transition value */
    bool transValueIsNull;
    bool noTransValue; /* true if transValue not set yet */
} AggStatePerGroupData;

typedef AggStatePerTransData* AggStatePerTrans;
typedef AggStatePerAggData* AggStatePerAgg;
typedef AggStatePerGroupData* AggStatePerGroup;

typedef struct AggState {
    TableScanDesc scan;
    int numaggs;
    int numtrans;
    size_t group_size;
    AggStatePerAgg peragg;
    AggStatePerTrans pertrans;
    AggStatePerGroup pergroups;
    bool input_done;
    bool agg_done;
    HeapTuple grp_firstTuple;

    AttrNumber max_attnum;
    Datum* values;
    bool* is_null;

    Datum* aggvalues;
    bool* aggnulls;

    TupleDesc ResultTupleDesc;
    TupleTableSlot* ResultTupleSlot;
} AggState;

__BEGIN_DECLS

AggState* agg_init(TableScanDesc scan, AggregateDesc aggs, int num_aggs,
                   size_t group_size);
TupleTableSlot* agg_getnext(AggState* aggstate);
void agg_end(AggState* aggstate);

__END_DECLS

#endif
