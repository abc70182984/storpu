#include "config.h"
#include "relation.h"
#include "catalog.h"

#include <string.h>
#include <stdlib.h>

static struct rel_desc {
    Oid oid;
    const char* fhandle;
    TupleDesc tupdesc;
} rel_descs[] = {
    {.oid = REL_OID_TABLE_TEST,
     .fhandle = "16387",
     .tupdesc = &table_test_schema},
    {.oid = REL_OID_INDEX_TEST,
     .fhandle = "16393",
     .tupdesc = &index_test_schema},

    {.oid = REL_OID_TABLE_TPCH_ORDERS,
     .fhandle = "orders",
     .tupdesc = &tpch_orders_schema},

    {.oid = REL_OID_TABLE_TPCH_CUSTOMER,
     .fhandle = "customer",
     .tupdesc = &tpch_customer_schema},

    {.oid = REL_OID_TABLE_TPCH_LINEITEM,
     .fhandle = "lineitem",
     .tupdesc = &tpch_lineitem_schema},

    {.oid = REL_OID_TABLE_TPCH_NATION,
     .fhandle = "nation",
     .tupdesc = &tpch_nation_schema},

    {.oid = REL_OID_TABLE_TPCH_PARTSUPP,
     .fhandle = "partsupp",
     .tupdesc = &tpch_partsupp_schema},

    {.oid = REL_OID_TABLE_TPCH_PART,
     .fhandle = "part",
     .tupdesc = &tpch_part_schema},

    {.oid = REL_OID_TABLE_TPCH_REGION,
     .fhandle = "region",
     .tupdesc = &tpch_region_schema},

    {.oid = REL_OID_TABLE_TPCH_SUPPLIER,
     .fhandle = "supplier",
     .tupdesc = &tpch_supplier_schema},

    {.oid = REL_OID_TABLE_TPCC_ORDERS,
     .fhandle = "orders1",
     .tupdesc = &tpcc_orders1_schema},

    {.oid = REL_OID_INDEX_TPCC_ORDERS,
     .fhandle = "idx_orders1",
     .tupdesc = &tpcc_idx_orders1_schema},
};

static int rel_open_file(Relation relation, Oid relid, const char* fhandle,
                         TupleDesc descr)
{
    char filename[32];

    relation->rd_id = relid;

    snprintf(filename, sizeof(filename), "%s", fhandle);
    relation->rd_opaque = fopen(filename, "rb");

    if (!relation->rd_opaque) return -1;

    relation->rd_att = descr;
    relation->rd_amcache = NULL;

    return 0;
}

int rel_open_relation(Relation relation, Oid relid)
{
    int i;
    struct rel_desc* desc = NULL;

    for (i = 0; i < sizeof(rel_descs) / sizeof(rel_descs[0]); i++) {
        if (rel_descs[i].oid == relid) desc = &rel_descs[i];
    }

    if (!desc) return -1;

    return rel_open_file(relation, relid, desc->fhandle, desc->tupdesc);
}

int rel_read_page(Relation relation, BlockNumber page_id, char* buf)
{
    int r;
    size_t n;
    FILE* fp = (FILE*)relation->rd_opaque;

    r = fseek(fp, (size_t)page_id * BLCKSZ, SEEK_SET);
    if (r != 0) return r;

    n = fread(buf, 1, BLCKSZ, fp);
    return 0;
}

Buffer rel_read_buffer(Relation relation, BlockNumber page_id, Buffer buf)
{
    int r;
    size_t n;
    FILE* fp = (FILE*)relation->rd_opaque;

    r = fseek(fp, (size_t)page_id * BLCKSZ, SEEK_SET);
    if (r != 0) return NULL;

    n = fread(buf->bufpage, 1, BLCKSZ, fp);
    if (n != BLCKSZ) return NULL;

    buf->blkno = page_id;
    return buf;
}

char* bufpage_alloc(void) { return malloc(BLCKSZ); }

void bufpage_free(char* page) { free(page); }
