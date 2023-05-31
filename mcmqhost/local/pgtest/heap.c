#include "config.h"
#include "heap.h"
#include "relation.h"
#include "bufpage.h"
#include "tupdesc.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static void initscan(HeapScanDesc scan, ScanKey key, int keep_startblock)
{
    scan->rs_startblock = 0;
    scan->rs_nblocks = scan->rs_base.rs_rd->rd_att->relpages;

    scan->rs_numblocks = InvalidBlockNumber;
    scan->rs_inited = false;
    scan->rs_ctup.t_data = 0;
    scan->rs_cblock = InvalidBlockNumber;
    scan->rs_cbuf = InvalidBuffer;

    if (key != NULL && scan->rs_base.rs_nkeys > 0)
        memcpy(scan->rs_base.rs_key, key,
               scan->rs_base.rs_nkeys * sizeof(ScanKeyData));
}

TableScanDesc heap_beginscan(Relation relation, Snapshot snapshot, int nkeys,
                             ScanKey key)
{
    HeapScanDesc scan;

    scan = (HeapScanDesc)malloc(sizeof(HeapScanDescData));

    scan->rs_base.rs_rd = relation;
    scan->rs_base.rs_snapshot = snapshot;
    scan->rs_base.rs_nkeys = nkeys;
    scan->rs_base.rs_flags = 0;

    scan->rs_cbufdat.blkno = InvalidBlockNumber;
    scan->rs_cbufdat.bufpage = bufpage_alloc();

    if (nkeys > 0)
        scan->rs_base.rs_key = (ScanKey)malloc(sizeof(ScanKeyData) * nkeys);
    else
        scan->rs_base.rs_key = NULL;

    initscan(scan, key, 0);

    return (TableScanDesc)scan;
}

void heap_rescan(TableScanDesc sscan, ScanKey key)
{
    HeapScanDesc scan = (HeapScanDesc)sscan;

    initscan(scan, key, 1);
}

void heap_endscan(TableScanDesc sscan)
{
    HeapScanDesc scan = (HeapScanDesc)sscan;

    if (BufferIsValid(scan->rs_cbuf)) ReleaseBuffer(scan->rs_cbuf);

    if (scan->rs_cbufdat.bufpage) bufpage_free(scan->rs_cbufdat.bufpage);

    if (scan->rs_base.rs_key) free(scan->rs_base.rs_key);

    free(scan);
}

void heapgetpage(TableScanDesc sscan, BlockNumber page)
{
    HeapScanDesc scan = (HeapScanDesc)sscan;

    if (BufferIsValid(scan->rs_cbuf)) {
        ReleaseBuffer(scan->rs_cbuf);
        scan->rs_cbuf = InvalidBuffer;
    }

    scan->rs_cbuf =
        rel_read_buffer(scan->rs_base.rs_rd, page, &scan->rs_cbufdat);
    scan->rs_cblock = page;
}

