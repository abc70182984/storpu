#include "catalog.h"
#include "aggregate.h"

#include <stdlib.h>
#include <string.h>

static void build_pertrans_for_aggref(AggStatePerTrans pertrans,
                                      PGFunction transfn, bool transfn_strict,
                                      Form_pg_type aggtranstype,
                                      Datum initValue, bool initValueIsNull,
                                      AttrNumber input_attnum)
{
    int numTransInputs = 1;
    int numTransArgs = numTransInputs + 1;

    pertrans->numTransInputs = numTransInputs;
    pertrans->input_attnum = input_attnum;
    pertrans->initValue = initValue;
    pertrans->initValueIsNull = initValueIsNull;

    pertrans->aggtranstype = aggtranstype;

    pertrans->transfn.fn_addr = transfn;
    pertrans->transfn.fn_strict = transfn_strict;

    pertrans->transfn_fcinfo =
        (FunctionCallInfo)malloc(SizeForFunctionCallInfo(numTransArgs));
    InitFunctionCallInfoData(*pertrans->transfn_fcinfo, &pertrans->transfn,
                             numTransArgs, 0, NULL, NULL);

    pertrans->transtypeLen = aggtranstype->typlen;
    pertrans->transtypeByVal = aggtranstype->typbyval;
}

AggState* agg_init(TableScanDesc scan, AggregateDesc aggs, int num_aggs,
                   size_t group_size)
{
    AggState* aggstate;
    AggStatePerAgg peraggs;
    AggStatePerTrans pertransstate;
    AggStatePerGroup pergroups;
    int num_trans = num_aggs;
    AttrNumber max_attnum;
    int i;

    aggstate = malloc(sizeof(AggState));
    memset(aggstate, 0, sizeof(*aggstate));
    aggstate->scan = scan;
    aggstate->numtrans = num_trans;
    aggstate->numaggs = num_aggs;
    aggstate->group_size = group_size;

    peraggs = (AggStatePerAgg)malloc(sizeof(AggStatePerAggData) * num_aggs);
    memset(peraggs, 0, sizeof(AggStatePerAggData) * num_aggs);
    pertransstate =
        (AggStatePerTrans)malloc(sizeof(AggStatePerTransData) * num_trans);
    memset(pertransstate, 0, sizeof(AggStatePerTransData) * num_trans);

    aggstate->peragg = peraggs;
    aggstate->pertrans = pertransstate;

    for (i = 0; i < num_trans; i++) {
        AggStatePerAgg peragg = &peraggs[i];
        AggStatePerTrans pertrans = &pertransstate[i];
        Form_pg_aggregate aggTuple;
        Form_pg_type aggtranstype;
        PGFunction transfn;
        bool transfn_strict;
        Datum initValue;
        bool initValueIsNull;

        aggTuple = catalog_get_aggregate(aggs[i].agg_id);
        if (!aggTuple) goto error;

        aggtranstype = aggTuple->aggtranstype;

        peragg->transno = i;
        peragg->numFinalArgs = 1;

        peragg->finalfn_ptr = aggTuple->aggfinalfn;

        if (aggTuple->aggfinalfn) {
            peragg->finalfn.fn_addr = aggTuple->aggfinalfn;
        }

        peragg->resulttypeByVal = aggTuple->aggrestype->typbyval;
        peragg->resulttypeLen = aggTuple->aggrestype->typlen;

        transfn = aggTuple->aggtransfn;
        transfn_strict = aggTuple->aggtransfnstrict;

        initValue = 0;
        initValueIsNull = aggTuple->agginitval == NULL;

        build_pertrans_for_aggref(pertrans, transfn, transfn_strict,
                                  aggtranstype, initValue, initValueIsNull,
                                  aggs[i].attnum);
    }

    pergroups =
        (AggStatePerGroup)malloc(sizeof(AggStatePerGroupData) * num_aggs);
    aggstate->pergroups = pergroups;

    max_attnum = scan->rs_rd->rd_att->natts;

    aggstate->max_attnum = max_attnum;
    aggstate->values = malloc(sizeof(Datum) * max_attnum);
    aggstate->is_null = malloc(sizeof(bool) * max_attnum);

    aggstate->aggvalues = malloc(sizeof(Datum) * num_aggs);
    aggstate->aggnulls = malloc(sizeof(bool) * num_aggs);

    aggstate->ResultTupleDesc =
        (TupleDesc)malloc(offsetof(struct TupleDescData, attrs) +
                          num_aggs * sizeof(FormData_pg_attribute));
    aggstate->ResultTupleDesc->natts = num_aggs;

    for (i = 0; i < num_aggs; i++) {
        Form_pg_attribute attr = &aggstate->ResultTupleDesc->attrs[i];
        attr->attbyval = peraggs[i].resulttypeByVal;
        attr->attlen = peraggs[i].resulttypeLen;
    }

    aggstate->ResultTupleSlot =
        MakeTupleTableSlot(aggstate->ResultTupleDesc, &TTSOpsVirtual);

    return aggstate;

error:
    free(peraggs);
    free(pertransstate);
    free(aggstate);

    return NULL;
}

