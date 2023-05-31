#include <xil_assert.h>
#include <errno.h>

#include <config.h>
#include <types.h>
#include <avl.h>
#include <bitmap.h>
#include "../proto.h"
#include <iov_iter.h>
#include "../tls.h"
#include <flash.h>
#include <flash_config.h>
#include <const.h>
#include <utils.h>
#include <page.h>
#include <timer.h>
#include <dma.h>
#include <memalloc.h>
#include <slab.h>
#include <intr.h>
#include "../thread.h"

#define DATA_CACHE_USE_PS_DDR 0

#define WB_BATCH_SIZE 16

static DEFINE_TLS(struct iovec, req_iovecs[1 << MAX_DATA_TRANSFER_SIZE]);
static DEFINE_TLS(struct flash_transaction, writeback_batch[WB_BATCH_SIZE]);

struct cache_stats {
    size_t total_read_hits;
    size_t total_read_misses;
};

enum cache_entry_status {
    CES_EMPTY,
    CES_CLEAN,
    CES_DIRTY,
};

struct cache_entry_key {
    lpa_t lpa;
    unsigned int nsid;
};

struct cache_entry {
    struct cache_entry_key key;
    page_bitmap_t bitmap;
    enum cache_entry_status status;
    struct avl_node avl;
    struct list_head lru;
    unsigned int pin_count;
    mutex_t mutex;
    void* data;
};

struct data_cache {
    size_t capacity_pages;
    size_t nr_pages;
    struct avl_root root;
    struct list_head lru_list;
    struct cache_stats stats;
};

static struct data_cache g_cache;

struct flusher_control {
    struct worker_thread* flusher;
    struct data_cache* cache;
    unsigned int nsid;
};

static bitchunk_t flusher_active[BITCHUNKS(NR_FLUSHERS)];
static struct flusher_control flusher_control[NR_FLUSHERS];
static mutex_t flusher_mutex;
static cond_t flusher_cond;

static inline struct data_cache* get_cache_for_ns(unsigned int nsid)
{
    /* TODO: multiple namespaces */
    return &g_cache;
}

static inline struct data_cache*
get_cache_for_txn(struct flash_transaction* txn)
{
    return get_cache_for_ns(txn->nsid);
}

static int cache_cmp_key(struct cache_entry_key* k1, struct cache_entry_key* k2)
{
    if (k1->nsid < k2->nsid) return -1;
    if (k1->nsid > k2->nsid) return 1;

    if (k1->lpa < k2->lpa)
        return -1;
    else if (k1->lpa > k2->lpa)
        return 1;
    return 0;
}

static int cache_key_node_comp(void* key, struct avl_node* node)
{
    struct cache_entry* r1 = (struct cache_entry*)key;
    struct cache_entry* r2 = avl_entry(node, struct cache_entry, avl);

    return cache_cmp_key(&r1->key, &r2->key);
}

static int cache_node_node_comp(struct avl_node* node1, struct avl_node* node2)
{
    struct cache_entry* r1 = avl_entry(node1, struct cache_entry, avl);
    struct cache_entry* r2 = avl_entry(node2, struct cache_entry, avl);

    return cache_cmp_key(&r1->key, &r2->key);
}

static inline void cache_avl_start_iter(struct data_cache* cache,
                                        struct avl_iter* iter, void* key,
                                        int flags)
{
    avl_start_iter(&cache->root, iter, key, flags);
}

static inline struct cache_entry* cache_avl_get_iter(struct avl_iter* iter)
{
    struct avl_node* node = avl_get_iter(iter);
    if (!node) return NULL;
    return avl_entry(node, struct cache_entry, avl);
}

static void cache_init(struct data_cache* cache, size_t capacity_pages)
{
    cache->capacity_pages = capacity_pages;
    cache->nr_pages = 0;
    INIT_LIST_HEAD(&cache->lru_list);
    INIT_AVL_ROOT(&cache->root, cache_key_node_comp, cache_node_node_comp);
    memset(&cache->stats, 0, sizeof(cache->stats));

    if (mutex_init(&flusher_mutex, NULL) != 0) {
        panic("failed to initialize flusher mutex");
    }
    if (cond_init(&flusher_cond, NULL) != 0) {
        panic("failed to initialize flusher condvar");
    }
}

