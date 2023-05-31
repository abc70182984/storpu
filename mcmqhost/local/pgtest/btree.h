#ifndef _BTREE_H_
#define _BTREE_H_

#include "config.h"
#include "types.h"
#include "bufpage.h"
#include "index.h"
#include "relation.h"
#include "buffer.h"

#define BT_READ  BUFFER_LOCK_SHARE
#define BT_WRITE BUFFER_LOCK_EXCLUSIVE

#define BTLessStrategyNumber         1
#define BTLessEqualStrategyNumber    2
#define BTEqualStrategyNumber        3
#define BTGreaterEqualStrategyNumber 4
#define BTGreaterStrategyNumber      5

#define BTMaxStrategyNumber 5

typedef struct BTStackData {
    BlockNumber bts_blkno;
    OffsetNumber bts_offset;
    struct BTStackData* bts_parent;
} BTStackData;

typedef BTStackData* BTStack;

typedef struct BTScanInsertData {
    bool heapkeyspace;
    bool pivotsearch;
    bool nextkey;
    ItemPointer scantid;
    int keysz;
    ScanKeyData scankeys[INDEX_MAX_KEYS];
} BTScanInsertData;

typedef BTScanInsertData* BTScanInsert;

typedef struct BTMetaPageData {
    uint32_t btm_magic;       /* should contain BTREE_MAGIC */
    uint32_t btm_version;     /* nbtree version (always <= BTREE_VERSION) */
    BlockNumber btm_root;     /* current root location */
    uint32_t btm_level;       /* tree level of the root page */
    BlockNumber btm_fastroot; /* current "fast" root location */
    uint32_t btm_fastlevel;   /* tree level of the "fast" root page */
    /* remaining fields only valid when btm_version >= BTREE_NOVAC_VERSION */

    /* number of deleted, non-recyclable pages during last cleanup */
    uint32_t btm_last_cleanup_num_delpages;
    /* number of heap tuples during last cleanup (deprecated) */
    uint8_t btm_last_cleanup_num_heap_tuples;

    bool btm_allequalimage; /* are all columns "equalimage"? */
} BTMetaPageData;

#define BTPageGetMeta(p) ((BTMetaPageData*)PageGetContents(p))

typedef struct BTPageOpaqueData {
    BlockNumber btpo_prev; /* left sibling, or P_NONE if leftmost */
    BlockNumber btpo_next; /* right sibling, or P_NONE if rightmost */
    uint32_t btpo_level;   /* tree level --- zero for leaf pages */
    uint16_t btpo_flags;   /* flag bits, see below */
    uint16_t btpo_cycleid; /* vacuum cycle ID of latest split */
} BTPageOpaqueData;

typedef BTPageOpaqueData* BTPageOpaque;

#define BTPageGetOpaque(page) ((BTPageOpaque)PageGetSpecialPointer(page))

/* Bits defined in btpo_flags */
#define BTP_LEAF        (1 << 0) /* leaf page, i.e. not internal page */
#define BTP_ROOT        (1 << 1) /* root page (has no parent) */
#define BTP_DELETED     (1 << 2) /* page has been deleted from tree */
#define BTP_META        (1 << 3) /* meta-page */
#define BTP_HALF_DEAD   (1 << 4) /* empty, but still in tree */
#define BTP_SPLIT_END   (1 << 5) /* rightmost page of split group */
#define BTP_HAS_GARBAGE (1 << 6) /* page has LP_DEAD tuples (deprecated) */
#define BTP_INCOMPLETE_SPLIT                                            \
    (1 << 7)                     /* right sibling's downlink is missing \
                                  */
#define BTP_HAS_FULLXID (1 << 8) /* contains BTDeletedPageData */

#define P_LEFTMOST(opaque)   ((opaque)->btpo_prev == P_NONE)
#define P_RIGHTMOST(opaque)  ((opaque)->btpo_next == P_NONE)
#define P_ISLEAF(opaque)     (((opaque)->btpo_flags & BTP_LEAF) != 0)
#define P_ISROOT(opaque)     (((opaque)->btpo_flags & BTP_ROOT) != 0)
#define P_ISDELETED(opaque)  (((opaque)->btpo_flags & BTP_DELETED) != 0)
#define P_ISMETA(opaque)     (((opaque)->btpo_flags & BTP_META) != 0)
#define P_ISHALFDEAD(opaque) (((opaque)->btpo_flags & BTP_HALF_DEAD) != 0)
#define P_IGNORE(opaque) \
    (((opaque)->btpo_flags & (BTP_DELETED | BTP_HALF_DEAD)) != 0)
#define P_HAS_GARBAGE(opaque) (((opaque)->btpo_flags & BTP_HAS_GARBAGE) != 0)
#define P_INCOMPLETE_SPLIT(opaque) \
    (((opaque)->btpo_flags & BTP_INCOMPLETE_SPLIT) != 0)
#define P_HAS_FULLXID(opaque) (((opaque)->btpo_flags & BTP_HAS_FULLXID) != 0)

#define P_NONE 0