static void initialize_aggregate(AggState* aggstate, AggStatePerTrans pertrans,
                                 AggStatePerGroup pergroupstate)
{
    if (pertrans->initValueIsNull)
        pergroupstate->transValue = pertrans->initValue;
    else
        pergroupstate->transValue =
            datumCopy(pertrans->initValue, pertrans->transtypeByVal,
                      pertrans->transtypeLen);

    pergroupstate->transValueIsNull = pertrans->initValueIsNull;
    pergroupstate->noTransValue = pertrans->initValueIsNull;
}

static void initialize_aggregates(AggState* aggstate,
                                  AggStatePerGroup pergroups)
{
    int transno;
    int numTrans = aggstate->numtrans;
    AggStatePerTrans transstates = aggstate->pertrans;

    for (transno = 0; transno < numTrans; transno++) {
        AggStatePerTrans pertrans = &transstates[transno];
        AggStatePerGroup pergroupstate = &pergroups[transno];

        initialize_aggregate(aggstate, pertrans, pergroupstate);
    }
}

static void advance_transition_function(AggState* aggstate,
                                        AggStatePerTrans pertrans,
                                        AggStatePerGroup pergroupstate)
{
    FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
    Datum newVal;

    fcinfo->args[1].value = aggstate->values[pertrans->input_attnum - 1];
    fcinfo->args[1].isnull = aggstate->is_null[pertrans->input_attnum - 1];

    if (pertrans->transfn.fn_strict) {
        int numTransInputs = pertrans->numTransInputs;
        int i;

        for (i = 1; i <= numTransInputs; i++) {
            if (fcinfo->args[i].isnull) return;
        }

        if (pergroupstate->noTransValue) {
            pergroupstate->transValue =
                datumCopy(fcinfo->args[1].value, pertrans->transtypeByVal,
                          pertrans->transtypeLen);
            pergroupstate->transValueIsNull = false;
            pergroupstate->noTransValue = false;
            return;
        }

        if (pergroupstate->transValueIsNull) {
            return;
        }
    }

    fcinfo->args[0].value = pergroupstate->transValue;
    fcinfo->args[0].isnull = pergroupstate->transValueIsNull;
    fcinfo->isnull = false;

    newVal = FunctionCallInvoke(fcinfo);

    if (!pertrans->transtypeByVal && newVal != pergroupstate->transValue) {
        if (!pergroupstate->transValueIsNull)
            free((void*)pergroupstate->transValue);
    }

    pergroupstate->transValue = newVal;
    pergroupstate->transValueIsNull = fcinfo->isnull;
}

static void advance_aggregates(AggState* aggstate)
{
    int transno;
    int numTrans = aggstate->numtrans;
    AggStatePerTrans transstates = aggstate->pertrans;
    AggStatePerGroup pergroups = aggstate->pergroups;

    for (transno = 0; transno < numTrans; transno++) {
        AggStatePerTrans pertrans = &transstates[transno];
        AggStatePerGroup pergroupstate = &pergroups[transno];

        advance_transition_function(aggstate, pertrans, pergroupstate);
    }
}

static void finalize_aggregate(AggState* aggstate, AggStatePerAgg peragg,
                               AggStatePerTrans pertrans,
                               AggStatePerGroup pergroupstate, Datum* resultVal,
                               bool* resultIsNull)
{
    LOCAL_FCINFO(fcinfo, 1);

    if (peragg->finalfn_ptr) {
        int numFinalArgs = peragg->numFinalArgs;

        InitFunctionCallInfoData(*fcinfo, &peragg->finalfn, numFinalArgs, 0,
                                 (void*)aggstate, NULL);

        fcinfo->args[0].value = pergroupstate->transValue;
        fcinfo->args[0].isnull = pergroupstate->transValueIsNull;

        *resultVal = FunctionCallInvoke(fcinfo);
        *resultIsNull = fcinfo->isnull;

        if (pertrans->transtypeByVal && !pergroupstate->transValueIsNull)
            free((void*)pergroupstate->transValue);
    } else {
        *resultVal = pergroupstate->transValue;
        *resultIsNull = pergroupstate->transValueIsNull;
    }

    pergroupstate->transValueIsNull = true;
}