static struct cache_entry* cache_find(struct data_cache* cache,
                                      unsigned int nsid, lpa_t lpa)
{
    struct avl_node* node = cache->root.node;
    struct cache_entry* entry = NULL;
    struct cache_entry_key key;
    int cmp;

    key.nsid = nsid;
    key.lpa = lpa;

    while (node) {
        entry = avl_entry(node, struct cache_entry, avl);
        cmp = cache_cmp_key(&key, &entry->key);

        if (!cmp) {
            return entry;
        } else if (cmp < 0) {
            node = node->left;
        } else {
            node = node->right;
        }
    }

    return NULL;
}

static inline void add_entry(struct data_cache* cache,
                             struct cache_entry* entry)
{
    avl_insert(&entry->avl, &cache->root);
    cache->nr_pages++;
}

/* These two functions should not be used directly because any cache entry in
 * the LRU list can be evicted. If used incorrectly, a cache entry may be added
 * back to the LRU list and evicted when someone is using it. Use pin_entry()
 * and unpin_entry() so that only unpinned entries will be added to LRU. */
static inline void add_lru(struct data_cache* cache, struct cache_entry* entry)
{
    list_add(&entry->lru, &cache->lru_list);
}

static inline void remove_lru(struct cache_entry* entry)
{
    list_del(&entry->lru);
    INIT_LIST_HEAD(&entry->lru);
}

static inline void pin_entry(struct cache_entry* entry)
{
    if (entry->pin_count++ == 0) remove_lru(entry);
}

static inline void unpin_entry(struct data_cache* cache,
                               struct cache_entry* entry)
{
    Xil_AssertVoid(entry->pin_count > 0);
    if (--entry->pin_count == 0) add_lru(cache, entry);
}

static void cache_touch_lru(struct data_cache* cache, struct cache_entry* entry)
{
    pin_entry(entry);
    unpin_entry(cache, entry);
}

static int cache_add(struct data_cache* cache, unsigned int nsid, lpa_t lpa,
                     page_bitmap_t bitmap, struct cache_entry** entrypp)
{
    struct cache_entry* entry;
    mutexattr_t mutex_attr = {
        .tag = LKT_DATA_CACHE,
    };

    if (cache->nr_pages >= cache->capacity_pages) return ENOSPC;

    SLABALLOC(entry);
    if (!entry) return ENOMEM;

    memset(entry, 0, sizeof(*entry));
    entry->key.nsid = nsid;
    entry->key.lpa = lpa;
    entry->bitmap = bitmap;
    entry->status = CES_DIRTY;
    /* New entry has pin count = 1, will be released when write_buffers()
     * finishes. */
    entry->pin_count = 1;
    INIT_LIST_HEAD(&entry->lru);
    mutex_init(&entry->mutex, &mutex_attr);

    /* Allocate data buffer for the entry. Prefer PL DDR. */
    entry->data = alloc_vmpages(FLASH_PG_SIZE >> ARCH_PG_SHIFT,
                                DATA_CACHE_USE_PS_DDR ? ZONE_ALL : ZONE_PL_DDR);
    if (!entry->data) {
        SLABFREE(entry);
        return ENOMEM;
    }

    add_entry(cache, entry);
    *entrypp = entry;

    return 0;
}

static int cache_evict_entry(struct data_cache* cache,
                             struct cache_entry** entrypp)
{
    struct cache_entry* entry;

    if (list_empty(&cache->lru_list)) return ENOMEM;

    entry = list_entry(cache->lru_list.prev, struct cache_entry, lru);

    /* Pinned entry should NEVER be evicted. */
    Xil_AssertNonvoid(entry->pin_count == 0);

    list_del(&entry->lru);
    avl_erase(&entry->avl, &cache->root);
    cache->nr_pages--;

