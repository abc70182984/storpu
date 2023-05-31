#include "btree.h"

#include <stdlib.h>
#include <string.h>

Buffer _bt_getbuf(Relation rel, BlockNumber blkno, int access, Buffer buf)
{
    if (blkno != P_NEW) {
        buf = rel_read_buffer(rel, blkno, buf);
        LockBuffer(buf, access);
    } else {
        abort();
        return NULL;
    }

    return buf;
}

void _bt_relbuf(Relation rel, Buffer buf)
{
    buf->blkno = InvalidBlockNumber;
    LockBuffer(buf, BUFFER_LOCK_UNLOCK);
    ReleaseBuffer(buf);
}

Buffer _bt_relandgetbuf(Relation rel, Buffer obuf, BlockNumber blkno,
                        int access)
{
    Buffer buf;

    if (BufferIsValid(obuf) && obuf->blkno != InvalidBlockNumber)
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
    ReleaseBuffer(obuf);
    buf = rel_read_buffer(rel, blkno, obuf);
    LockBuffer(buf, access);

    return buf;
}

static Buffer _bt_getroot(Relation rel, int access, Buffer buf)
{
    BTMetaPageData* metad;
    BTPageOpaque rootopaque;
    BlockNumber rootblkno;
    Buffer rootbuf;
    char* rootpage;
    Buffer metabuf;
    char* metapg;

    if (rel->rd_amcache != NULL) {
        metad = (BTMetaPageData*)rel->rd_amcache;

        rootblkno = metad->btm_fastlevel;

        rootbuf = _bt_getbuf(rel, rootblkno, BT_READ, buf);
        rootpage = BufferGetPage(rootbuf);
        rootopaque = BTPageGetOpaque(rootpage);

        if (!P_IGNORE(rootopaque) && P_LEFTMOST(rootopaque) &&
            P_RIGHTMOST(rootopaque))
            return rootbuf;

        _bt_relbuf(rel, buf);
        if (rel->rd_amcache) free(rel->rd_amcache);
        rel->rd_amcache = NULL;
    }

    metabuf = _bt_getbuf(rel, 0, BT_READ, buf);
    metapg = BufferGetPage(metabuf);
    metad = BTPageGetMeta(metapg);

    if (metad->btm_root == P_NONE) {
        return 0;
    } else {
        rootblkno = metad->btm_fastroot;

        rel->rd_amcache = malloc(sizeof(BTMetaPageData));
        memcpy(rel->rd_amcache, metad, sizeof(BTMetaPageData));

        rootbuf = metabuf;

        for (;;) {
            rootbuf = _bt_relandgetbuf(rel, rootbuf, rootblkno, BT_READ);
            rootpage = BufferGetPage(rootbuf);
            rootopaque = BTPageGetOpaque(rootpage);

            if (!P_IGNORE(rootopaque)) break;

            rootblkno = rootopaque->btpo_next;
        }

        return rootbuf;
    }
}

static int _bt_compare(Relation rel, BTScanInsert key, char* page,
                       OffsetNumber offnum)
{
    TupleDesc itupdesc = rel->rd_att;
    BTPageOpaque opaque = BTPageGetOpaque(page);
    IndexTuple itup;
    int ncmpkey;
    int ntupatts;
    ScanKey scankey;
    ItemPointer heapTid;
    int result;

    if (!P_ISLEAF(opaque) && offnum == P_FIRSTDATAKEY(opaque)) return 1;

    itup = (IndexTuple)PageGetItem(page, PageGetItemId(page, offnum));
    ntupatts = BTreeTupleGetNAtts(itup, rel);

    ncmpkey = key->keysz;
    if (ncmpkey > ntupatts) ncmpkey = ntupatts;

    scankey = key->scankeys;

    for (int i = 1; i <= ncmpkey; i++) {
        Datum datum;
        bool isnull;

        datum = index_getattr(itup, scankey->sk_attno, itupdesc, &isnull);

        if (isnull) {
            result = -1;
        } else {
            result = FunctionCall2Coll(&scankey->sk_func, C_COLLATION_OID,
                                       datum, scankey->sk_argument);
            result = -result;
        }

        if (result != 0) return result;

        scankey++;
    }

    if (key->keysz > ntupatts) return 1;

    heapTid = BTreeTupleGetHeapTID(itup);
    if (key->scantid == NULL) {
        if (key->heapkeyspace && !key->pivotsearch && key->keysz == ntupatts &&
            heapTid == NULL)
            return 1;

        return 0;
    }

    if (heapTid == NULL) return 1;

    result = ItemPointerCompare(key->scantid, heapTid);
    return result;
}

