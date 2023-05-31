#ifndef _RELATION_H_
#define _RELATION_H_

#include "types.h"
#include "tupdesc.h"
#include "buffer.h"

#include <stdio.h>

typedef struct RelationData {
    Oid rd_id;
    unsigned int rd_file;

    TupleDesc rd_att;
    void* rd_amcache;

    void* rd_opaque;
} RelationData;

typedef RelationData* Relation;

typedef struct TableScanDescData {
    /* scan parameters */
    Relation rs_rd;                   /* heap relation descriptor */
    struct SnapshotData* rs_snapshot; /* snapshot to see */
    int rs_nkeys;                     /* number of scan keys */
    struct ScanKeyData* rs_key;       /* array of scan key descriptors */

    /* Range of ItemPointers for table_scan_getnextslot_tidrange() to scan. */
    /* ItemPointerData rs_mintid; */
    /* ItemPointerData rs_maxtid; */

    /*
     * Information about type and behaviour of the scan, a bitmask of members
     * of the ScanOptions enum (see tableam.h).
     */
    uint32_t rs_flags;

} TableScanDescData;
typedef struct TableScanDescData* TableScanDesc;

__BEGIN_DECLS

int rel_open_relation(Relation rfil, Oid relid);

int rel_read_page(Relation rfil, BlockNumber page_id, char* buf);
Buffer rel_read_buffer(Relation rfil, BlockNumber page_id, Buffer buf);

__END_DECLS

#endif
