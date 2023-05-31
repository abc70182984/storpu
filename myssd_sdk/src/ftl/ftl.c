#include <xil_printf.h>
#include <errno.h>
#include "ff.h"

#include <types.h>
#include <flash_config.h>
#include <flash.h>
#include "../proto.h"
#include <const.h>
#include <timer.h>
#include <slab.h>
#include <bitmap.h>
#include <memalloc.h>
#include <page.h>

#include <histogram.h>

#define NAME "[FTL]"

#define MANIFEST_FILENAME "MANIFEST"
#define MANIFEST_MAGIC    0x4c54464a
#define MANIFEST_VERSION  1

#define IDX2NSID(idx)  ((idx) + 1)
#define NSID2IDX(nsid) ((nsid)-1)

struct namespace_metadata {
    size_t size_blocks;
    size_t capacity_blocks;
    size_t util_blocks;
};

static struct manifest {
    unsigned int magic;
    unsigned int version;

    /* Namespace information */
    unsigned int namespace_max;
    bitchunk_t allocated_namespace[BITCHUNKS(NAMESPACE_MAX)];
    bitchunk_t active_namespace[BITCHUNKS(NAMESPACE_MAX)];
    struct namespace_metadata namespaces[NAMESPACE_MAX];
} manifest;

_Static_assert(NAMESPACE_MAX <= 64,
               "More than 64 namespaces are not supported");

static struct {
    histogram_t read_txns_per_req_hist;
    histogram_t write_txns_per_req_hist;
    histogram_t total_txns_per_req_hist;
    histogram_t ecc_error_blocks_per_req_hist;

    histogram_t read_transfer_time_hist;
    histogram_t write_transfer_time_hist;
    histogram_t read_command_time_hist;
    histogram_t write_command_time_hist;
} stats;

static int save_manifest(void);

int ftl_get_namespace(unsigned int nsid, struct namespace_info* info)
{
    int idx;
    struct namespace_metadata* ns_meta;

    if (nsid <= 0 || nsid > NAMESPACE_MAX) return EINVAL;

    idx = NSID2IDX(nsid);
    if (!GET_BIT(manifest.allocated_namespace, idx)) return ESRCH;

    if (info) {
        ns_meta = &manifest.namespaces[idx];

        info->active = GET_BIT(manifest.active_namespace, idx);
        info->size_blocks = ns_meta->size_blocks;
        info->capacity_blocks = ns_meta->capacity_blocks;
        info->util_blocks = ns_meta->util_blocks;
    }

    return 0;
}

static int segment_user_request(struct user_request* req)
{
    struct flash_transaction *txn, *tmp;
    unsigned count = 0;
    lha_t slba = req->start_lba;
    int r;

    while (count < req->sector_count) {
        unsigned page_offset = slba % SECTORS_PER_FLASH_PG;
        unsigned int txn_size = SECTORS_PER_FLASH_PG - page_offset;
        lpa_t lpa = slba / SECTORS_PER_FLASH_PG;
        page_bitmap_t bitmap;

        if (count + txn_size > req->sector_count)
            txn_size = req->sector_count - count;

        SLABALLOC(txn);
        if (!txn) {
            r = ENOMEM;
            goto cleanup;
        }

        flash_transaction_init(txn);

        bitmap = ~(~0ULL << txn_size);
        bitmap <<= (slba % SECTORS_PER_FLASH_PG);

        txn->req = req;
        txn->type = (req->req_type == IOREQ_READ) ? TXN_READ : TXN_WRITE;
        txn->source = TS_USER_IO;
        txn->nsid = req->nsid;
        txn->lpa = lpa;
        txn->ppa = NO_PPA;
        txn->offset = page_offset << SECTOR_SHIFT;
        txn->length = txn_size << SECTOR_SHIFT;
        txn->bitmap = bitmap;
        list_add_tail(&txn->list, &req->txn_list);

        slba += txn_size;
        count += txn_size;
    }

    return 0;

cleanup:
    list_for_each_entry_safe(txn, tmp, &req->txn_list, list)
    {
        list_del(&txn->list);
        SLABFREE(txn);
    }

    return r;
}