static OffsetNumber _bt_binsrch(Relation rel, BTScanInsert key, Buffer buf)
{
    char* page;
    BTPageOpaque opaque;
    OffsetNumber low, high;
    int result, cmpval;

    page = BufferGetPage(buf);
    opaque = BTPageGetOpaque(page);

    low = P_FIRSTDATAKEY(opaque);
    high = PageGetMaxOffsetNumber(page);

    if (high < low) return low;

    high++;
    cmpval = key->nextkey ? 0 : 1;

    while (high > low) {
        OffsetNumber mid = low + ((high - low) / 2);

        result = _bt_compare(rel, key, page, mid);

        if (result >= cmpval)
            low = mid + 1;
        else
            high = mid;
    }

    if (P_ISLEAF(opaque)) return low;

    return low - 1;
}

static Buffer _bt_moveright(Relation rel, BTScanInsert key, Buffer buf,
                            BTStack stack, int access)
{
    char* page;
    BTPageOpaque opaque;
    int cmpval;

    cmpval = key->nextkey ? 0 : 1;

    for (;;) {
        page = BufferGetPage(buf);
        opaque = BTPageGetOpaque(page);

        if (P_RIGHTMOST(opaque)) break;

        if (P_IGNORE(opaque) ||
            _bt_compare(rel, key, page, P_HIKEY) >= cmpval) {
            buf = _bt_relandgetbuf(rel, buf, opaque->btpo_next, access);
            continue;
        } else
            break;
    }

    return buf;
}

BTStack _bt_search(Relation rel, BTScanInsert key, Buffer* bufP, int access)
{
    BTStack stack_in = NULL;
    int page_access = BT_READ;

    *bufP = _bt_getroot(rel, access, *bufP);

    if (!BufferIsValid(*bufP)) return (BTStack)NULL;

    for (;;) {
        char* page;
        BTPageOpaque opaque;
        OffsetNumber offnum;
        ItemId itemid;
        IndexTuple itup;
        BlockNumber child;
        BTStack new_stack;

        *bufP = _bt_moveright(rel, key, *bufP, stack_in, page_access);

        page = BufferGetPage(*bufP);
        opaque = BTPageGetOpaque(page);
        if (P_ISLEAF(opaque)) break;

        offnum = _bt_binsrch(rel, key, *bufP);
        itemid = PageGetItemId(page, offnum);
        itup = (IndexTuple)PageGetItem(page, itemid);
        child = BTreeTupleGetDownLink(itup);

        new_stack = (BTStack)malloc(sizeof(BTStackData));
        new_stack->bts_blkno = BufferGetBlockNumber(*bufP);
        new_stack->bts_offset = offnum;
        new_stack->bts_parent = stack_in;

        *bufP = _bt_relandgetbuf(rel, *bufP, child, page_access);

        stack_in = new_stack;
    }

    return stack_in;
}

void _bt_freestack(BTStack stack)
{
    BTStack ostack;

    while (stack != NULL) {
        ostack = stack;
        stack = stack->bts_parent;
        free(ostack);
    }
}

void _bt_preprocess_key(IndexScanDesc scan)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    int numberOfKeys = scan->numberOfKeys;

    memcpy(so->keyData, scan->keyData, numberOfKeys * sizeof(ScanKeyData));
    so->numberOfKeys = numberOfKeys;
}

static inline void _bt_initialize_more_data(BTScanOpaque so, ScanDirection dir)
{
    if (dir == ForwardScanDirection) {
        so->currPos.moreLeft = false;
        so->currPos.moreRight = true;
    } else {
        so->currPos.moreLeft = true;
        so->currPos.moreRight = false;
    }
}

