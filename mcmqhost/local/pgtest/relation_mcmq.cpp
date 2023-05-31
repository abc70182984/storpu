#include "config.h"
#include "relation.h"
#include "catalog.h"
#include "libunvme/nvme_driver.h"

static NVMeDriver* g_nvme_driver;

void relation_set_nvme_driver(NVMeDriver* drv) { g_nvme_driver = drv; }

static struct rel_desc {
    Oid oid;
    unsigned int fhandle;
    TupleDesc tupdesc;
} rel_descs[] = {
    {.oid = REL_OID_TABLE_TEST, .fhandle = 2, .tupdesc = &table_test_schema},
    {.oid = REL_OID_INDEX_TEST, .fhandle = 3, .tupdesc = &index_test_schema},

    {.oid = REL_OID_TABLE_TPCH_CUSTOMER,
     .fhandle = 4,
     .tupdesc = &tpch_customer_schema},

    {.oid = REL_OID_TABLE_TPCH_LINEITEM,
     .fhandle = 5,
     .tupdesc = &tpch_lineitem_schema},

    {.oid = REL_OID_TABLE_TPCH_NATION,
     .fhandle = 8,
     .tupdesc = &tpch_nation_schema},

    {.oid = REL_OID_TABLE_TPCH_ORDERS,
     .fhandle = 9,
     .tupdesc = &tpch_orders_schema},

    {.oid = REL_OID_TABLE_TPCH_PARTSUPP,
     .fhandle = 10,
     .tupdesc = &tpch_partsupp_schema},

    {.oid = REL_OID_TABLE_TPCH_PART,
     .fhandle = 11,
     .tupdesc = &tpch_part_schema},

    {.oid = REL_OID_TABLE_TPCH_REGION,
     .fhandle = 12,
     .tupdesc = &tpch_region_schema},

    {.oid = REL_OID_TABLE_TPCH_SUPPLIER,
     .fhandle = 13,
     .tupdesc = &tpch_supplier_schema},

    {.oid = REL_OID_TABLE_TPCC_ORDERS,
     .fhandle = 6,
     .tupdesc = &tpcc_orders1_schema},

    {.oid = REL_OID_INDEX_TPCC_ORDERS,
     .fhandle = 7,
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

extern "C"
{
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
        auto* mem_space = g_nvme_driver->get_dma_space();
        MemorySpace::Address dma_buf = mem_space->allocate_pages(BLCKSZ);
        int r = 0;

        try {
            g_nvme_driver->read(relation->rd_file, (loff_t)page_id * BLCKSZ,
                                dma_buf, BLCKSZ);

            mem_space->read(dma_buf, buf, BLCKSZ);
        } catch (const NVMeDriver::DeviceIOError&) {
            r = -1;
        }

        mem_space->free(dma_buf, BLCKSZ);

        return r;
    }

    Buffer rel_read_buffer(Relation relation, BlockNumber page_id, Buffer buf)
    {
        auto* mem_space = g_nvme_driver->get_dma_space();
        MemorySpace::Address dma_buf = mem_space->allocate_pages(BLCKSZ);
        Buffer r = buf;

        try {
            g_nvme_driver->read(relation->rd_file, (loff_t)page_id * BLCKSZ,
                                dma_buf, BLCKSZ);

            mem_space->read(dma_buf, buf->bufpage, BLCKSZ);
            buf->blkno = page_id;
        } catch (const NVMeDriver::DeviceIOError&) {
            r = nullptr;
        }

        mem_space->free(dma_buf, BLCKSZ);

        return r;
    }

    char* bufpage_alloc(void) { return new char[BLCKSZ]; }

    void bufpage_free(char* page) { delete page; }
}