#define P_HIKEY                ((OffsetNumber)1)
#define P_FIRSTKEY             ((OffsetNumber)2)
#define P_FIRSTDATAKEY(opaque) (P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY)

#define INDEX_ALT_TID_MASK 0x2000

/* Item pointer offset bit masks */
#define BT_OFFSET_MASK        0x0FFF
#define BT_STATUS_OFFSET_MASK 0xF000
/* BT_STATUS_OFFSET_MASK status bits */
#define BT_PIVOT_HEAP_TID_ATTR 0x1000
#define BT_IS_POSTING          0x2000

typedef struct BTScanPosItem /* what we remember about each match */
{
    ItemPointerData heapTid;   /* TID of referenced heap item */
    OffsetNumber indexOffset;  /* index item's location within page */
    LocationIndex tupleOffset; /* IndexTuple's offset in workspace, if any */
} BTScanPosItem;

#define MaxTIDsPerBTreePage                                              \
    (int)((BLCKSZ - sizeof(PageHeaderData) - sizeof(BTPageOpaqueData)) / \
          sizeof(ItemPointerData))

typedef struct BTScanPosData {
    Buffer buf;

    BlockNumber currPage; /* page referenced by items array */
    BlockNumber nextPage; /* page's right link when we scanned it */

    /*
     * moreLeft and moreRight track whether we think there may be matching
     * index entries to the left and right of the current page, respectively.
     * We can clear the appropriate one of these flags when _bt_checkkeys()
     * returns continuescan = false.
     */
    bool moreLeft;
    bool moreRight;

    /*
     * If we are doing an index-only scan, nextTupleOffset is the first free
     * location in the associated tuple storage workspace.
     */
    int nextTupleOffset;

    /*
     * The items array is always ordered in index order (ie, increasing
     * indexoffset).  When scanning backwards it is convenient to fill the
     * array back-to-front, so we start at the last slot and fill downwards.
     * Hence we need both a first-valid-entry and a last-valid-entry counter.
     * itemIndex is a cursor showing which entry was last returned to caller.
     */
    int firstItem; /* first valid index in items[] */
    int lastItem;  /* last valid index in items[] */
    int itemIndex; /* current index in items[] */

    BTScanPosItem items[MaxTIDsPerBTreePage]; /* MUST BE LAST */
} BTScanPosData;

typedef BTScanPosData* BTScanPos;

#define BTScanPosIsPinned(scanpos) BufferIsValid((scanpos).buf)

#define BTScanPosUnpin(scanpos)        \
    do {                               \
        ReleaseBuffer((scanpos).buf);  \
        (scanpos).buf = InvalidBuffer; \
    } while (0)
#define BTScanPosUnpinIfPinned(scanpos)                          \
    do {                                                         \
        if (BTScanPosIsPinned(scanpos)) BTScanPosUnpin(scanpos); \
    } while (0)

#define BTScanPosIsValid(scanpos) ((scanpos).currPage != InvalidBlockNumber)

#define BTScanPosInvalidate(scanpos)             \
    do {                                         \
        (scanpos).buf = NULL;                    \
        (scanpos).currPage = InvalidBlockNumber; \
        (scanpos).nextPage = InvalidBlockNumber; \
        (scanpos).nextTupleOffset = 0;           \
    } while (0)

typedef struct BTScanOpaqueData {
    BufferData bufdat;

    int numberOfKeys;
    ScanKey keyData;

    char* currTuples;

    BTScanPosData currPos;
} BTScanOpaqueData;

typedef BTScanOpaqueData* BTScanOpaque;

#define SK_BT_REQFWD  0x00010000 /* required to continue forward scan */
#define SK_BT_REQBKWD 0x00020000 /* required to continue backward scan */

static inline bool BTreeTupleIsPivot(IndexTuple itup)
{
    if ((itup->t_info & INDEX_ALT_TID_MASK) == 0) return false;
    if ((itup->t_tid.ip_posid & BT_IS_POSTING) != 0) return false;
    return true;
}

#define BTreeTupleGetNAtts(itup, rel)                                    \
    ((BTreeTupleIsPivot(itup)) ? (itup->t_tid.ip_posid & BT_OFFSET_MASK) \
                               : rel->rd_att->natts)

static inline ItemPointer BTreeTupleGetHeapTID(IndexTuple itup)
{
    if (BTreeTupleIsPivot(itup)) {
        if (itup->t_tid.ip_posid & BT_PIVOT_HEAP_TID_ATTR)
            return (ItemPointer)((char*)itup + IndexTupleSize(itup) -
                                 sizeof(ItemPointerData));
        return NULL;
    }

    return &itup->t_tid;
}

static inline BlockNumber BTreeTupleGetDownLink(IndexTuple pivot)
{
    return ItemPointerGetBlockNumber(&pivot->t_tid);
}

__BEGIN_DECLS

IndexScanDesc btbeginscan(Relation rel, int nkeys);
void btrescan(IndexScanDesc scan, ScanKey scankey, int nkeys);
bool btgettuple(IndexScanDesc scan, ScanDirection dir);
void btendscan(IndexScanDesc scan);

__END_DECLS

#endif
