#ifndef _HEAP_H_
#define _HEAP_H_

#include "types.h"
#include "relation.h"
#include "index.h"
#include "skey.h"

typedef struct HeapTupleFields {
    TransactionId t_xmin; /* inserting xact ID */
    TransactionId t_xmax; /* deleting or locking xact ID */

    union {
        CommandId t_cid;      /* inserting or deleting command ID, or both */
        TransactionId t_xvac; /* old-style VACUUM FULL xact ID */
    } t_field3;
} HeapTupleFields;

typedef struct DatumTupleFields {
    uint32_t datum_len_; /* varlena header (do not touch directly!) */

    uint32_t datum_typmod; /* -1, or identifier of a record type */

    Oid datum_typeid; /* composite type OID, or RECORDOID */

    /*
     * datum_typeid cannot be a domain over composite, only plain composite,
     * even if the datum is meant as a value of a domain-over-composite type.
     * This is in line with the general principle that CoerceToDomain does not
     * change the physical representation of the base type value.
     *
     * Note: field ordering is chosen with thought that Oid might someday
     * widen to 64 bits.
     */
} DatumTupleFields;

typedef struct HeapTupleHeaderData {
    union {
        HeapTupleFields t_heap;
        DatumTupleFields t_datum;
    } t_choice;

    ItemPointerData t_ctid; /* current TID of this or newer tuple (or a
                             * speculative insertion token) */
    uint16_t t_infomask2;
    uint16_t t_infomask;
    uint8_t t_hoff;
    uint8_t t_bits[];
} HeapTupleHeaderData;

typedef HeapTupleHeaderData* HeapTupleHeader;

/*
 * information stored in t_infomask:
 */
#define HEAP_HASNULL          0x0001 /* has null attribute(s) */
#define HEAP_HASVARWIDTH      0x0002 /* has variable-width attribute(s) */
#define HEAP_HASEXTERNAL      0x0004 /* has external stored attribute(s) */
#define HEAP_HASOID_OLD       0x0008 /* has an object-id field */
#define HEAP_XMAX_KEYSHR_LOCK 0x0010 /* xmax is a key-shared locker */
#define HEAP_COMBOCID         0x0020 /* t_cid is a combo CID */
#define HEAP_XMAX_EXCL_LOCK   0x0040 /* xmax is exclusive locker */
#define HEAP_XMAX_LOCK_ONLY   0x0080 /* xmax, if valid, is only a locker */

/* xmax is a shared locker */
#define HEAP_XMAX_SHR_LOCK (HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)

#define HEAP_LOCK_MASK \
    (HEAP_XMAX_SHR_LOCK | HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)
#define HEAP_XMIN_COMMITTED 0x0100 /* t_xmin committed */
#define HEAP_XMIN_INVALID   0x0200 /* t_xmin invalid/aborted */
#define HEAP_XMIN_FROZEN    (HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID)
#define HEAP_XMAX_COMMITTED 0x0400 /* t_xmax committed */
#define HEAP_XMAX_INVALID   0x0800 /* t_xmax invalid/aborted */
#define HEAP_XMAX_IS_MULTI  0x1000 /* t_xmax is a MultiXactId */
#define HEAP_UPDATED        0x2000 /* this is UPDATEd version of row */
#define HEAP_MOVED_OFF                          \
    0x4000 /* moved to another place by pre-9.0 \
            * VACUUM FULL; kept for binary      \
            * upgrade support */
#define HEAP_MOVED_IN                             \
    0x8000 /* moved from another place by pre-9.0 \
            * VACUUM FULL; kept for binary        \
            * upgrade support */
#define HEAP_MOVED (HEAP_MOVED_OFF | HEAP_MOVED_IN)

#define HEAP_XACT_MASK 0xFFF0 /* visibility-related bits */

#define HEAP_NATTS_MASK  0x07FF /* 11 bits for number of attributes */
#define HEAP_HOT_UPDATED 0x4000 /* tuple was HOT-updated */
#define HEAP_ONLY_TUPLE  0x8000 /* this is heap-only tuple */

#define HeapTupleHasNulls(tuple) \
    (((tuple)->t_data->t_infomask & HEAP_HASNULL) != 0)