static Datum nocachegetattr(HeapTuple tuple, int attnum, TupleDesc tupdesc)
{
    HeapTupleHeader tup = tuple->t_data;
    char* tp;
    bool slow = false;
    int off;
    uint8_t* bp = tup->t_bits;

    attnum--;

    if (!HeapTupleNoNulls(tuple)) {
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

    tp = (char*)tup + tup->t_hoff;

    if (!slow) {
        Form_pg_attribute att = TupleDescAttr(tupdesc, attnum);
        if (att->attcacheoff >= 0) return fetchatt(att, tp + att->attcacheoff);

        if (HeapTupleHasVarWidth(tuple)) {
            int j;

            for (j = 0; j <= attnum; j++) {
                if (TupleDescAttr(tupdesc, j)->attlen <= 0) {
                    slow = true;
                    break;
                }
            }
        }
    }

    if (!slow) {
        int natts = tupdesc->natts;
        int j = 1;

        TupleDescAttr(tupdesc, 0)->attcacheoff = 0;

        while (j < natts && TupleDescAttr(tupdesc, j)->attcacheoff > 0)
            j++;

        off = TupleDescAttr(tupdesc, j - 1)->attcacheoff +
              TupleDescAttr(tupdesc, j - 1)->attlen;

        for (; j < natts; j++) {
            Form_pg_attribute att = TupleDescAttr(tupdesc, j);

            if (att->attlen <= 0) break;

            off = att_align_nominal(off, att->attalign);

            att->attcacheoff = off;

            off += att->attlen;
        }

        off = TupleDescAttr(tupdesc, attnum)->attcacheoff;
    } else {
        bool usecache = true;
        int i;

        off = 0;
        for (i = 0;; i++) {
            Form_pg_attribute att = TupleDescAttr(tupdesc, i);

            if (HeapTupleHasNulls(tuple) && att_isnull(i, bp)) {
                usecache = false;
                continue;
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

    return fetchatt(TupleDescAttr(tupdesc, attnum), tp + off);
}

static inline Datum fastgetattr(HeapTuple tup, int attnum, TupleDesc tupdesc,
                                bool* isnull)
{
    *isnull = false;

    if (HeapTupleNoNulls(tup)) {
        Form_pg_attribute att;

        att = TupleDescAttr(tupdesc, attnum - 1);
        if (att->attcacheoff >= 0)
            return fetchatt(att, (char*)tup->t_data + tup->t_data->t_hoff +
                                     att->attcacheoff);
        else
            return nocachegetattr(tup, attnum, tupdesc);
    } else {
        if (att_isnull(attnum - 1, tup->t_data->t_bits)) {
            *isnull = true;
            return (Datum)NULL;
        } else
            return nocachegetattr(tup, attnum, tupdesc);
    }
}

static Datum heap_getsysattr(HeapTuple tup, int attnum, TupleDesc tupdesc,
                             bool* isnull)
{
    Datum result;

    *isnull = false;

    return 0;
}

static inline Datum heap_getattr(HeapTuple tup, int attnum, TupleDesc tupdesc,
                                 bool* isnull)
{
    if (attnum > 0) {
        return fastgetattr(tup, attnum, tupdesc, isnull);
    }

    return heap_getsysattr(tup, attnum, tupdesc, isnull);
}

static bool HeapTupleSatisfiesVisibility(HeapTuple tup, Snapshot snapshot,
                                         Buffer buffer)
{
    return true;
}

static bool HeapKeyTest(HeapTuple tuple, TupleDesc tupdesc, int nkeys,
                        ScanKey keys)
{
    int cur_nkeys = nkeys;
    ScanKey cur_keys = keys;

    for (; cur_nkeys--; cur_keys++) {
        Datum atp;
        bool is_null;
        int test;

        atp = heap_getattr(tuple, cur_keys->sk_attno, tupdesc, &is_null);

        if (is_null) return false;

        test = FunctionCall2Coll(&cur_keys->sk_func, C_COLLATION_OID, atp,
                                 cur_keys->sk_argument);

        if (!test) return false;
    }

    return true;
}

static void heapgettup(HeapScanDesc scan, ScanDirection dir, int nkeys,
                       ScanKey key)
{
    HeapTuple tuple = &(scan->rs_ctup);
    Snapshot snapshot = scan->rs_base.rs_snapshot;
    int backward = dir == BackwardScanDirection;
    char* dp;
    BlockNumber page;
    int finished;
    int lines;
    OffsetNumber lineoff;
    int linesleft;
    ItemId lpp;

    if (dir == ForwardScanDirection) {
        if (!scan->rs_inited) {
            if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0) {
                tuple->t_data = NULL;
                return;
            }

            page = scan->rs_startblock;
            heapgetpage((TableScanDesc)scan, page);
            lineoff = FirstOffsetNumber;
            scan->rs_inited = true;
        } else {
            page = scan->rs_cblock;
            lineoff = tuple->t_self.ip_posid + 1;
        }

        LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

        dp = BufferGetPage(scan->rs_cbuf);
        lines = PageGetMaxOffsetNumber(dp);

        linesleft = lines - lineoff + 1;
    } else if (backward) {
        if (!scan->rs_inited) {
            if (scan->rs_nblocks == 0 || scan->rs_numblocks == 0) {
                tuple->t_data = NULL;
                return;
            }

            if (scan->rs_numblocks != InvalidBlockNumber)
                page = (scan->rs_startblock + scan->rs_numblocks - 1) %
                       scan->rs_nblocks;
            else if (scan->rs_startblock > 0)
                page = scan->rs_startblock - 1;
            else
                page = scan->rs_nblocks - 1;
            heapgetpage((TableScanDesc)scan, page);
        } else {
            page = scan->rs_cblock;
        }

        LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

        dp = BufferGetPage(scan->rs_cbuf);
        lines = PageGetMaxOffsetNumber(dp);

        if (!scan->rs_inited) {
            lineoff = lines;
            scan->rs_inited = true;
        } else {
            lineoff = tuple->t_self.ip_posid - 1;
        }

        linesleft = lineoff;
    } else {
        if (!scan->rs_inited) {
            tuple->t_data = NULL;
            return;
        }

        page = ItemPointerGetBlockNumber(&tuple->t_self);
        if (page != scan->rs_cblock) heapgetpage((TableScanDesc)scan, page);

        dp = BufferGetPage(scan->rs_cbuf);
        lineoff = tuple->t_self.ip_posid;
        lpp = PageGetItemId(dp, lineoff);

        tuple->t_data = (HeapTupleHeader)PageGetItem(dp, lpp);
        tuple->t_len = lpp->lp_len;

        return;
    }

    lpp = PageGetItemId(dp, lineoff);
    for (;;) {
        while (linesleft > 0) {
            bool valid;

            tuple->t_data = (HeapTupleHeader)PageGetItem(dp, lpp);
            tuple->t_len = lpp->lp_len;
            ItemPointerSet(&(tuple->t_self), page, lineoff);

            valid =
                HeapTupleSatisfiesVisibility(tuple, snapshot, scan->rs_cbuf);

            if (valid && key != NULL)
                valid =
                    HeapKeyTest(tuple, scan->rs_base.rs_rd->rd_att, nkeys, key);

            if (valid) {
                LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
                return;
            }

            --linesleft;
            if (backward) {
                --lpp;
                --lineoff;
            } else {
                ++lpp;
                ++lineoff;
            }
        }

        LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

        if (backward) {
            finished = (page == scan->rs_startblock) ||
                       (scan->rs_numblocks != InvalidBlockNumber
                            ? --scan->rs_numblocks == 0
                            : 0);
            if (page == 0) page = scan->rs_nblocks;
            page--;
        } else {
            page++;
            if (page >= scan->rs_nblocks) page = 0;
            finished = (page == scan->rs_startblock) ||
                       (scan->rs_numblocks != InvalidBlockNumber
                            ? --scan->rs_numblocks == 0
                            : 0);
        }

        if (finished) {
            if (BufferIsValid(scan->rs_cbuf)) ReleaseBuffer(scan->rs_cbuf);
            scan->rs_cbuf = InvalidBuffer;
            scan->rs_cblock = InvalidBlockNumber;
            tuple->t_data = NULL;
            scan->rs_inited = false;
            return;
        }

        heapgetpage((TableScanDesc)scan, page);

        LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

        dp = BufferGetPage(scan->rs_cbuf);
        lines = PageGetMaxOffsetNumber(dp);
        linesleft = lines;

        if (backward) {
            lineoff = lines;
            lpp = PageGetItemId(dp, lines);
        } else {
            lineoff = FirstOffsetNumber;
            lpp = PageGetItemId(dp, FirstOffsetNumber);
        }
    }
}

HeapTuple heap_getnext(TableScanDesc sscan, ScanDirection direction)
{
    HeapScanDesc scan = (HeapScanDesc)sscan;

    heapgettup(scan, direction, scan->rs_base.rs_nkeys, scan->rs_base.rs_key);

    if (scan->rs_ctup.t_data == NULL) return NULL;

    return &scan->rs_ctup;
}

HeapTuple heap_hot_search_buffer(ItemPointer tid, Relation relation,
                                 Buffer buffer, Snapshot snapshot,
                                 HeapTuple heapTuple, bool first_call)
{
    char* dp = BufferGetPage(buffer);
    TransactionId prev_xmax = InvalidTransactionId;
    BlockNumber blkno;
    OffsetNumber offnum;
    bool at_chain_start;
    bool valid;
    bool skip;

    blkno = ItemPointerGetBlockNumber(tid);
    offnum = tid->ip_posid;
    at_chain_start = first_call;
    skip = !first_call;

    for (;;) {
        ItemId lp;

        if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(dp))
            break;

        lp = PageGetItemId(dp, offnum);

        if (!ItemIdIsNormal(lp)) {
            if (ItemIdIsRedirect(lp) && at_chain_start) {
                offnum = lp->lp_off;
                at_chain_start = false;
                continue;
            }

            break;
        }

        heapTuple->t_data = (HeapTupleHeader)PageGetItem(dp, lp);
        heapTuple->t_len = lp->lp_len;
        ItemPointerSet(&heapTuple->t_self, blkno, offnum);

        if (at_chain_start && HeapTupleHeaderIsHeapOnly(heapTuple->t_data))
            break;

        if (prev_xmax != InvalidTransactionId &&
            prev_xmax != HeapTupleHeaderGetXmin(heapTuple->t_data))
            break;

        if (!skip) {
            valid = HeapTupleSatisfiesVisibility(heapTuple, snapshot, buffer);

            if (valid) {
                tid->ip_posid = offnum;
                return heapTuple;
            }
        }
        skip = false;

        if (HeapTupleHeaderIsHotUpdated(heapTuple->t_data)) {
            offnum = heapTuple->t_data->t_ctid.ip_posid;
            at_chain_start = false;
            prev_xmax = HeapTupleHeaderGetUpdateXid(heapTuple->t_data);
        } else
            break;
    }

    return NULL;
}

IndexFetchTableData* heapam_index_fetch_begin(Relation rel)
{
    IndexFetchHeapData* hscan = malloc(sizeof(IndexFetchHeapData));

    hscan->xs_base.rel = rel;
    hscan->xs_cbuf = InvalidBuffer;

    hscan->xs_bufdat.blkno = InvalidBlockNumber;
    hscan->xs_bufdat.bufpage = bufpage_alloc();

    return &hscan->xs_base;
}

void heapam_index_fetch_reset(IndexFetchTableData* scan)
{
    IndexFetchHeapData* hscan = (IndexFetchHeapData*)scan;

    if (BufferIsValid(hscan->xs_cbuf)) {
        ReleaseBuffer(hscan->xs_cbuf);
        hscan->xs_cbuf = InvalidBuffer;
    }

    hscan->xs_bufdat.blkno = InvalidBlockNumber;
}

void heapam_index_fetch_end(IndexFetchTableData* scan)
{
    IndexFetchHeapData* hscan = (IndexFetchHeapData*)scan;

    heapam_index_fetch_reset(scan);

    if (hscan->xs_bufdat.bufpage) bufpage_free(hscan->xs_bufdat.bufpage);

    free(hscan);
}

HeapTuple heapam_index_fetch_tuple(struct IndexFetchTableData* scan,
                                   ItemPointer tid, Snapshot snapshot,
                                   bool* call_again)
{
    IndexFetchHeapData* hscan = (IndexFetchHeapData*)scan;
    HeapTuple htup;

    if (!*call_again) {
        BlockNumber blkno = ItemPointerGetBlockNumber(tid);

        if (!BufferIsValid(hscan->xs_cbuf) ||
            BufferGetBlockNumber(hscan->xs_cbuf) != blkno) {
            if (BufferIsValid(hscan->xs_cbuf)) ReleaseBuffer(hscan->xs_cbuf);

            hscan->xs_cbuf =
                rel_read_buffer(hscan->xs_base.rel, blkno, &hscan->xs_bufdat);
        }
    }

    LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_SHARE);
    htup = heap_hot_search_buffer(tid, hscan->xs_base.rel, hscan->xs_cbuf,
                                  snapshot, &hscan->xs_tupdata, !*call_again);
    hscan->xs_tupdata.t_self = *tid;
    LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);

    *call_again = false;

    return htup;
}

