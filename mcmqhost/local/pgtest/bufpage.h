#ifndef _BUFPAGE_H_
#define _BUFPAGE_H_

#include "types.h"
#include <stdint.h>

typedef uint16_t LocationIndex;

typedef struct ItemIdData {
    unsigned lp_off : 15, /* offset to tuple (from start of page) */
        lp_flags : 2,     /* state of line pointer, see below */
        lp_len : 15;      /* byte length of tuple */
} ItemIdData;

typedef ItemIdData* ItemId;

#define LP_UNUSED   0 /* unused (should always have lp_len=0) */
#define LP_NORMAL   1 /* used (should always have lp_len>0) */
#define LP_REDIRECT 2 /* HOT redirect (should have lp_len=0) */
#define LP_DEAD     3 /* dead, may or may not have storage */

#define ItemIdIsNormal(itemId) ((itemId)->lp_flags == LP_NORMAL)

#define ItemIdIsRedirect(itemId) ((itemId)->lp_flags == LP_REDIRECT)

typedef struct {
    uint32_t xlogid;  /* high bits */
    uint32_t xrecoff; /* low bits */
} PageXLogRecPtr;

typedef struct PageHeaderData {
    /* XXX LSN is member of *any* block, not only page-organized ones */
    PageXLogRecPtr pd_lsn;    /* LSN: next byte after last byte of xlog
                               * record for last change to this page */
    uint16_t pd_checksum;     /* checksum */
    uint16_t pd_flags;        /* flag bits, see below */
    LocationIndex pd_lower;   /* offset to start of free space */
    LocationIndex pd_upper;   /* offset to end of free space */
    LocationIndex pd_special; /* offset to start of special space */
    uint16_t pd_pagesize_version;
    TransactionId pd_prune_xid; /* oldest prunable XID, or zero if none */
    ItemIdData pd_linp[];       /* line pointer array */
} PageHeaderData;

typedef PageHeaderData* PageHeader;

#define PageGetItemId(page, offsetNumber) \
    ((ItemId)(&((PageHeader)(page))->pd_linp[(offsetNumber)-1]))

#define PageGetMaxOffsetNumber(page)                                    \
    (((PageHeader)(page))->pd_lower <= sizeof(PageHeaderData)           \
         ? 0                                                            \
         : ((((PageHeader)(page))->pd_lower - sizeof(PageHeaderData)) / \
            sizeof(ItemIdData)))

#define PageGetItem(page, itemId) (void*)(((char*)(page)) + (itemId)->lp_off)

#define PageGetContents(page) ((char*)(page) + MAXALIGN(sizeof(PageHeaderData)))

#define PageGetSpecialPointer(page) \
    (char*)((char*)(page) + ((PageHeader)(page))->pd_special)

__BEGIN_DECLS

char* bufpage_alloc(void);
void bufpage_free(char* page);

__END_DECLS

#endif