    *entrypp = entry;
    return 0;
}

static int generate_writeback(struct flash_transaction* txn,
                              struct data_cache* cache,
                              struct cache_entry* entry)
{
    void* data;
    unsigned long i;

    /* We cannot use entry->data directly because it is allocated from PL DDR
     * but FIL only has access to PS low DDR. */
    data =
        alloc_vmpages(FLASH_PG_BUFFER_SIZE >> ARCH_PG_SHIFT, ZONE_PS_DDR_LOW);
    if (!data) return ENOMEM;

    for (i = 0; i < (FLASH_PG_SIZE >> SECTOR_SHIFT); i++) {
        if (entry->bitmap & (1UL << i)) {
            memcpy(data + (i << SECTOR_SHIFT),
                   entry->data + (i << SECTOR_SHIFT), SECTOR_SIZE);
        }
    }

    dma_sync_single_for_device(data, FLASH_PG_SIZE, DMA_TO_DEVICE);

    flash_transaction_init(txn);
    txn->type = TXN_WRITE;
    txn->source = TS_USER_IO;
    txn->nsid = entry->key.nsid;
    txn->lpa = entry->key.lpa;
    txn->ppa = NO_PPA;
    txn->data = data;
    txn->offset = 0;
    txn->length = FLASH_PG_SIZE;
    txn->bitmap = entry->bitmap;

    return 0;
}