#define HeapTupleNoNulls(tuple) (!((tuple)->t_data->t_infomask & HEAP_HASNULL))

#define HeapTupleHasVarWidth(tuple) \
    (((tuple)->t_data->t_infomask & HEAP_HASVARWIDTH) != 0)

#define HeapTupleAllFixed(tuple) \
    (!((tuple)->t_data->t_infomask & HEAP_HASVARWIDTH))

#define HeapTupleHeaderGetRawXmin(tup) ((tup)->t_choice.t_heap.t_xmin)

#define HeapTupleHeaderGetXmin(tup)                       \
    (HeapTupleHeaderXminFrozen(tup) ? FrozenTransactionId \
                                    : HeapTupleHeaderGetRawXmin(tup))

#define HeapTupleHeaderGetUpdateXid(tup) HeapTupleHeaderGetRawXmax(tup)

#define HeapTupleHeaderGetRawXmax(tup) ((tup)->t_choice.t_heap.t_xmax)

#define HeapTupleHeaderXminInvalid(tup)                                 \
    (((tup)->t_infomask & (HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID)) == \
     HEAP_XMIN_INVALID)

#define HeapTupleHeaderXminFrozen(tup) \
    (((tup)->t_infomask & (HEAP_XMIN_FROZEN)) == HEAP_XMIN_FROZEN)

#define HeapTupleHeaderIsHeapOnly(tup) \
    (((tup)->t_infomask2 & HEAP_ONLY_TUPLE) != 0)

#define HeapTupleHeaderIsHotUpdated(tup)             \
    (((tup)->t_infomask2 & HEAP_HOT_UPDATED) != 0 && \
     ((tup)->t_infomask & HEAP_XMAX_INVALID) == 0 && \
     !HeapTupleHeaderXminInvalid(tup))

#define HeapTupleHeaderGetNatts(tup) ((tup)->t_infomask2 & HEAP_NATTS_MASK)

typedef struct HeapTupleData {
    uint32_t t_len;
    ItemPointerData t_self;
    Oid t_tableOid;
    HeapTupleHeader t_data;
} HeapTupleData;

#define HEAPTUPLESIZE MAXALIGN(sizeof(HeapTupleData))

typedef HeapTupleData* HeapTuple;

typedef struct HeapScanDescData {
    TableScanDescData rs_base;

    BlockNumber rs_nblocks;    /* total number of blocks in rel */
    BlockNumber rs_startblock; /* block # to start at */
    BlockNumber rs_numblocks;  /* max number of blocks to scan */

    bool rs_inited;
    BlockNumber rs_cblock; /* current block # in scan, if any */
    Buffer rs_cbuf;        /* current buffer in scan, if any */
    BufferData rs_cbufdat;
    /* NB: if rs_cbuf is not InvalidBuffer, we hold a pin on that buffer */

    /* rs_numblocks is usually InvalidBlockNumber, meaning "scan whole rel" */

    HeapTupleData rs_ctup; /* current tuple in scan, if any */
} HeapScanDescData;

typedef struct HeapScanDescData* HeapScanDesc;

typedef struct IndexFetchHeapData {
    IndexFetchTableData xs_base;

    Buffer xs_cbuf;
    BufferData xs_bufdat;
    HeapTupleData xs_tupdata;
} IndexFetchHeapData;

__BEGIN_DECLS

TableScanDesc heap_beginscan(Relation relation, Snapshot snapshot, int nkeys,
                             ScanKey key);
void heap_rescan(TableScanDesc sscan, ScanKey key);
void heap_endscan(TableScanDesc sscan);

HeapTuple heap_getnext(TableScanDesc sscan, ScanDirection direction);

IndexFetchTableData* heapam_index_fetch_begin(Relation rel);
void heapam_index_fetch_reset(IndexFetchTableData* scan);
void heapam_index_fetch_end(IndexFetchTableData* scan);
HeapTuple heapam_index_fetch_tuple(struct IndexFetchTableData* scan,
                                   ItemPointer tid, Snapshot snapshot,
                                   bool* call_again);

void heap_deform_tuple(HeapTuple tuple, TupleDesc tupleDesc, Datum* values,
                       bool* isnull);
HeapTuple heap_copytuple(HeapTuple tuple);

__END_DECLS

#endif