static void finalize_aggregates(AggState* aggstate, AggStatePerAgg peraggs,
                                AggStatePerTrans pertrans,
                                AggStatePerGroup pergroup)
{
    Datum* aggvalues = aggstate->aggvalues;
    bool* aggnulls = aggstate->aggnulls;
    int aggno;
    int transno;

    for (aggno = 0; aggno < aggstate->numaggs; aggno++) {
        AggStatePerAgg peragg = &peraggs[aggno];
        int transno = peragg->transno;
        AggStatePerGroup pergroupstate;

        pergroupstate = &pergroup[transno];

        finalize_aggregate(aggstate, peragg, pertrans, pergroupstate,
                           &aggvalues[aggno], &aggnulls[aggno]);
    }
}

TupleTableSlot* project_aggregates(AggState* aggstate)
{
    int i;

    ExecClearTuple(aggstate->ResultTupleSlot);

    for (i = 0; i < aggstate->numaggs; i++) {
        aggstate->ResultTupleSlot->tts_values[i] = aggstate->aggvalues[i];
        aggstate->ResultTupleSlot->tts_isnull[i] = aggstate->aggnulls[i];
    }

    ExecMaterializeSlot(aggstate->ResultTupleSlot);

    for (i = 0; i < aggstate->numaggs; i++) {
        AggStatePerAgg peragg = &aggstate->peragg[i];

        if (!peragg->resulttypeByVal && !aggstate->aggnulls[i])
            free((void*)aggstate->aggvalues[i]);
    }

    return aggstate->ResultTupleSlot;
}

TupleTableSlot* agg_getnext(AggState* aggstate)
{
    AggStatePerAgg peragg;
    AggStatePerGroup pergroups;
    TupleTableSlot* result;

    peragg = aggstate->peragg;
    pergroups = aggstate->pergroups;

    if (aggstate->agg_done) return NULL;

    if (aggstate->grp_firstTuple == NULL) {
        HeapTuple htup = heap_getnext(aggstate->scan, ForwardScanDirection);

        if (htup) {
            aggstate->grp_firstTuple = heap_copytuple(htup);
        } else {
            aggstate->agg_done = true;
            return NULL;
        }
    }

    initialize_aggregates(aggstate, pergroups);

    if (aggstate->grp_firstTuple) {
        heap_deform_tuple(aggstate->grp_firstTuple,
                          aggstate->scan->rs_rd->rd_att, aggstate->values,
                          aggstate->is_null);

        int i;
        for (i = 0; i < aggstate->group_size; i++) {
            HeapTuple htup;

            advance_aggregates(aggstate);

            if (aggstate->grp_firstTuple) {
                free(aggstate->grp_firstTuple);
                aggstate->grp_firstTuple = NULL;
            }

            htup = heap_getnext(aggstate->scan, ForwardScanDirection);
            if (!htup) {
                aggstate->agg_done = true;
                break;
            }

            heap_deform_tuple(htup, aggstate->scan->rs_rd->rd_att,
                              aggstate->values, aggstate->is_null);
        }

        finalize_aggregates(aggstate, peragg, aggstate->pertrans,
                            aggstate->pergroups);

        result = project_aggregates(aggstate);
        if (result) return result;
    }

    return NULL;
}

void agg_end(AggState* aggstate)
{
    int i;

    for (i = 0; i < aggstate->numtrans; i++) {
        AggStatePerTrans pertrans = &aggstate->pertrans[i];
        AggStatePerGroup pergroup = &aggstate->pergroups[i];

        if (pertrans->transtypeByVal && !pergroup->transValueIsNull)
            free((void*)pergroup->transValue);
        if (pertrans->transtypeByVal && !pertrans->initValueIsNull)
            free((void*)pertrans->initValue);

        free(pertrans->transfn_fcinfo);
    }

    free(aggstate->peragg);
    free(aggstate->pertrans);
    free(aggstate->pergroups);

    if (aggstate->grp_firstTuple) free(aggstate->grp_firstTuple);

    free(aggstate->values);
    free(aggstate->is_null);

    free(aggstate->aggvalues);
    free(aggstate->aggnulls);

    free(aggstate->ResultTupleDesc);
    ExecDropSingleTupleTableSlot(aggstate->ResultTupleSlot);
}
