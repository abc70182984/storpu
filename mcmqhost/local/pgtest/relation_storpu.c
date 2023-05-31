#include "config.h"
#include "relation.h"
#include "catalog.h"

#include <stdlib.h>
#include <storpu.h>
#include <storpu/thread.h>
#include <storpu/file.h>

static struct rel_desc {
    Oid oid;
    unsigned int fhandle;
    TupleDesc tupdesc;
} rel_descs[] = {
    {.oid = REL_OID_TABLE_TEST, .fhandle = 1, .tupdesc = &table_test_schema},
    {.oid = REL_OID_INDEX_TEST, .fhandle = 2, .tupdesc = &index_test_schema},

    {.oid = REL_OID_TABLE_TPCH_CUSTOMER,
     .fhandle = 3,
     .tupdesc = &tpch_customer_schema},

    {.oid = REL_OID_TABLE_TPCH_LINEITEM,
     .fhandle = 4,
     .tupdesc = &tpch_lineitem_schema},

    {.oid = REL_OID_TABLE_TPCH_NATION,
     .fhandle = 7,
     .tupdesc = &tpch_nation_schema},

    {.oid = REL_OID_TABLE_TPCH_ORDERS,
     .fhandle = 8,
     .tupdesc = &tpch_orders_schema},

    {.oid = REL_OID_TABLE_TPCH_PARTSUPP,
     .fhandle = 9,
     .tupdesc = &tpch_partsupp_schema},

    {.oid = REL_OID_TABLE_TPCH_PART,
     .fhandle = 10,
     .tupdesc = &tpch_part_schema},

    {.oid = REL_OID_TABLE_TPCH_REGION,
     .fhandle = 11,
     .tupdesc = &tpch_region_schema},

    {.oid = REL_OID_TABLE_TPCH_SUPPLIER,
     .fhandle = 12,
     .tupdesc = &tpch_supplier_schema},

    {.oid = REL_OID_TABLE_TPCC_ORDERS,
     .fhandle = 5,
     .tupdesc = &tpcc_orders1_schema},

    {.oid = REL_OID_INDEX_TPCC_ORDERS,
     .fhandle = 6,
     .tupdesc = &tpcc_idx_orders1_schema},
};

static int rel_open_file(Relation relation, Oid relid, unsigned int fhandle,
                         TupleDesc descr)
{
    relation->rd_id = relid;
    relation->rd_file = fhandle;
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
    spu_read(relation->rd_file, buf, BLCKSZ, (unsigned long)page_id * BLCKSZ);
    return 0;
}

Buffer rel_read_buffer(Relation relation, BlockNumber page_id, Buffer buf)
{
    size_t n;

    n = spu_read(relation->rd_file, buf->bufpage, BLCKSZ,
                 (unsigned long)page_id * BLCKSZ);
    if (n != BLCKSZ) return NULL;

    buf->blkno = page_id;

    return buf;
}

void abort() { spu_thread_exit(EXIT_FAILURE); }

char* bufpage_alloc(void)
{
    void* addr =
        mmap(NULL, BLCKSZ, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_CONTIG, -1, 0);

    if (addr == MAP_FAILED) return NULL;
    return addr;
}

void bufpage_free(char* page) { munmap(page, BLCKSZ); }