static bool _bt_checkkeys(IndexScanDesc scan, IndexTuple tuple, int tupnatts,
                          ScanDirection dir, bool* continuescan)
{
    TupleDesc tupdesc;
    BTScanOpaque so;
    int keysz;
    int ikey;
    ScanKey key;

    *continuescan = true;

    tupdesc = scan->indexRelation->rd_att;
    so = (BTScanOpaque)scan->opaque;
    keysz = so->numberOfKeys;

    for (key = so->keyData, ikey = 0; ikey < keysz; key++, ikey++) {
        Datum datum;
        bool isnull;
        int test;

        if (key->sk_attno > tupnatts) continue;

        datum = index_getattr(tuple, key->sk_attno, tupdesc, &isnull);

        if (isnull) {
            abort();
            return false;
        }

        test = FunctionCall2Coll(&key->sk_func, C_COLLATION_OID, datum,
                                 key->sk_argument);

        if (!test) {
            if ((key->sk_flags & SK_BT_REQFWD) &&
                (dir == ForwardScanDirection)) {
                *continuescan = false;
            } else if ((key->sk_flags & SK_BT_REQBKWD) &&
                       (dir == BackwardScanDirection)) {
                *continuescan = false;
            }

            return false;
        }
    }

    return true;
}

static void _bt_saveitem(BTScanOpaque so, int itemIndex, OffsetNumber offnum,
                         IndexTuple itup)
{
    BTScanPosItem* currItem = &so->currPos.items[itemIndex];

    currItem->heapTid = itup->t_tid;
    currItem->indexOffset = offnum;
    if (so->currTuples) {
        size_t itupsz = IndexTupleSize(itup);

        currItem->tupleOffset = so->currPos.nextTupleOffset;
        memcpy(so->currTuples + so->currPos.nextTupleOffset, itup, itupsz);
        so->currPos.nextTupleOffset += MAXALIGN(itupsz);
    }
}

static bool _bt_readpage(IndexScanDesc scan, ScanDirection dir,
                         OffsetNumber offnum)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    char* page;
    BTPageOpaque opaque;
    OffsetNumber minoff;
    OffsetNumber maxoff;
    int itemIndex;
    bool continuescan;
    int indnatts;

    page = BufferGetPage(so->currPos.buf);
    opaque = BTPageGetOpaque(page);

    continuescan = true;
    indnatts = scan->indexRelation->rd_att->natts;
    minoff = P_FIRSTDATAKEY(opaque);
    maxoff = PageGetMaxOffsetNumber(page);

    so->currPos.currPage = BufferGetBlockNumber(so->currPos.buf);

    so->currPos.nextPage = opaque->btpo_next;
    so->currPos.nextTupleOffset = 0;

    if (dir == ForwardScanDirection) {
        itemIndex = 0;

        if (offnum < minoff) offnum = minoff;

        while (offnum <= maxoff) {
            ItemId iid = PageGetItemId(page, offnum);
            IndexTuple itup;

            itup = (IndexTuple)PageGetItem(page, iid);

            if (_bt_checkkeys(scan, itup, indnatts, dir, &continuescan)) {
                _bt_saveitem(so, itemIndex, offnum, itup);
                itemIndex++;
            }

            if (!continuescan) break;

            offnum++;
        }

        if (continuescan && !P_RIGHTMOST(opaque)) {
            ItemId iid = PageGetItemId(page, P_HIKEY);
            IndexTuple itup = (IndexTuple)PageGetItem(page, iid);
            int truncatt;

            truncatt = BTreeTupleGetNAtts(itup, scan->indexRelation);
            _bt_checkkeys(scan, itup, truncatt, dir, &continuescan);
        }

        if (!continuescan) so->currPos.moreRight = false;

        so->currPos.firstItem = 0;
        so->currPos.lastItem = itemIndex - 1;
        so->currPos.itemIndex = 0;
    } else {
        itemIndex = MaxTIDsPerBTreePage;

        if (offnum > maxoff) offnum = maxoff;

        while (offnum >= minoff) {
            ItemId iid = PageGetItemId(page, offnum);
            IndexTuple itup;

            itup = (IndexTuple)PageGetItem(page, iid);

            if (_bt_checkkeys(scan, itup, indnatts, dir, &continuescan)) {
                itemIndex--;
                _bt_saveitem(so, itemIndex, offnum, itup);
            }

            if (!continuescan) {
                so->currPos.moreLeft = false;
                break;
            }

            offnum--;
        }

        so->currPos.firstItem = itemIndex;
        so->currPos.lastItem = MaxTIDsPerBTreePage - 1;
        so->currPos.itemIndex = MaxTIDsPerBTreePage - 1;
    }

    return so->currPos.firstItem <= so->currPos.lastItem;
}

