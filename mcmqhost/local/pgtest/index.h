#ifndef _INDEX_H_
#define _INDEX_H_

#include "types.h"
#include "tupdesc.h"
#include "relation.h"
#include "skey.h"

#define INDEX_MAX_KEYS 32

typedef struct IndexTupleData {
    ItemPointerData t_tid; /* reference TID to heap tuple */

    /* ---------------
     * t_info is laid out in the following fashion:
     *
     * 15th (high) bit: has nulls
     * 14th bit: has var-width attributes
     * 13th bit: AM-defined meaning
     * 12-0 bit: size of tuple
     * ---------------
     */

    unsigned short t_info; /* various info about tuple */

} IndexTupleData; /* MORE DATA FOLLOWS AT END OF STRUCT */

typedef IndexTupleData* IndexTuple;

typedef struct IndexAttributeBitMapData {
    uint8_t bits[(INDEX_MAX_KEYS + 8 - 1) / 8];
} IndexAttributeBitMapData;

typedef IndexAttributeBitMapData* IndexAttributeBitMap;

#define INDEX_SIZE_MASK 0x1FFF
#define INDEX_AM_RESERVED_BIT                \
    0x2000 /* reserved for index-AM specific \
            * usage */
#define INDEX_VAR_MASK  0x4000
#define INDEX_NULL_MASK 0x8000

#define IndexTupleSize(itup) ((size_t)((itup)->t_info & INDEX_SIZE_MASK))
#define IndexTupleHasNulls(itup) \
    ((((IndexTuple)(itup))->t_info & INDEX_NULL_MASK))
#define IndexTupleHasVarwidths(itup) \
    ((((IndexTuple)(itup))->t_info & INDEX_VAR_MASK))

typedef struct IndexFetchTableData {
    Relation rel;
} IndexFetchTableData;

typedef IndexFetchTableData* IndexFetchTable;

typedef struct IndexScanDescData {
    Relation heapRelation;
    Relation indexRelation;
    Snapshot xs_snapshot;
    int numberOfKeys;
    struct ScanKeyData* keyData;

    void* opaque;

    IndexTuple xs_itup;
    TupleDesc xs_itupdesc;
    /* HeapTuple xs_hitup; */

    ItemPointerData xs_heaptid;
    bool xs_heap_continue;
    IndexFetchTableData* xs_heapfetch;
} IndexScanDescData;

typedef IndexScanDescData* IndexScanDesc;

#define IndexInfoFindDataOffset(t_info)               \
    ((!((t_info)&INDEX_NULL_MASK))                    \
         ? ((size_t)MAXALIGN(sizeof(IndexTupleData))) \
         : ((size_t)MAXALIGN(sizeof(IndexTupleData) + \
                             sizeof(IndexAttributeBitMapData))))

#define index_getattr(tup, attnum, tupleDesc, isnull)                       \
    (*(isnull) = false,                                                     \
     !IndexTupleHasNulls(tup)                                               \
         ? (TupleDescAttr((tupleDesc), (attnum)-1)->attcacheoff >= 0        \
                ? (fetchatt(TupleDescAttr((tupleDesc), (attnum)-1),         \
                            (char*)(tup) +                                  \
                                IndexInfoFindDataOffset((tup)->t_info) +    \
                                TupleDescAttr((tupleDesc), (attnum)-1)      \
                                    ->attcacheoff))                         \
                : nocache_index_getattr((tup), (attnum), (tupleDesc)))      \
         : ((att_isnull((attnum)-1, (char*)(tup) + sizeof(IndexTupleData))) \
                ? (*(isnull) = true, (Datum)NULL)                           \
                : (nocache_index_getattr((tup), (attnum), (tupleDesc)))))

__BEGIN_DECLS

Datum nocache_index_getattr(IndexTuple tup, int attnum, TupleDesc tupleDesc);

IndexScanDesc RelationGetIndexSacn(Relation indexRelation, int nkeys);
void IndexScanEnd(IndexScanDesc scan);

IndexScanDesc index_beginscan(Relation heapRelation, Relation indexRelation,
                              Snapshot snapshot, int nkeys);
void index_rescan(IndexScanDesc scan, ScanKey scankey, int nkeys);
void index_endscan(IndexScanDesc scan);

ItemPointer index_getnext_tid(IndexScanDesc scan, ScanDirection direction);

typedef struct HeapTupleData* HeapTuple;
HeapTuple index_getnext_slot(IndexScanDesc scan, ScanDirection direction);

__END_DECLS

#endif