static int write_buffers(struct user_request* req, int write_zeroes)
{
    /* Write a user request to the data cache. */
    struct flash_transaction *txn, *tmp;
    struct iovec* iov = get_local_var(req_iovecs);
    struct iov_iter iter;
    size_t count;
    struct list_head wb_txns;
    int i, r;

    INIT_LIST_HEAD(&wb_txns);

    i = 0;
    count = 0;
    /* Setup IO vectors for DMA. */
    list_for_each_entry(txn, &req->txn_list, list)
    {
#if USE_WRITE_CACHE
        struct data_cache* cache = get_cache_for_txn(txn);
        struct cache_entry* entry = cache_find(cache, txn->nsid, txn->lpa);
#endif

        if (i >= (1 << MAX_DATA_TRANSFER_SIZE)) {
            r = ENOMEM;
            goto cleanup;
        }

#if USE_WRITE_CACHE
        /* With the volatile write cache enabled, we need to fetch the cache
         * entries for all flash pages modified by the transactions and lock
         * them. After that, data is directly transferred from the host into
         * cache entries. */

        if (entry) {
            /* If the entry already exists then just lock it. */
            pin_entry(entry);
            mutex_lock(&entry->mutex);
            entry->bitmap |= txn->bitmap;
            entry->status = CES_DIRTY;

            Xil_AssertNonvoid(entry->key.lpa == txn->lpa);
        } else {
            /* Try to add a new entry. */
            r = cache_add(cache, txn->nsid, txn->lpa, txn->bitmap, &entry);

            if (r == ENOSPC) {
                /* Cache is full. */
                r = cache_evict_entry(cache, &entry);
                if (r != 0) goto cleanup;

                if (entry->status == CES_DIRTY) {
                    /* Cache writeback. */
                    struct flash_transaction* wb_txn;

                    SLABALLOC(wb_txn);
                    if (!wb_txn) {
                        r = ENOMEM;
                        goto cleanup;
                    }

                    r = generate_writeback(wb_txn, cache, entry);
                    if (r) {
                        SLABFREE(wb_txn);
                        goto cleanup;
                    }

                    wb_txn->req = txn->req; /* For accounting */
                    list_add_tail(&wb_txn->list, &wb_txns);
                }

                /* XXX: maybe reset the status after the page is written (i.e.
                 * after amu_dispatch) for better consistency. */
                entry->status = CES_DIRTY;
                entry->key.nsid = txn->nsid;
                entry->key.lpa = txn->lpa;
                entry->bitmap = txn->bitmap;
                add_entry(cache, entry);

                pin_entry(entry);
                mutex_lock(&entry->mutex);
            } else if (r != 0) {
                /* Error. */
                goto cleanup;
            } else {
                /* Ok. */
                Xil_AssertNonvoid(entry->pin_count == 1);
                mutex_lock(&entry->mutex);
            }
        }

        txn->opaque = entry;

        iov->iov_base = entry->data + txn->offset;
        iov->iov_len = txn->length;

#if DATA_CACHE_USE_PS_DDR
        dma_sync_single_for_device(iov->iov_base, iov->iov_len,
                                   DMA_FROM_DEVICE);
#endif

        if (write_zeroes) memset(iov->iov_base, 0, iov->iov_len);

#else
        /* Without the write cache, we allocate a temporary buffer for each
         * transaction in the request and copy the data into the temporary
         * buffers. */

        txn->data = alloc_vmpages(FLASH_PG_BUFFER_SIZE >> ARCH_PG_SHIFT,
                                  ZONE_PS_DDR_LOW);
        if (txn->data == NULL) {
            r = ENOMEM;
            goto cleanup;
        }

        if (write_zeroes) memset(txn->data, 0, FLASH_PG_BUFFER_SIZE);

        /* Cache maintenance: we only need to invalidate cachelines of the
         * newly-allocated buffer here to prevent dirty cachelines from being
         * flushed to memory when we are reading the data from the host. Other
         * than that no further cache maintenance is needed because the CPU does
         * not need to touch the read data. */

        dma_sync_single_for_device(txn->data, FLASH_PG_BUFFER_SIZE,
                                   DMA_FROM_DEVICE);

        iov->iov_base = txn->data + txn->offset;
        iov->iov_len = txn->length;

#endif

        iov++;
        i++;
        count += txn->length;
    }

    /* Cache maintenance:
     * Data is always read into the data cache which is on non-cacheable memory
     * so no cache maintenance is needed. For writebacks, buffers are flushed in
     * generate_writeback(). */

    /* Dispatch writebacks first. */
    if (!list_empty(&wb_txns)) {
        r = amu_dispatch(&wb_txns);
        if (r) goto cleanup;
    }

    if (likely(!write_zeroes)) {
        /* At this point, all cache entries for the request are allocated,
         * locked and detached from LRU so that they will not be evicted until
         * we finish. */
        iov_iter_init(&iter, get_local_var(req_iovecs), i, count);

        /* Read data into data buffers. */
        r = nvme_dma_read(req, &iter, count, count);
        if (r) goto cleanup;
    }

#if !USE_WRITE_CACHE
    /* Perform writes immediately if write cache is disabled. */
    r = amu_dispatch(&req->txn_list);
    if (r) goto cleanup;
#endif

cleanup:
    list_for_each_entry_safe(txn, tmp, &wb_txns, list)
    {
        list_del(&txn->list);
        if (txn->data) free_mem(__pa(txn->data), FLASH_PG_BUFFER_SIZE);
        SLABFREE(txn);
    }

    list_for_each_entry(txn, &req->txn_list, list)
    {
#if USE_WRITE_CACHE

        struct cache_entry* entry = (struct cache_entry*)txn->opaque;
        struct data_cache* cache = get_cache_for_txn(txn);

#if DATA_CACHE_USE_PS_DDR
        dma_sync_single_for_cpu(entry->data + txn->offset, txn->length,
                                DMA_FROM_DEVICE);
#endif

        /* Reset entry status on failure. */
        if (r != 0) {
            entry->status = CES_EMPTY;
            avl_erase(&entry->avl, &cache->root);
        }

        mutex_unlock(&entry->mutex);
        unpin_entry(cache, entry);

#else
        /* Release temporary buffers. */
        if (txn->data) free_mem(__pa(txn->data), FLASH_PG_BUFFER_SIZE);
#endif
    }

    return r;
}