static void record_request_stats(struct user_request* req)
{
    histogram_record(stats.read_txns_per_req_hist,
                     req->stats.total_flash_read_txns);
    histogram_record(stats.write_txns_per_req_hist,
                     req->stats.total_flash_write_txns);
    histogram_record(stats.total_txns_per_req_hist,
                     req->stats.total_flash_read_txns +
                         req->stats.total_flash_write_txns);

    if (req->stats.ecc_error_blocks)
        histogram_record(stats.ecc_error_blocks_per_req_hist,
                         req->stats.ecc_error_blocks);

    if (req->stats.flash_read_transfer_us > 0) {
        histogram_record(stats.read_transfer_time_hist,
                         req->stats.flash_read_transfer_us);
        histogram_record(stats.read_command_time_hist,
                         req->stats.flash_read_command_us);
    }

    if (req->stats.flash_write_transfer_us) {
        histogram_record(stats.write_transfer_time_hist,
                         req->stats.flash_write_transfer_us);
        histogram_record(stats.write_command_time_hist,
                         req->stats.flash_write_command_us);
    }
}

static int process_io_request(struct user_request* req)
{
    struct flash_transaction *txn, *tmp;
    int r;

    r = segment_user_request(req);
    if (r != 0) return r;

    r = dc_process_request(req);

    list_for_each_entry_safe(txn, tmp, &req->txn_list, list)
    {
        list_del(&txn->list);
        SLABFREE(txn);
    }

    record_request_stats(req);

    return r;
}

static int ftl_flush_ns(unsigned int nsid)
{
    dc_flush_ns(nsid);
    amu_save_domain(nsid);
    return 0;
}

static int ftl_flush_data_ns(unsigned int nsid)
{
    dc_flush_ns(nsid);
    return 0;
}

static void ftl_sync(void)
{
    int i, r;

    xil_printf(NAME " Saving metadata for namespace ");

    for (i = 0; i < NAMESPACE_MAX; i++) {
        unsigned int nsid = IDX2NSID(i);

        if (!GET_BIT(manifest.active_namespace, i)) continue;

        xil_printf("%d...", nsid);
        dc_flush_ns(nsid);

        r = amu_save_domain(nsid);
        xil_printf("%s", r ? "FAILED " : "OK ");
    }
    xil_printf("\n");

    bm_shutdown();
    save_manifest();
}

int ftl_process_request(struct user_request* req)
{
    int r;

    switch (req->req_type) {
    case IOREQ_FLUSH:
        r = ftl_flush_ns(req->nsid);
        break;
    case IOREQ_FLUSH_DATA:
        r = ftl_flush_data_ns(req->nsid);
        break;
    case IOREQ_SYNC:
        ftl_sync();
        r = 0;
        break;
    case IOREQ_READ:
    case IOREQ_WRITE:
    case IOREQ_WRITE_ZEROES:
        r = process_io_request(req);
        break;
    default:
        r = EINVAL;
        break;
    }

    return r;
}

static void reset_manifest(void)
{
    memset(&manifest, 0, sizeof(manifest));

    manifest.magic = MANIFEST_MAGIC;
    manifest.version = MANIFEST_VERSION;

    manifest.namespace_max = NAMESPACE_MAX;

    /* Initialize the default namespace. */
    SET_BIT(manifest.allocated_namespace, 0);
    SET_BIT(manifest.active_namespace, 0);
    manifest.namespaces[0].size_blocks =
        CONFIG_STORAGE_CAPACITY_BYTES >> SECTOR_SHIFT;
    manifest.namespaces[0].capacity_blocks =
        CONFIG_STORAGE_CAPACITY_BYTES >> SECTOR_SHIFT;
    manifest.namespaces[0].util_blocks =
        CONFIG_STORAGE_CAPACITY_BYTES >> SECTOR_SHIFT;
}

static int save_manifest(void)
{
    FIL fil;
    UINT bw;
    int rc;

    rc = f_open(&fil, MANIFEST_FILENAME, FA_CREATE_ALWAYS | FA_WRITE);
    if (rc) return EIO;

    rc = f_write(&fil, &manifest, sizeof(manifest), &bw);
    if (rc || bw != sizeof(manifest)) return EIO;

    rc = f_close(&fil);
    return rc > 0 ? EIO : 0;
}

