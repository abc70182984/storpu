#ifndef _TYPES_H_
#define _TYPES_H_

#include <stdint.h>
#include <stdbool.h>

typedef uintptr_t Datum;

typedef struct NullableDatum {
    Datum value;
    bool isnull;
} NullableDatum;

typedef unsigned int Oid;

typedef uint32_t CommandId;
typedef uint32_t TransactionId;

#define InvalidTransactionId     ((TransactionId)0)
#define BootstrapTransactionId   ((TransactionId)1)
#define FrozenTransactionId      ((TransactionId)2)
#define FirstNormalTransactionId ((TransactionId)3)
#define MaxTransactionId         ((TransactionId)0xFFFFFFFF)

typedef uint32_t BlockNumber;
#define InvalidBlockNumber ((BlockNumber)0xFFFFFFFF)

typedef struct BlockIdData {
    uint16_t bi_hi;
    uint16_t bi_lo;
} BlockIdData;

typedef BlockIdData* BlockId; /* block identifier */

#define BlockIdSet(blockId, blockNumber)     \
    ((blockId)->bi_hi = (blockNumber) >> 16, \
     (blockId)->bi_lo = (blockNumber)&0xffff)

#define BlockIdGetBlockNumber(blockId) \
    ((((BlockNumber)(blockId)->bi_hi) << 16) | ((BlockNumber)(blockId)->bi_lo))

typedef uint16_t AttrNumber;

typedef uint16_t OffsetNumber;
#define InvalidOffsetNumber ((OffsetNumber)0)
#define FirstOffsetNumber   ((OffsetNumber)1)

typedef struct ItemPointerData {
    BlockIdData ip_blkid;
    OffsetNumber ip_posid;
} __attribute__((packed)) ItemPointerData;

typedef ItemPointerData* ItemPointer;

#define ItemPointerGetBlockNumber(pointer) \
    (BlockIdGetBlockNumber(&(pointer)->ip_blkid))

#define ItemPointerSet(pointer, blockNumber, offNum) \
    (BlockIdSet(&(pointer)->ip_blkid, blockNumber),  \
     (pointer)->ip_posid = offNum)

#define ItemPointerSetInvalid(pointer)                       \
    (BlockIdSet(&((pointer)->ip_blkid), InvalidBlockNumber), \
     (pointer)->ip_posid = InvalidOffsetNumber)

static inline int ItemPointerCompare(ItemPointer arg1, ItemPointer arg2)
{
    BlockNumber b1 = ItemPointerGetBlockNumber(arg1);
    BlockNumber b2 = ItemPointerGetBlockNumber(arg2);

    if (b1 < b2) return -1;
    if (b1 > b2)
        return 1;
    else if (arg1->ip_posid < arg2->ip_posid)
        return -1;
    else if (arg1->ip_posid > arg2->ip_posid)
        return 1;
    return 0;
}

typedef enum ScanDirection {
    BackwardScanDirection = -1,
    NoMovementScanDirection = 0,
    ForwardScanDirection = 1
} ScanDirection;

typedef struct SnapshotData {
    TransactionId xmin;
    TransactionId xmax;

    TransactionId* xip;
    uint32_t xcnt;
} SnapshotData;

typedef SnapshotData* Snapshot;

__BEGIN_DECLS

Datum datumCopy(Datum value, bool typByVal, int typLen);

__END_DECLS

#endif