void heap_deform_tuple(HeapTuple tuple, TupleDesc tupleDesc, Datum* values,
                       bool* isnull)
{
    HeapTupleHeader tup = tuple->t_data;
    bool hasnulls = HeapTupleHasNulls(tuple);
    int tdesc_natts = tupleDesc->natts;
    int natts;
    int attnum;
    char* tp;
    uint32_t off;
    uint8_t* bp = tup->t_bits;
    bool slow = false;

    natts = HeapTupleHeaderGetNatts(tup);

    if (natts > tdesc_natts) natts = tdesc_natts;

    tp = (char*)tup + tup->t_hoff;

    off = 0;

    for (attnum = 0; attnum < natts; attnum++) {
        Form_pg_attribute thisatt = TupleDescAttr(tupleDesc, attnum);

        if (hasnulls && att_isnull(attnum, bp)) {
            values[attnum] = (Datum)0;
            isnull[attnum] = true;
            slow = true;
            continue;
        }

        isnull[attnum] = false;

        if (!slow && thisatt->attcacheoff >= 0)
            off = thisatt->attcacheoff;
        else if (thisatt->attlen == -1) {
            if (!slow && off == att_align_nominal(off, thisatt->attalign))
                thisatt->attcacheoff = off;
            else {
                off = att_align_pointer(off, thisatt->attalign, -1, tp + off);
                slow = true;
            }
        } else {
            off = att_align_nominal(off, thisatt->attalign);

            if (!slow) thisatt->attcacheoff = off;
        }

        values[attnum] = fetchatt(thisatt, tp + off);

        off = att_addlength_pointer(off, thisatt->attlen, tp + off);

        if (thisatt->attlen <= 0) slow = true;
    }
}

HeapTuple heap_copytuple(HeapTuple tuple)
{
    HeapTuple newTuple;

    if (tuple == NULL || tuple->t_data == NULL) return NULL;

    newTuple = (HeapTuple)malloc(HEAPTUPLESIZE + tuple->t_len);
    newTuple->t_len = tuple->t_len;
    newTuple->t_self = tuple->t_self;
    newTuple->t_tableOid = tuple->t_tableOid;
    newTuple->t_data = (HeapTupleHeader)((char*)newTuple + HEAPTUPLESIZE);
    memcpy((char*)newTuple->t_data, (char*)tuple->t_data, tuple->t_len);
    return newTuple;
}