static int restore_manifest(void)
{
    FIL fil;
    u8* buf;
    struct manifest* manifest_buf;
    UINT br;
    int r = EIO, rc;

    rc = f_open(&fil, MANIFEST_FILENAME, FA_READ);
    if (!rc) rc = f_lseek(&fil, 0);
    if (rc) return EIO;

    /* Read manifest into a temporary buffer to avoid screwing up the master
     * copy if anything goes wrong. */
    buf = alloc_vmpages(1, ZONE_PS_DDR);
    manifest_buf = (struct manifest*)buf;

    rc = f_read(&fil, buf, sizeof(manifest), &br);
    if (rc || br != sizeof(manifest)) goto out;

    r = EBADMSG;
    if (manifest_buf->magic != MANIFEST_MAGIC) goto out;

    r = ENOTSUP;
    if (manifest_buf->version != MANIFEST_VERSION) goto out;
    if (manifest_buf->namespace_max != NAMESPACE_MAX) goto out;

    memcpy(&manifest, manifest_buf, sizeof(manifest));

    r = EIO;
    rc = f_close(&fil);
    if (rc) goto out;

    r = 0;
out:
    free_mem(__pa(buf), ARCH_PG_SIZE);

    return r;
}

void ftl_shutdown(int abrupt)
{
    /* Alright, just take our time and write everything so that we can still get
     * them next time ... */
    ftl_sync();
}

void ftl_init(void)
{
    int wipe_manifest = FALSE;
    int wipe_ssd = FALSE;
    int wipe_mt = FALSE;
    int full_scan = FALSE;

    FILINFO fno;
    int i, r;

#ifdef WIPE_SSD
    wipe_ssd = TRUE;
    wipe_mt = TRUE;
#endif

#ifdef WIPE_MANIFEST
    wipe_manifest = TRUE;
#endif

#ifdef WIPE_MAPPING_TABLE
    wipe_mt = TRUE;
#endif

#ifdef FULL_BAD_BLOCK_SCAN
    full_scan = TRUE;
    wipe_mt = TRUE;
#endif

    if (f_stat(MANIFEST_FILENAME, &fno) != 0 || wipe_manifest) {
        xil_printf(NAME " Reseting manifest ...");
        reset_manifest();
        save_manifest();
        xil_printf("OK\n");
    } else {
        xil_printf(NAME " Restoring manifest (%lu bytes) ...", fno.fsize);
        r = restore_manifest();
        if (r) {
            panic(NAME " Failed to restore manifest\n");
        } else {
            xil_printf("OK\n");
        }
    }

    histogram_init(1, UINT64_C(1000), 1, &stats.read_txns_per_req_hist);
    histogram_init(1, UINT64_C(1000), 1, &stats.write_txns_per_req_hist);
    histogram_init(1, UINT64_C(1000), 1, &stats.total_txns_per_req_hist);
    histogram_init(1, UINT64_C(1000), 1, &stats.ecc_error_blocks_per_req_hist);

    histogram_init(1, UINT64_C(1000), 1, &stats.read_transfer_time_hist);
    histogram_init(1, UINT64_C(1000), 1, &stats.write_transfer_time_hist);
    histogram_init(1, UINT64_C(1000), 1, &stats.read_command_time_hist);
    histogram_init(1, UINT64_C(1000), 1, &stats.write_command_time_hist);

    dc_init(CONFIG_DATA_CACHE_CAPACITY);
    bm_init(wipe_ssd, full_scan);

    for (i = 0; i < NAMESPACE_MAX; i++) {
        struct namespace_metadata* ns_meta = &manifest.namespaces[i];

        if (!GET_BIT(manifest.active_namespace, i)) continue;

        r = amu_attach_domain(
            IDX2NSID(i), CONFIG_MAPPING_TABLE_CAPACITY,
            (ns_meta->size_blocks << SECTOR_SHIFT) >> FLASH_PG_SHIFT, wipe_mt);
        if (r) panic("Failed to attach namespace %d\n", IDX2NSID(i));
    }
}