static bool _bt_readnextpage(IndexScanDesc scan, BlockNumber blkno,
                             ScanDirection dir)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    Relation rel;
    char* page;
    BTPageOpaque opaque;
    bool status;

    rel = scan->indexRelation;

    if (dir == ForwardScanDirection) {
        for (;;) {
            if (blkno == P_NONE || !so->currPos.moreRight) {
                BTScanPosInvalidate(so->currPos);
                return false;
            }

            so->currPos.buf = _bt_getbuf(rel, blkno, BT_READ, &so->bufdat);
            page = BufferGetPage(so->currPos.buf);
            opaque = BTPageGetOpaque(page);

            if (!P_IGNORE(opaque)) {
                if (_bt_readpage(scan, dir, P_FIRSTDATAKEY(opaque))) break;
            }

            blkno = opaque->btpo_next;
            _bt_relbuf(rel, so->currPos.buf);
        }
    } else {
        abort();
        return false;
    }

    return true;
}

static bool _bt_steppage(IndexScanDesc scan, ScanDirection dir)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    BlockNumber blkno = InvalidBlockNumber;
    bool status;

    if (dir == ForwardScanDirection) {
        blkno = so->currPos.nextPage;
        so->currPos.moreLeft = true;

        BTScanPosUnpinIfPinned(so->currPos);
    } else {
        so->currPos.moreRight = true;
        blkno = so->currPos.currPage;
    }

    if (!_bt_readnextpage(scan, blkno, dir)) return false;

    LockBuffer(so->currPos.buf, BUFFER_LOCK_UNLOCK);

    return true;
}

bool _bt_first(IndexScanDesc scan, ScanDirection dir)
{
    Relation rel = scan->indexRelation;
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    ScanKey startKeys[INDEX_MAX_KEYS];
    int keysCount = 0;
    Buffer buf;
    BTStack stack;
    BTScanInsertData inskey;
    int i;
    uint16_t strat;
    bool nextkey;
    bool goback;
    uint16_t strat_total;
    OffsetNumber offnum;
    BTScanPosItem* currItem;
    BlockNumber blkno;

    _bt_preprocess_key(scan);

    strat_total = BTEqualStrategyNumber;
    if (so->numberOfKeys > 0) {
        AttrNumber curattr;
        ScanKey chosen;
        ScanKey impliesNN;
        ScanKey cur;

        curattr = 1;
        chosen = NULL;
        impliesNN = NULL;

        for (cur = so->keyData, i = 0;; cur++, i++) {
            if (i >= so->numberOfKeys || cur->sk_attno != curattr) {
                if (!chosen) break;
                startKeys[keysCount++] = chosen;

                strat = chosen->sk_strategy;
                if (strat != BTEqualStrategyNumber) {
                    strat_total = strat;
                    if (strat == BTGreaterStrategyNumber ||
                        strat == BTLessStrategyNumber)
                        break;
                }

                if (i >= so->numberOfKeys || cur->sk_attno != curattr + 1)
                    break;

                curattr = cur->sk_attno;
                chosen = NULL;
                impliesNN = NULL;
            }

            switch (cur->sk_strategy) {
            case BTLessStrategyNumber:
            case BTLessEqualStrategyNumber:
                if (chosen == NULL) {
                    if (dir == BackwardScanDirection)
                        chosen = cur;
                    else
                        impliesNN = cur;
                }
                break;
            case BTEqualStrategyNumber:
                chosen = cur;
                break;
            case BTGreaterEqualStrategyNumber:
            case BTGreaterStrategyNumber:
                if (chosen == NULL) {
                    if (dir == ForwardScanDirection)
                        chosen = cur;
                    else
                        impliesNN = cur;
                }
                break;
            }
        }
    }

    if (keysCount == 0) {
        abort();
        return false;
    }

    for (i = 0; i < keysCount; i++) {
        ScanKey cur = startKeys[i];

        ScanKeyInit(&inskey.scankeys[i], cur->sk_attno, 0,
                    rel->rd_att->attrs[cur->sk_attno - 1].atttypid->cmp_proc,
                    cur->sk_argument);
    }

    switch (strat_total) {
    case BTLessStrategyNumber:

        nextkey = false;
        goback = true;
        break;

    case BTLessEqualStrategyNumber:

        nextkey = true;
        goback = true;
        break;

    case BTEqualStrategyNumber:

        if (dir == BackwardScanDirection) {
            nextkey = true;
            goback = true;
        } else {
            nextkey = false;
            goback = false;
        }
        break;

    case BTGreaterEqualStrategyNumber:

        nextkey = false;
        goback = false;
        break;

    case BTGreaterStrategyNumber:

        nextkey = true;
        goback = false;
        break;

    default:
        return false;
    }

    inskey.heapkeyspace = true;
    inskey.nextkey = nextkey;
    inskey.pivotsearch = false;
    inskey.scantid = NULL;
    inskey.keysz = keysCount;

    buf = &so->bufdat;
    stack = _bt_search(rel, &inskey, &buf, BT_READ);

    _bt_freestack(stack);
    stack = NULL;

    if (!BufferIsValid(buf)) {
        BTScanPosInvalidate(so->currPos);
        return false;
    }

    _bt_initialize_more_data(so, dir);

    offnum = _bt_binsrch(rel, &inskey, buf);
    if (goback) offnum = offnum - 1;

    so->currPos.buf = buf;

    if (!_bt_readpage(scan, dir, offnum)) {
        LockBuffer(so->currPos.buf, BUFFER_LOCK_UNLOCK);
        if (!_bt_steppage(scan, dir)) return false;
    } else {
        LockBuffer(so->currPos.buf, BUFFER_LOCK_UNLOCK);
    }

    currItem = &so->currPos.items[so->currPos.itemIndex];
    scan->xs_heaptid = currItem->heapTid;
    scan->xs_itup = (IndexTuple)(so->currTuples + currItem->tupleOffset);

    return true;
}