static int handle_cached_read(struct user_request* req)
{
    struct flash_transaction *txn, *tmp;
    struct iovec* iov = get_local_var(req_iovecs);
    struct iov_iter iter;
    struct list_head hit_list;
    size_t count;
    timestamp_t now = timer_get_cycles();
    int i, r;

    /* All flash transactions with a cache hit are moved to this list. */
    INIT_LIST_HEAD(&hit_list);

    i = 0;
    count = 0;
    /* Setup IO vectors for DMA. */
    list_for_each_entry_safe(txn, tmp, &req->txn_list, list)
    {
        struct data_cache* cache = get_cache_for_txn(txn);
        struct cache_entry* entry = cache_find(cache, txn->nsid, txn->lpa);
        page_bitmap_t avail_sectors = 0;

        txn->stats.dc_begin_service_time = now;

        if (entry) avail_sectors = entry->bitmap & txn->bitmap;

        if (entry && avail_sectors == txn->bitmap) {
            /* Cache hit and all requested sectors are present. */
            pin_entry(entry);
            mutex_lock(&entry->mutex);
            txn->data = entry->data;
            txn->opaque = entry;

            list_del(&txn->list);
            list_add(&txn->list, &hit_list);

            cache->stats.total_read_hits++;
        } else {
            /* Cache miss or some requested sectors are not present. In either
             * case, we need to do a flash read to get the missing sectors. */

            /* Allocate data buffer from PS DDR low because we need to read NAND
             * data. */
            txn->data = alloc_vmpages(FLASH_PG_BUFFER_SIZE >> ARCH_PG_SHIFT,
                                      ZONE_PS_DDR_LOW);
            if (txn->data == NULL) {
                r = ENOMEM;
                goto cleanup;
            }

            txn->bitmap &= ~avail_sectors;
            txn->opaque = NULL;

            if (avail_sectors) {
                /* If there are available sectors we need to overlay
                 * cached data on top of the data read from NAND
                 * flash later. */
                pin_entry(entry);
                mutex_lock(&entry->mutex);
                txn->opaque = entry;
            } else if (entry) {
                cache_touch_lru(cache, entry);
            }

            /* Cache maintenance:
             * 1) For pages with cache hits, no cache maintenance is needed
             * because they are on non-cacheable memory.
             *
             * 2) For pages with cache misses, we only need to invalidate
             * cachelines of the newly-allocated buffer here to prevent dirty
             * cachelines from being flushed to memory when we are reading the
             * data from NAND flash. Other than that no further cache
             * maintenance is needed because the CPU does not need to touch the
             * read data. */
            dma_sync_single_for_device(txn->data, FLASH_PG_BUFFER_SIZE,
                                       DMA_FROM_DEVICE);

            cache->stats.total_read_misses++;
        }

        iov->iov_base = txn->data + txn->offset;
        iov->iov_len = txn->length;

#if DATA_CACHE_USE_PS_DDR
        dma_sync_single_for_device(iov->iov_base, iov->iov_len, DMA_TO_DEVICE);
#endif

        iov++;
        i++;
        count += txn->length;
    }

    /* Read data from NAND flash before we overlay the cached sectors. */
    r = amu_dispatch(&req->txn_list);
    if (r) goto cleanup;

    list_for_each_entry(txn, &req->txn_list, list)
    {
        /* Copy usable sectors from cached page. */
        struct cache_entry* entry = (struct cache_entry*)txn->opaque;
        struct data_cache* cache = get_cache_for_txn(txn);
        struct iovec copy_iov[SECTORS_PER_FLASH_PG];
        struct iov_iter copy_iter;
        ssize_t copied_bytes;
        int i;

        if (!entry) continue;

        if (r == 0) {
            for (i = (txn->offset >> SECTOR_SHIFT);
                 (i < SECTORS_PER_FLASH_PG) &&
                 (i < (txn->offset + txn->length) >> SECTOR_SHIFT);
                 i++) {

                copy_iov[i].iov_base = NULL;
                copy_iov[i].iov_len = SECTOR_SIZE;

                if (entry->bitmap & (1UL << i)) {
                    copy_iov[i].iov_base = txn->data + (i << SECTOR_SHIFT);
                }
            }

            iov_iter_init(&copy_iter, &copy_iov[txn->offset >> SECTOR_SHIFT],
                          txn->length >> SECTOR_SHIFT, txn->length);
            copied_bytes =
                zdma_iter_copy_to(&copy_iter, entry->data + txn->offset,
                                  txn->length, DATA_CACHE_USE_PS_DDR);

            if (copied_bytes < 0) {
                r = -copied_bytes;
            }
        }

        mutex_unlock(&entry->mutex);
        unpin_entry(cache, entry);
    }

    /* At this point, all cache entries for the request are locked and detached
     * from LRU so that they will not be evicted until we finish. Data buffers
     * for missed pages are also allocated. */
    iov_iter_init(&iter, get_local_var(req_iovecs), i, count);

    /* Write data into user buffers. */
    r = nvme_dma_write(req, &iter, count, count);

cleanup:
    list_for_each_entry_safe(txn, tmp, &hit_list, list)
    {
        struct cache_entry* entry = (struct cache_entry*)txn->opaque;
        struct data_cache* cache = get_cache_for_txn(txn);

#if DATA_CACHE_USE_PS_DDR
        dma_sync_single_for_cpu(entry->data + txn->offset, txn->length,
                                DMA_TO_DEVICE);
#endif

        /* Cache hit. Release the lock now. */
        mutex_unlock(&entry->mutex);
        unpin_entry(cache, entry);

        list_del(&txn->list);
        SLABFREE(txn);
    }

    list_for_each_entry(txn, &req->txn_list, list)
    {
        /* Cache miss. Release the data buffer. */
        if (txn->data) free_mem(__pa(txn->data), FLASH_PG_BUFFER_SIZE);
    }

    return r;
}

