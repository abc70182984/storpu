#include "index.h"
#include "btree.h"
#include "heap.h"

#include <stddef.h>
#include <string.h>
#include <stdlib.h>

Datum nocache_index_getattr(IndexTuple tup, int attnum, TupleDesc tupleDesc)
{
    char* tp;           /* ptr to data part of tuple */
    uint8_t* bp = NULL; /* ptr to null bitmap in tuple */
    bool slow = false;  /* do we have to walk attrs? */
    int data_off;       /* tuple data offset */
    int off;            /* current offset within data */

    data_off = IndexInfoFindDataOffset(tup->t_info);

    attnum--;

    if (IndexTupleHasNulls(tup)) {
        bp = (uint8_t*)((char*)tup + sizeof(IndexTupleData));

        {
            int byte = attnum >> 3;
            int finalbit = attnum & 0x07;

            if ((~bp[byte]) & ((1 << finalbit) - 1))
                slow = true;
            else {
                int i;

                for (i = 0; i < byte; i++) {
                    if (bp[i] != 0xFF) {
                        slow = true;
                        break;
                    }
                }
            }
        }
    }

    tp = (char*)tup + data_off;

    if (!slow) {
        Form_pg_attribute att;

        att = TupleDescAttr(tupleDesc, attnum);
        if (att->attcacheoff >= 0) return fetchatt(att, tp + att->attcacheoff);

        if (IndexTupleHasVarwidths(tup)) {
            int j;

            for (j = 0; j <= attnum; j++) {
                if (TupleDescAttr(tupleDesc, j)->attlen <= 0) {
                    slow = true;
                    break;
                }
            }
        }
    }

    if (!slow) {
        int natts = tupleDesc->natts;
        int j = 1;

        TupleDescAttr(tupleDesc, 0)->attcacheoff = 0;

        while (j < natts && TupleDescAttr(tupleDesc, j)->attcacheoff > 0)
            j++;

        off = TupleDescAttr(tupleDesc, j - 1)->attcacheoff +
              TupleDescAttr(tupleDesc, j - 1)->attlen;

        for (; j < natts; j++) {
            Form_pg_attribute att = TupleDescAttr(tupleDesc, j);

            if (att->attlen <= 0) break;

            off = att_align_nominal(off, att->attalign);

            att->attcacheoff = off;

            off += att->attlen;
        }

        off = TupleDescAttr(tupleDesc, attnum)->attcacheoff;
    } else {
        bool usecache = true;
        int i;

        off = 0;
        for (i = 0;; i++) /* loop exit is at "break" */
        {
            Form_pg_attribute att = TupleDescAttr(tupleDesc, i);

            if (IndexTupleHasNulls(tup) && att_isnull(i, bp)) {
                usecache = false;
                continue; /* this cannot be the target att */
            }

            if (usecache && att->attcacheoff >= 0)
                off = att->attcacheoff;
            else if (att->attlen == -1) {
                if (usecache && off == att_align_nominal(off, att->attalign))
                    att->attcacheoff = off;
                else {
                    off = att_align_pointer(off, att->attalign, -1, tp + off);
                    usecache = false;
                }
            } else {
                off = att_align_nominal(off, att->attalign);

                if (usecache) att->attcacheoff = off;
            }

            if (i == attnum) break;

            off = att_addlength_pointer(off, att->attlen, tp + off);

            if (usecache && att->attlen <= 0) usecache = false;
        }
    }

    return fetchatt(TupleDescAttr(tupleDesc, attnum), tp + off);
}

IndexScanDesc RelationGetIndexSacn(Relation indexRelation, int nkeys)
{
    IndexScanDesc scan;

    scan = (IndexScanDesc)malloc(sizeof(IndexScanDescData));

    scan->heapRelation = NULL;
    scan->indexRelation = indexRelation;
    scan->xs_heapfetch = NULL;
    scan->numberOfKeys = nkeys;

    if (nkeys > 0)
        scan->keyData = (ScanKey)malloc(sizeof(ScanKeyData) * nkeys);
    else
        scan->keyData = NULL;

    scan->opaque = NULL;

    scan->xs_itup = NULL;
    scan->xs_itupdesc = NULL;

    return scan;
}

void IndexScanEnd(IndexScanDesc scan)
{
    if (scan->keyData) free(scan->keyData);
    free(scan);
}

IndexScanDesc index_beginscan(Relation heapRelation, Relation indexRelation,
                              Snapshot snapshot, int nkeys)
{
    IndexScanDesc scan;

    scan = btbeginscan(indexRelation, nkeys);

    scan->heapRelation = heapRelation;
    scan->xs_snapshot = snapshot;

    scan->xs_heapfetch = heapam_index_fetch_begin(heapRelation);

    return scan;
}

void index_rescan(IndexScanDesc scan, ScanKey scankey, int nkeys)
{
    if (scan->xs_heapfetch) heapam_index_fetch_reset(scan->xs_heapfetch);

    scan->xs_heap_continue = false;

    btrescan(scan, scankey, nkeys);
}

void index_endscan(IndexScanDesc scan)
{
    if (scan->xs_heapfetch) {
        heapam_index_fetch_end(scan->xs_heapfetch);
        scan->xs_heapfetch = NULL;
    }

    btendscan(scan);

    IndexScanEnd(scan);
}

ItemPointer index_getnext_tid(IndexScanDesc scan, ScanDirection direction)
{
    bool found;

    found = btgettuple(scan, direction);

    scan->xs_heap_continue = false;

    if (!found) {
        if (scan->xs_heapfetch) heapam_index_fetch_reset(scan->xs_heapfetch);

        return NULL;
    }

    return &scan->xs_heaptid;
}

HeapTuple index_getnext_slot(IndexScanDesc scan, ScanDirection direction)
{
    for (;;) {
        HeapTuple htup;

        if (!scan->xs_heap_continue) {
            ItemPointer tid;

            tid = index_getnext_tid(scan, direction);

            if (!tid) break;
        }

        htup = heapam_index_fetch_tuple(scan->xs_heapfetch, &scan->xs_heaptid,
                                        scan->xs_snapshot,
                                        &scan->xs_heap_continue);

        if (htup) return htup;
    }

    return NULL;
}