bool _bt_next(IndexScanDesc scan, ScanDirection dir)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    BTScanPosItem* currItem;

    if (dir == ForwardScanDirection) {
        if (++so->currPos.itemIndex > so->currPos.lastItem) {
            if (!_bt_steppage(scan, dir)) return false;
        }
    } else {
        if (--so->currPos.itemIndex < so->currPos.firstItem) {
            if (!_bt_steppage(scan, dir)) return false;
        }
    }

    /* OK, itemIndex says what to return */
    currItem = &so->currPos.items[so->currPos.itemIndex];
    scan->xs_heaptid = currItem->heapTid;
    scan->xs_itup = (IndexTuple)(so->currTuples + currItem->tupleOffset);

    return true;
}

IndexScanDesc btbeginscan(Relation rel, int nkeys)
{
    IndexScanDesc scan;
    BTScanOpaque so;

    scan = RelationGetIndexSacn(rel, nkeys);

    so = malloc(sizeof(BTScanOpaqueData));
    so->bufdat.bufpage = bufpage_alloc();
    BTScanPosInvalidate(so->currPos);
    if (scan->numberOfKeys > 0)
        so->keyData = malloc(scan->numberOfKeys * sizeof(ScanKeyData));
    else
        so->keyData = NULL;

    scan->xs_itupdesc = rel->rd_att;

    scan->opaque = so;

    return scan;
}

void btrescan(IndexScanDesc scan, ScanKey scankey, int nkeys)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;

    if (BTScanPosIsValid(so->currPos)) {
        BTScanPosInvalidate(so->currPos);
    }

    if (!so->currTuples) so->currTuples = malloc(BLCKSZ);

    if (scankey && scan->numberOfKeys > 0)
        memmove(scan->keyData, scankey,
                scan->numberOfKeys * sizeof(ScanKeyData));

    so->numberOfKeys = 0;
}

bool btgettuple(IndexScanDesc scan, ScanDirection dir)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;
    bool res;

    if (!BTScanPosIsValid(so->currPos))
        res = _bt_first(scan, dir);
    else {
        res = _bt_next(scan, dir);
    }

    return res;
}

void btendscan(IndexScanDesc scan)
{
    BTScanOpaque so = (BTScanOpaque)scan->opaque;

    if (so->keyData) free(so->keyData);
    if (so->currTuples) free(so->currTuples);

    if (so->bufdat.bufpage) bufpage_free(so->bufdat.bufpage);
    free(so);
}