int dc_process_request(struct user_request* req)
{
    int r;

    switch (req->req_type) {
    case IOREQ_READ:
        r = handle_cached_read(req);
        break;
    case IOREQ_WRITE:
        r = write_buffers(req, FALSE);
        break;
    case IOREQ_WRITE_ZEROES:
        r = write_buffers(req, TRUE);
        break;
    default:
        r = EINVAL;
        break;
    }

    return r;
}

void dc_init(size_t capacity)
{
    cache_init(&g_cache, capacity / FLASH_PG_SIZE);
}

static unsigned int cache_lookup_range(struct data_cache* cache,
                                       unsigned int nsid, lpa_t* offset,
                                       lpa_t end, unsigned int tag,
                                       struct cache_entry** entries,
                                       unsigned int nr_entries)
{
    struct cache_entry *entry, key;
    struct avl_iter iter;
    unsigned int ret = 0;

    if (!nr_entries) return 0;

    key.key.nsid = nsid;
    key.key.lpa = *offset;

    cache_avl_start_iter(cache, &iter, &key, AVL_GREATER_EQUAL);
    for (entry = cache_avl_get_iter(&iter); entry;) {
        if ((entry->key.nsid != nsid) || (entry->key.lpa >= end)) break;
        if ((entry->status != CES_DIRTY) ||
            ((entry->key.lpa % NR_FLUSHERS) != tag))
            goto next;

        entries[ret] = entry;
        if (++ret == nr_entries) {
            *offset = entry->key.lpa + 1;
            return ret;
        }

    next:
        avl_inc_iter(&iter);
        entry = cache_avl_get_iter(&iter);
    }

    if (end == (lpa_t)-1)
        *offset = (lpa_t)-1;
    else
        *offset = end;

    return ret;
}