int ftl_create_namespace(struct namespace_info* info)
{
    unsigned int index, nsid;
    struct namespace_metadata* ns_meta;

    index = find_next_zero_bit(manifest.allocated_namespace, NAMESPACE_MAX, 0);
    if (index >= NAMESPACE_MAX) return -ENFILE;

    nsid = IDX2NSID(index);
    ns_meta = &manifest.namespaces[index];

    ns_meta->size_blocks = info->size_blocks;
    ns_meta->capacity_blocks = info->capacity_blocks;
    ns_meta->util_blocks = info->capacity_blocks;

    SET_BIT(manifest.allocated_namespace, index);
    save_manifest();

    return nsid;
}

int ftl_delete_namespace(unsigned int nsid)
{
    unsigned int index;
    int r;

    if (nsid == 0 || nsid > NAMESPACE_MAX) return EINVAL;

    index = NSID2IDX(nsid);

    if (!GET_BIT(manifest.allocated_namespace, index)) return ESRCH;

    if (GET_BIT(manifest.active_namespace, index)) {
        r = ftl_detach_namespace(nsid);
        if (r != 0) return r;
    }

    r = amu_delete_domain(nsid);
    if (r == 0) {
        UNSET_BIT(manifest.allocated_namespace, index);
        save_manifest();
    }

    return r;
}

int ftl_attach_namespace(unsigned int nsid)
{
    unsigned int index;
    struct namespace_metadata* ns_meta;
    int r;

    if (nsid == 0 || nsid > NAMESPACE_MAX) return EINVAL;

    index = NSID2IDX(nsid);
    ns_meta = &manifest.namespaces[index];

    if (!GET_BIT(manifest.allocated_namespace, index)) return ESRCH;
    if (GET_BIT(manifest.active_namespace, index)) return EBUSY;

    r = amu_attach_domain(
        nsid, CONFIG_MAPPING_TABLE_CAPACITY,
        (ns_meta->size_blocks << SECTOR_SHIFT) >> FLASH_PG_SHIFT, FALSE);

    if (r == 0) {
        SET_BIT(manifest.active_namespace, index);
        save_manifest();
    }

    return r;
}

int ftl_detach_namespace(unsigned int nsid)
{
    unsigned int index;
    int r;

    if (nsid == 0 || nsid > NAMESPACE_MAX) return EINVAL;

    index = NSID2IDX(nsid);

    if (!GET_BIT(manifest.active_namespace, index)) return ENOENT;

    r = amu_detach_domain(nsid);
    if (r == 0) {
        UNSET_BIT(manifest.active_namespace, index);
        save_manifest();
    }

    return r;
}

void ftl_report_stats(void)
{
    xil_printf("Read transactions per request\n");
    xil_printf("===========================================\n");
    histogram_print(stats.read_txns_per_req_hist, 2);
    xil_printf("===========================================\n\n");

    xil_printf("Write transactions per request\n");
    xil_printf("===========================================\n");
    histogram_print(stats.write_txns_per_req_hist, 2);
    xil_printf("============================================\n\n");

    xil_printf("Total transactions per request\n");
    xil_printf("===========================================\n");
    histogram_print(stats.total_txns_per_req_hist, 2);
    xil_printf("============================================\n\n");

    xil_printf("ECC error blocks per request\n");
    xil_printf("===========================================\n");
    histogram_print(stats.ecc_error_blocks_per_req_hist, 2);
    xil_printf("============================================\n\n");

    xil_printf("Read transfer time per request (microseconds)\n");
    xil_printf("===========================================\n");
    histogram_print(stats.read_transfer_time_hist, 2);
    xil_printf("===========================================\n\n");

    xil_printf("Read command time per request (microseconds)\n");
    xil_printf("===========================================\n");
    histogram_print(stats.read_command_time_hist, 2);
    xil_printf("===========================================\n\n");

    xil_printf("Write transfer time per request (microseconds)\n");
    xil_printf("===========================================\n");
    histogram_print(stats.write_transfer_time_hist, 2);
    xil_printf("===========================================\n\n");

    xil_printf("Write command time per request (microseconds)\n");
    xil_printf("===========================================\n");
    histogram_print(stats.write_command_time_hist, 2);
    xil_printf("===========================================\n\n");

    dc_report_stats();
    bm_report_stats();
}