static void flush_cache_range(struct data_cache* cache, unsigned int nsid,
                              unsigned int tag, lpa_t start, lpa_t end)
{
    struct flash_transaction *txn, *tmp;
    struct flash_transaction* wb_batch = get_local_var(writeback_batch);
    struct cache_entry* pvec[WB_BATCH_SIZE];
    struct list_head wb_txns;
    unsigned int nr_entries;
    lpa_t index = start;
    unsigned int count;
    int i;

    /* In each iteration, we try to flush a batch of cached pages so as not to
     * lock too many pages and block foreground workers for too long. */
    while (index < end) {
        nr_entries = cache_lookup_range(cache, nsid, &index, end, tag, pvec,
                                        WB_BATCH_SIZE);
        if (nr_entries == 0) break;

        INIT_LIST_HEAD(&wb_txns);
        count = 0;

        for (i = 0; i < nr_entries; i++) {
            struct cache_entry* entry = pvec[i];
            struct flash_transaction* wb_txn;
            int r;

            /* The cache entry has been modified since we retrieve it from the
             * data cache, either flushed by a concurrent thread or reused for a
             * different LPA. */
            if ((entry->status != CES_DIRTY) || (entry->key.nsid != nsid) ||
                (entry->key.lpa < start) || (entry->key.lpa >= end) ||
                ((entry->key.lpa % NR_FLUSHERS) != tag))
                continue;

            pin_entry(entry);
            mutex_lock(&entry->mutex);

            wb_txn = &wb_batch[count];

            r = generate_writeback(wb_txn, cache, entry);
            if (r) {
                continue;
            }

            count++;
            list_add_tail(&wb_txn->list, &wb_txns);

            wb_txn->opaque = entry;
        }

        amu_dispatch(&wb_txns);

        list_for_each_entry_safe(txn, tmp, &wb_txns, list)
        {
            if (txn->opaque) {
                struct cache_entry* entry = (struct cache_entry*)txn->opaque;
                entry->status = CES_CLEAN;
                mutex_unlock(&entry->mutex);
                unpin_entry(cache, entry);
            }

            list_del(&txn->list);
            if (txn->data) free_mem(__pa(txn->data), FLASH_PG_BUFFER_SIZE);
        }
    }
}

static void start_flush_sync(struct data_cache* cache, unsigned int nsid)
{
    static struct data_cache* flushing = NULL;
    int i;

    mutex_lock(&flusher_mutex);

    while (flushing)
        cond_wait(&flusher_cond, &flusher_mutex);
    flushing = cache;

    for (i = 0; i < NR_FLUSHERS; i++) {
        struct flusher_control* ctrl = &flusher_control[i];
        Xil_AssertVoid(!ctrl->cache);
        Xil_AssertVoid(!GET_BIT(flusher_active, i));

        ctrl->cache = cache;
        ctrl->nsid = nsid;
        SET_BIT(flusher_active, i);

        worker_wake(ctrl->flusher, WT_BLOCKED_ON_FLUSH);
    }

    while (find_first_bit(flusher_active, NR_FLUSHERS) < NR_FLUSHERS)
        cond_wait(&flusher_cond, &flusher_mutex);

    flushing = NULL;
    cond_broadcast(&flusher_cond);

    mutex_unlock(&flusher_mutex);
}

void dc_flush_ns(unsigned int nsid)
{
    struct data_cache* cache = get_cache_for_ns(nsid);
    start_flush_sync(cache, nsid);
}

void flusher_main(int index)
{
    struct worker_thread* self = worker_self();
    struct flusher_control* ctrl = &flusher_control[index];

    ctrl->flusher = self;

    local_irq_enable();

    for (;;) {
        if (!GET_BIT(flusher_active, index)) {
            worker_wait(WT_BLOCKED_ON_FLUSH);
            continue;
        }

        Xil_AssertVoid(ctrl->cache);
        flush_cache_range(ctrl->cache, ctrl->nsid, index, 0, (lpa_t)-1);

        mutex_lock(&flusher_mutex);
        UNSET_BIT(flusher_active, index);
        ctrl->cache = NULL;
        cond_broadcast(&flusher_cond);
        mutex_unlock(&flusher_mutex);
    }
}

void dc_report_stats(void)
{
    xil_printf("=============== Data cache ===============\n");
    xil_printf("  Total read hits: %lu\n", g_cache.stats.total_read_hits);
    xil_printf("  Total read misses: %lu\n", g_cache.stats.total_read_misses);
    xil_printf("==========================================\n");
}
