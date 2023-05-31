/* Address Mapping Unit */
#include "xil_assert.h"
#include "xil_printf.h"
#include "ff.h"

#include <flash_config.h>
#include <list.h>
#include <flash.h>
#include <types.h>
#include <utils.h>
#include "../proto.h"
#include <const.h>
#include <avl.h>
#include "../thread.h"
#include <page.h>
#include <timer.h>
#include <memalloc.h>
#include <slab.h>
#include <dma.h>
#include <iov_iter.h>
#include <kref.h>

#include <stdio.h>
#include <stdint.h>
#include <errno.h>

#define NAME "[AMU]"

#define GTD_FILENAME "gtd_ns%d.bin"

#define PAGES_PER_PLANE   (PAGES_PER_BLOCK * BLOCKS_PER_PLANE)
#define PAGES_PER_DIE     (PAGES_PER_PLANE * PLANES_PER_DIE)
#define PAGES_PER_CHIP    (PAGES_PER_DIE * DIES_PER_CHIP)
#define PAGES_PER_CHANNEL (PAGES_PER_CHIP * CHIPS_ENABLED_PER_CHANNEL)

#define IDX2NSID(idx)  ((idx) + 1)
#define NSID2IDX(nsid) ((nsid)-1)

/* Mapping virtual & physical page number. */
typedef uint32_t mvpn_t;
typedef uint32_t mppn_t;
#define NO_MPPN UINT32_MAX

#define XLATE_PG_SIZE                             \
    (FLASH_PG_SIZE * sizeof(struct xlate_entry) / \
     sizeof(struct xlate_entry_disk))

/* Translation entry */
/* Generally, flash page size (16k) can be larger than sector size (512B or 4k).
 * Thus, the host can write only a subset of sectors in a flash page. In that
 * case, we use the bitmap to record which sectors in a physical flash page are
 * valid. Upon writing a flash page, we check the sectors to be written with
 * sectors in the bitmap. If the set of sectors to be written covers all sectors
 * in the bitmap (which means all valid sectors stored in the old physical
 * page will be written), then we can skip reading the old PP. Otherwise reading
 * the old PP is required. */
struct xlate_entry {
    ppa_t ppa;
    uint32_t bitmap;
};

/* Strip page bitmap when an entry is saved on the disk to save space. Because
 * of this, the sizes of on-disk translation pages and in-memory translation
 * pages are different, which are represented as FLASH_PG_SIZE and
 * XLATE_PG_SIZE, respectively. */
struct xlate_entry_disk {
    ppa_t ppa;
};

/* Cached translation page. Contains (domain->xlate_ents_per_page) entries. */
struct xlate_page {
    mvpn_t mvpn;
    struct xlate_entry* entries;
    int dirty;
    mutex_t mutex;
    struct avl_node avl;
    struct list_head lru;
    unsigned int pin_count;
};

/* Translation page cache. Capacity = max # of xlate_page in the cache. */
struct xlate_pcache {
    size_t capacity;
    size_t size;
    struct avl_root root;
    struct list_head lru_list;
};

/* Per-namespace address mapping domain. */
struct am_domain {
    struct kref kref;
    unsigned int nsid;
    size_t total_logical_pages;
    enum plane_allocate_scheme pa_scheme;

    /* Global translation directory */
    mppn_t* gtd;
    size_t gtd_size;
    size_t xlate_ents_per_page;
    size_t total_xlate_pages;

    /* Translation page cache */
    struct xlate_pcache pcache;
};

static struct am_domain* active_domains[NAMESPACE_MAX];

static void assign_plane(struct am_domain* domain,
                         struct flash_transaction* txn);
static int alloc_page_for_mapping(struct am_domain* domain,
                                  struct flash_transaction* txn, mvpn_t mvpn,
                                  int for_gc);
static void domain_free(struct kref* kref);

static inline struct am_domain* domain_get_by_nsid(unsigned int nsid)
{
    struct am_domain* domain;

    Xil_AssertNonvoid(nsid > 0 && nsid <= NAMESPACE_MAX);
    domain = active_domains[NSID2IDX(nsid)];
    if (domain) kref_get(&domain->kref);
    return domain;
}

/* Return the AM domain for a transaction based on its NSID. */
static inline struct am_domain* domain_get(struct flash_transaction* txn)
{
    return domain_get_by_nsid(txn->nsid);
}

static inline void domain_put(struct am_domain* domain)
{
    kref_put(&domain->kref, domain_free);
}

/* Get the mapping virtual page number for an LPA. */
static inline mvpn_t get_mvpn(struct am_domain* domain, lpa_t lpa)
{
    return lpa / domain->xlate_ents_per_page;
}

/* Get the slot within a mapping virtual page for an LPA. */
static inline unsigned int get_mvpn_slot(struct am_domain* domain, lpa_t lpa)
{
    return lpa % domain->xlate_ents_per_page;
}

static inline void ppa_to_address(ppa_t ppa, struct flash_address* addr)
{
#define XLATE_PPA(ppa, name, cname)           \
    do {                                      \
        addr->name = ppa / PAGES_PER_##cname; \
        ppa = ppa % PAGES_PER_##cname;        \
    } while (0)
    XLATE_PPA(ppa, channel, CHANNEL);
    XLATE_PPA(ppa, chip, CHIP);
    XLATE_PPA(ppa, die, DIE);
    XLATE_PPA(ppa, plane, PLANE);
    XLATE_PPA(ppa, block, BLOCK);
    addr->page = ppa;
#undef XLATE_PPA
}

ppa_t address_to_ppa(struct flash_address* addr)
{
    return PAGES_PER_CHIP *
               (CHIPS_ENABLED_PER_CHANNEL * addr->channel + addr->chip) +
           PAGES_PER_DIE * addr->die + PAGES_PER_PLANE * addr->plane +
           PAGES_PER_BLOCK * addr->block + addr->page;
}

int amu_submit_transaction(struct flash_transaction* txn)
{
    int r;

    txn->stats.amu_submit_time = timer_get_cycles();

    if (txn->type == TXN_READ && txn->data) {
        txn->code_buf = txn->data + FLASH_PG_SIZE;
        txn->code_length = FLASH_PG_OOB_SIZE;
    }

    r = submit_flash_transaction(txn);
    if (r) return r;

    if (txn->req) {
        switch (txn->type) {
        case TXN_READ:
            txn->req->stats.total_flash_read_txns++;
            txn->req->stats.total_flash_read_bytes +=
                txn->length + txn->code_length;
            txn->req->stats.flash_read_transfer_us += txn->total_xfer_us;
            txn->req->stats.flash_read_command_us += txn->total_exec_us;
            break;
        case TXN_WRITE:
            txn->req->stats.total_flash_write_txns++;
            txn->req->stats.total_flash_write_bytes += txn->length;
            txn->req->stats.flash_write_transfer_us += txn->total_xfer_us;
            txn->req->stats.flash_write_command_us += txn->total_exec_us;
            break;
        default:
            break;
        }
    }

    if (txn->type == TXN_READ && txn->err_bitmap) {
        if (txn->req) {
            txn->req->stats.ecc_error_blocks +=
                __builtin_popcountl(txn->err_bitmap);
        }

        /* Correction. */
        r = ecc_correct(txn->data, FLASH_PG_SIZE, txn->data + FLASH_PG_SIZE,
                        FLASH_PG_OOB_SIZE, txn->err_bitmap);

        WARN(
            r == -EBADMSG,
            "ECC uncorrectable error t%d s%d ch%d w%d d%d pl%d b%d p%d data %p offset %u length %u\n",
            txn->type, txn->source, txn->addr.channel, txn->addr.chip,
            txn->addr.die, txn->addr.plane, txn->addr.block, txn->addr.page,
            txn->data, txn->offset, txn->length);

        if (r < 0) return -r;
    }

    return 0;
}

static int xpc_key_node_comp(void* key, struct avl_node* node)
{
    struct xlate_page* r1 = (struct xlate_page*)key;
    struct xlate_page* r2 = avl_entry(node, struct xlate_page, avl);

    if (r1->mvpn < r2->mvpn)
        return -1;
    else if (r1->mvpn > r2->mvpn)
        return 1;
    return 0;
}

static int xpc_node_node_comp(struct avl_node* node1, struct avl_node* node2)
{
    struct xlate_page* r1 = avl_entry(node1, struct xlate_page, avl);
    struct xlate_page* r2 = avl_entry(node2, struct xlate_page, avl);

    if (r1->mvpn < r2->mvpn)
        return -1;
    else if (r1->mvpn > r2->mvpn)
        return 1;
    return 0;
}

static inline void xpc_avl_start_iter(struct xlate_pcache* xpc,
                                      struct avl_iter* iter, void* key,
                                      int flags)
{
    avl_start_iter(&xpc->root, iter, key, flags);
}

static inline struct xlate_page* xpc_avl_get_iter(struct avl_iter* iter)
{
    struct avl_node* node = avl_get_iter(iter);
    if (!node) return NULL;
    return avl_entry(node, struct xlate_page, avl);
}

static struct xlate_page* xpc_find(struct xlate_pcache* xpc, mvpn_t mvpn)
{
    struct avl_node* node = xpc->root.node;
    struct xlate_page* xpg = NULL;

    while (node) {
        xpg = avl_entry(node, struct xlate_page, avl);

        if (xpg->mvpn == mvpn) {
            return xpg;
        } else if (mvpn < xpg->mvpn)
            node = node->left;
        else if (mvpn > xpg->mvpn)
            node = node->right;
    }

    return NULL;
}

static void xpc_init(struct xlate_pcache* xpc, size_t capacity)
{
    xpc->capacity = capacity;
    xpc->size = 0;
    INIT_LIST_HEAD(&xpc->lru_list);
    INIT_AVL_ROOT(&xpc->root, xpc_key_node_comp, xpc_node_node_comp);
}

static void xpc_free(struct xlate_pcache* xpc)
{
    struct avl_node* node;
    struct xlate_page* xpg = NULL;

    /* Takes longer time (O(nlogn)) but less stack space than recursion. */
    while ((node = xpc->root.node) != NULL) {
        xpg = avl_entry(node, struct xlate_page, avl);

        /* No one should be using this page anymore. */
        Xil_AssertVoid(xpg->pin_count == 0);

        avl_erase(node, &xpc->root);
        list_del(&xpg->lru);

        free_mem(__pa(xpg->entries), XLATE_PG_SIZE);
        SLABFREE(xpg);
    }
}

static int xpc_add(struct xlate_pcache* xpc, mvpn_t mvpn,
                   struct xlate_page** xpgpp)
{
    struct xlate_page* xpg;
    mutexattr_t mutex_attr = {
        .tag = LKT_AMU,
    };

    if (xpc->size >= xpc->capacity) return ENOSPC;

    SLABALLOC(xpg);
    if (!xpc) return ENOMEM;

    memset(xpg, 0, sizeof(*xpg));
    xpg->mvpn = mvpn;
    xpg->dirty = FALSE;
    xpg->pin_count = 1;
    INIT_LIST_HEAD(&xpg->lru);
    mutex_init(&xpg->mutex, &mutex_attr);

    /* Allocate buffer for the translation page. Prefer PS DDR. */
    xpg->entries = alloc_vmpages(XLATE_PG_SIZE >> ARCH_PG_SHIFT, ZONE_PS_DDR);
    if (!xpg->entries) {
        SLABFREE(xpg);
        return ENOMEM;
    }

    avl_insert(&xpg->avl, &xpc->root);
    xpc->size++;
    *xpgpp = xpg;

    return 0;
}

static inline void xpc_add_lru(struct xlate_pcache* xpc, struct xlate_page* xpg)
{
    list_add(&xpg->lru, &xpc->lru_list);
}

static inline void xpc_remove_lru(struct xlate_page* xpg)
{
    list_del(&xpg->lru);
}

static inline void xpc_pin(struct xlate_page* xpg)
{
    if (xpg->pin_count++ == 0) xpc_remove_lru(xpg);
}

static inline void xpc_unpin(struct xlate_pcache* xpc, struct xlate_page* xpg)
{
    Xil_AssertVoid(xpg->pin_count > 0);
    if (--xpg->pin_count == 0) xpc_add_lru(xpc, xpg);
}

static inline void xpc_update_mapping(struct xlate_page* xpg, unsigned int slot,
                                      ppa_t ppa, page_bitmap_t bitmap)
{
    xpg->entries[slot].ppa = ppa;
    xpg->entries[slot].bitmap |= bitmap;
    xpg->dirty = TRUE;
}

static int xpc_read_page(struct am_domain* domain, struct xlate_page* xpg)
{
    mvpn_t mvpn = xpg->mvpn;
    mppn_t mppn = domain->gtd[mvpn];
    struct flash_transaction txn;
    struct xlate_entry_disk* buf;
    int r;

    Xil_AssertNonvoid(!xpg->dirty);

    buf = alloc_vmpages(FLASH_PG_BUFFER_SIZE >> ARCH_PG_SHIFT, ZONE_PS_DDR_LOW);
    if (!buf) return ENOMEM;

    flash_transaction_init(&txn);
    txn.type = TXN_READ;
    txn.source = TS_MAPPING;
    txn.nsid = domain->nsid;
    txn.lpa = mvpn;
    txn.ppa = mppn;
    txn.data = (u8*)buf;
    txn.offset = 0;
    txn.length = FLASH_PG_SIZE;
    txn.bitmap = (1UL << (FLASH_PG_SIZE >> SECTOR_SHIFT)) - 1;
    ppa_to_address(txn.ppa, &txn.addr);

    dma_sync_single_for_device(buf, FLASH_PG_BUFFER_SIZE, DMA_FROM_DEVICE);
    r = amu_submit_transaction(&txn);
    dma_sync_single_for_cpu(buf, FLASH_PG_BUFFER_SIZE, DMA_FROM_DEVICE);

    WARN(r != 0, "Read translation page failed\n");

    if (r == 0) {
        int i;
        for (i = 0; i < domain->xlate_ents_per_page; i++) {
            xpg->entries[i].ppa = buf[i].ppa;
            xpg->entries[i].bitmap = (1UL << SECTORS_PER_FLASH_PG) - 1;
        }
    }

    free_mem(__pa(buf), FLASH_PG_BUFFER_SIZE);
    return r;
}

static int xpc_flush_page(struct am_domain* domain, struct xlate_page* xpg)
{
    mvpn_t mvpn = xpg->mvpn;
    struct flash_transaction txn;
    struct xlate_entry_disk* buf;
    int i, r;

    if (!xpg->dirty) return 0;

    buf = alloc_vmpages(FLASH_PG_BUFFER_SIZE >> ARCH_PG_SHIFT, ZONE_PS_DDR_LOW);
    if (!buf) return ENOMEM;

    for (i = 0; i < domain->xlate_ents_per_page; i++) {
        buf[i].ppa = xpg->entries[i].ppa;
    }

    flash_transaction_init(&txn);
    txn.type = TXN_WRITE;
    txn.source = TS_MAPPING;
    txn.nsid = domain->nsid;
    txn.lpa = mvpn;
    txn.ppa = NO_PPA;
    txn.data = (u8*)buf;
    txn.offset = 0;
    txn.length = FLASH_PG_SIZE;
    txn.bitmap = (1UL << (FLASH_PG_SIZE >> SECTOR_SHIFT)) - 1;

    assign_plane(domain, &txn);
    alloc_page_for_mapping(domain, &txn, mvpn, FALSE);

    dma_sync_single_for_device(buf, FLASH_PG_BUFFER_SIZE, DMA_TO_DEVICE);
    r = amu_submit_transaction(&txn);
    dma_sync_single_for_cpu(buf, FLASH_PG_BUFFER_SIZE, DMA_TO_DEVICE);
    if (r) goto out;

    xpg->dirty = FALSE;
    domain->gtd[mvpn] = txn.ppa;

    r = 0;

out:
    if (r) {
        bm_invalidate_page(&txn.addr);
    }

    free_mem(__pa(buf), FLASH_PG_BUFFER_SIZE);
    return r;
}

static int xpc_evict(struct xlate_pcache* xpc, struct xlate_page** xpgpp)
{
    struct xlate_page* xpg;

    if (list_empty(&xpc->lru_list)) return ENOMEM;

    xpg = list_entry(xpc->lru_list.prev, struct xlate_page, lru);

    /* Pinned page should NEVER be evicted. */
    Xil_AssertNonvoid(xpg->pin_count == 0);

    list_del(&xpg->lru);
    avl_erase(&xpg->avl, &xpc->root);
    xpc->size--;

    *xpgpp = xpg;
    return 0;
}

/* Get the translation page referenced by mvpn from the page cache. Return
 * the page exclusively locked and detached from LRU. */
static int get_translation_page(struct am_domain* domain, mvpn_t mvpn,
                                struct xlate_page** xpgpp)
{
    struct xlate_page* xpg;
    struct xlate_pcache* xpc = &domain->pcache;
    int r;

    xpg = xpc_find(xpc, mvpn);

    if (xpg) {
        /* Cache hit. */
        xpc_pin(xpg);
        mutex_lock(&xpg->mutex);
    } else {
        /* Try to add a new translation page. */
        r = xpc_add(xpc, mvpn, &xpg);

        if (unlikely(r == ENOSPC)) {
            /* Cache is full. */
            r = xpc_evict(xpc, &xpg);
            if (r) return r;

            if (xpg->dirty) {
                xpc_flush_page(domain, xpg);
            }

            xpg->mvpn = mvpn;
            avl_insert(&xpg->avl, &xpc->root);
            xpc->size++;

            xpc_pin(xpg);
            mutex_lock(&xpg->mutex);
        } else if (unlikely(r != 0)) {
            /* Error. */
            return r;
        } else {
            /* Ok. */
            Xil_AssertNonvoid(xpg->pin_count == 1);
            mutex_lock(&xpg->mutex);

            /* If there is no physical page for this translation page then
             * populate it with invalid PPAs. Otherwise issue a flash
             * transaction to read it from NAND flash. */
            if (domain->gtd[mvpn] == NO_MPPN) {
                int i;
                for (i = 0; i < domain->xlate_ents_per_page; i++) {
                    xpg->entries[i].ppa = NO_PPA;
                    xpg->entries[i].bitmap = 0;
                }
                xpg->dirty = TRUE;
            } else {
                r = xpc_read_page(domain, xpg);

                if (r) {
                    mutex_unlock(&xpg->mutex);
                    avl_erase(&xpg->avl, &xpc->root);
                    xpc_unpin(&domain->pcache, xpg);
                }
            }
        }
    }

    *xpgpp = xpg;
    return 0;
}

/* Get the PPA associated with an LPA. Return NO_PPA if not mapped. */
static int get_ppa(struct am_domain* domain, lpa_t lpa, ppa_t* ppap,
                   page_bitmap_t* bitmapp)
{
    mvpn_t mvpn = get_mvpn(domain, lpa);
    unsigned int slot = get_mvpn_slot(domain, lpa);
    struct xlate_page* xpg;
    int r;

    r = get_translation_page(domain, mvpn, &xpg);
    if (r) return r;

    *ppap = xpg->entries[slot].ppa;
    if (bitmapp) *bitmapp = xpg->entries[slot].bitmap;

    mutex_unlock(&xpg->mutex);
    xpc_unpin(&domain->pcache, xpg);

    return 0;
}

static void assign_plane(struct am_domain* domain,
                         struct flash_transaction* txn)
{
    struct flash_address* addr = &txn->addr;
    lpa_t lpa = txn->lpa;

    unsigned int channel_count;
    static const unsigned int chip_count = CHIPS_ENABLED_PER_CHANNEL;
    static const unsigned int die_count = DIES_PER_CHIP;
    static const unsigned int plane_count = PLANES_PER_DIE;

    static unsigned int enable_channels[NR_CHANNELS];
    static unsigned int num_enable_channels = 0;

    Xil_AssertVoid(domain);

    if (!num_enable_channels) {
        int i;

        for (i = 0; i < NR_CHANNELS; i++) {
            if (CHANNEL_ENABLE_MASK & (1 << i))
                enable_channels[num_enable_channels++] = i;
        }

        Xil_AssertVoid(num_enable_channels > 0);
        Xil_AssertVoid(num_enable_channels <= NR_CHANNELS);
    }

    channel_count = num_enable_channels;

#define ASSIGN_PHYS_ADDR(lpa, name)        \
    do {                                   \
        addr->name = (lpa) % name##_count; \
        (lpa) = (lpa) / name##_count;      \
    } while (0)

#define ASSIGN_PLANE(lpa, first, second, third, fourth) \
    do {                                                \
        ASSIGN_PHYS_ADDR(lpa, first);                   \
        ASSIGN_PHYS_ADDR(lpa, second);                  \
        ASSIGN_PHYS_ADDR(lpa, third);                   \
        ASSIGN_PHYS_ADDR(lpa, fourth);                  \
    } while (0)

    switch (domain->pa_scheme) {
    case PAS_CWDP:
        ASSIGN_PLANE(lpa, channel, chip, die, plane);
        break;
    case PAS_CWPD:
        ASSIGN_PLANE(lpa, channel, chip, plane, die);
        break;
    case PAS_CDWP:
        ASSIGN_PLANE(lpa, channel, die, chip, plane);
        break;
    case PAS_CDPW:
        ASSIGN_PLANE(lpa, channel, die, plane, chip);
        break;
    case PAS_CPWD:
        ASSIGN_PLANE(lpa, channel, plane, chip, die);
        break;
    case PAS_CPDW:
        ASSIGN_PLANE(lpa, channel, plane, die, chip);
        break;
    case PAS_WCDP:
        ASSIGN_PLANE(lpa, chip, channel, die, plane);
        break;
    case PAS_WCPD:
        ASSIGN_PLANE(lpa, chip, channel, plane, die);
        break;
    case PAS_WDCP:
        ASSIGN_PLANE(lpa, chip, die, channel, plane);
        break;
    case PAS_WDPC:
        ASSIGN_PLANE(lpa, chip, die, plane, channel);
        break;
    case PAS_WPCD:
        ASSIGN_PLANE(lpa, chip, plane, channel, die);
        break;
    case PAS_WPDC:
        ASSIGN_PLANE(lpa, chip, plane, die, channel);
        break;
    case PAS_DCWP:
        ASSIGN_PLANE(lpa, die, channel, chip, plane);
        break;
    case PAS_DCPW:
        ASSIGN_PLANE(lpa, die, channel, plane, chip);
        break;
    case PAS_DWCP:
        ASSIGN_PLANE(lpa, die, chip, channel, plane);
        break;
    case PAS_DWPC:
        ASSIGN_PLANE(lpa, die, chip, plane, channel);
        break;
    case PAS_DPCW:
        ASSIGN_PLANE(lpa, die, plane, channel, chip);
        break;
    case PAS_DPWC:
        ASSIGN_PLANE(lpa, die, plane, chip, channel);
        break;
    case PAS_PCWD:
        ASSIGN_PLANE(lpa, plane, channel, chip, die);
        break;
    case PAS_PCDW:
        ASSIGN_PLANE(lpa, plane, channel, die, chip);
        break;
    case PAS_PWCD:
        ASSIGN_PLANE(lpa, plane, chip, channel, die);
        break;
    case PAS_PWDC:
        ASSIGN_PLANE(lpa, plane, chip, die, channel);
        break;
    case PAS_PDCW:
        ASSIGN_PLANE(lpa, plane, die, channel, chip);
        break;
    case PAS_PDWC:
        ASSIGN_PLANE(lpa, plane, die, chip, channel);
        break;
    default:
        panic("Invalid plane allocation scheme");
        break;
    }
#undef ASSIGN_PLANE
#undef ASSIGN_PHYS_ADDR

    Xil_AssertVoid(addr->channel < num_enable_channels);
    addr->channel = enable_channels[addr->channel];

    Xil_AssertVoid(addr->channel < NR_CHANNELS);
    Xil_AssertVoid(addr->chip < CHIPS_PER_CHANNEL);
    Xil_AssertVoid(addr->die < DIES_PER_CHIP);
    Xil_AssertVoid(addr->plane < PLANES_PER_DIE);
}

static int update_read(struct flash_transaction* txn, struct xlate_entry* entry)
{
    struct flash_transaction read_txn;
    void* buf;
    page_bitmap_t bitmap = entry->bitmap & ~txn->bitmap;
    int r;

    Xil_AssertNonvoid(entry->ppa != NO_PPA);
    Xil_AssertNonvoid(bitmap);

    buf = alloc_vmpages(FLASH_PG_BUFFER_SIZE >> ARCH_PG_SHIFT, ZONE_PS_DDR_LOW);
    if (!buf) return ENOMEM;

    flash_transaction_init(&read_txn);
    read_txn.type = TXN_READ;
    read_txn.req = txn->req;
    read_txn.source = TS_USER_IO;
    read_txn.nsid = txn->nsid;
    read_txn.lpa = txn->lpa;
    read_txn.ppa = entry->ppa;
    read_txn.data = (u8*)buf;
    read_txn.offset = (ffsl(bitmap) - 1) << SECTOR_SHIFT;
    Xil_AssertNonvoid(read_txn.offset < FLASH_PG_SIZE);
    read_txn.length = FLASH_PG_SIZE - read_txn.offset;
    read_txn.bitmap = bitmap;
    ppa_to_address(read_txn.ppa, &read_txn.addr);

    dma_sync_single_for_device(buf, FLASH_PG_BUFFER_SIZE, DMA_FROM_DEVICE);
    r = amu_submit_transaction(&read_txn);
    dma_sync_single_for_cpu(buf, FLASH_PG_BUFFER_SIZE, DMA_FROM_DEVICE);

    WARN(r != 0, "Update read failed\n");

    if (r == 0) {
        struct iovec copy_iov[SECTORS_PER_FLASH_PG];
        struct iov_iter copy_iter;
        ssize_t copied_bytes;
        int i;

        for (i = 0; i < SECTORS_PER_FLASH_PG; i++) {
            copy_iov[i].iov_base = NULL;
            copy_iov[i].iov_len = SECTOR_SIZE;

            if (bitmap & (1UL << i)) {
                copy_iov[i].iov_base = txn->data + (i << SECTOR_SHIFT);
            }
        }

        iov_iter_init(&copy_iter, copy_iov, SECTORS_PER_FLASH_PG,
                      FLASH_PG_SIZE);
        copied_bytes = zdma_iter_copy_to(&copy_iter, buf, FLASH_PG_SIZE, FALSE);

        if (copied_bytes < 0) {
            r = -copied_bytes;
        }

        if (r == 0) {
            /* Now, the original transaction needs to include the sectors
             * just read from the disk. */
            txn->bitmap |= bitmap;
            txn->offset = min(txn->offset, read_txn.offset);
            txn->length = FLASH_PG_SIZE - txn->offset;
        }
    }

    free_mem(__pa(buf), FLASH_PG_BUFFER_SIZE);
    return r;
}

static int alloc_page_for_write(struct am_domain* domain,
                                struct flash_transaction* txn, int for_gc)
{
    mvpn_t mvpn = get_mvpn(domain, txn->lpa);
    unsigned int slot = get_mvpn_slot(domain, txn->lpa);
    struct xlate_page* xpg;
    ppa_t ppa;
    int r;

    r = get_translation_page(domain, mvpn, &xpg);
    if (r) return r;

    ppa = xpg->entries[slot].ppa;

    if (ppa != NO_PPA) {
        /* Existing PP is found. We may need to read the old PP
         * (read-modify-write). */
        struct flash_address addr;
        page_bitmap_t bitmap = xpg->entries[slot].bitmap & txn->bitmap;

        if (bitmap != xpg->entries[slot].bitmap) {
            /* Update read required. */
            r = update_read(txn, &xpg->entries[slot]);
            if (r) goto out_unlock;
        }

        /* Invalidate the old PP. */
        ppa_to_address(ppa, &addr);
        bm_invalidate_page(&addr);
    }

    /* Allocate a new PP and update the mapping. */
    bm_alloc_page(txn->nsid, &txn->addr, for_gc, FALSE /* for_mapping */);
    txn->ppa = address_to_ppa(&txn->addr);
    xpc_update_mapping(xpg, slot, txn->ppa, txn->bitmap);

out_unlock:
    mutex_unlock(&xpg->mutex);
    xpc_unpin(&domain->pcache, xpg);

    return r;
}

static int alloc_page_for_mapping(struct am_domain* domain,
                                  struct flash_transaction* txn, mvpn_t mvpn,
                                  int for_gc)
{
    mppn_t mppn = domain->gtd[mvpn];

    if (mppn != NO_MPPN) {
        struct flash_address addr;
        ppa_to_address((ppa_t)mppn, &addr);
        bm_invalidate_page(&addr);
    }

    bm_alloc_page(txn->nsid, &txn->addr, for_gc, TRUE /* for_mapping */);
    txn->ppa = address_to_ppa(&txn->addr);

    return 0;
}

/* Translate an LPA into PPA. Allocate a new physical page if not mapped. */
static int translate_lpa(struct flash_transaction* txn)
{
    struct am_domain* domain = domain_get(txn);
    int r = 0;

    if (!domain) return EINVAL;

    if (txn->type == TXN_READ) {
        ppa_t ppa;

        r = get_ppa(domain, txn->lpa, &ppa, NULL);
        if (r != 0) goto out;

        if (ppa == NO_PPA) {
            /* Read an LP that has not been written. Just allocate a PP for
             * it without initialization. */
            assign_plane(domain, txn);
            r = alloc_page_for_write(domain, txn, FALSE);
            if (r != 0) goto out;
        } else {
            txn->ppa = ppa;
            ppa_to_address(txn->ppa, &txn->addr);
        }
    } else {
        assign_plane(domain, txn);
        r = alloc_page_for_write(domain, txn, FALSE);
        if (r != 0) goto out;
    }

    txn->ppa_ready = TRUE;

out:
    domain_put(domain);
    return r;
}

int amu_dispatch(struct list_head* txn_list)
{
    struct flash_transaction* txn;
    timestamp_t now = timer_get_cycles();
    int r = 0;

    if (list_empty(txn_list)) return 0;

    list_for_each_entry(txn, txn_list, list)
    {
        txn->stats.amu_begin_service_time = now;
        r = translate_lpa(txn);
        if (r) goto cleanup;
    }

    /* All transactions are assigned PPAs -- it's time to execute them. */
    list_for_each_entry(txn, txn_list, list)
    {
        if (txn->ppa_ready) {
            r = amu_submit_transaction(txn);

            if (r != 0) break;
        }
    }

cleanup:
    if (r) {
        /* TODO: Revert any mapping table change. */
        /* We do mapping table update in =translate_lpa= and catch ECC errors in
         * =amu_submit_transaction=, which is bad because it means we lose both
         * the old and the new page in case of a write failure. A better way
         * is to finalize the mapping table update only after all transactions
         * are successfully executed. */

        list_for_each_entry(txn, txn_list, list)
        {
            if (txn->type == TXN_WRITE && txn->ppa_ready) {
                struct flash_address addr;
                ppa_to_address(txn->ppa, &addr);
                bm_invalidate_page(&addr);
            }
        }
    }

    return r;
}

static int save_gtd(struct am_domain* domain, const char* filename)
{
    FIL fil;
    size_t write_size;
    UINT bw;
    int rc;

    write_size = domain->total_xlate_pages * sizeof(mppn_t);

    rc = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (rc) return EIO;

    rc = f_write(&fil, domain->gtd, write_size, &bw);
    if (rc || bw != write_size) return EIO;

    rc = f_close(&fil);
    return rc > 0 ? EIO : 0;
}

static int restore_gtd(struct am_domain* domain, const char* filename)
{
    FIL fil;
    size_t read_size;
    UINT br;
    int rc;

    read_size = domain->total_xlate_pages * sizeof(mppn_t);

    rc = f_open(&fil, filename, FA_READ);
    if (rc) return EIO;

    rc = f_read(&fil, domain->gtd, read_size, &br);
    if (rc || br != read_size) return EIO;

    rc = f_close(&fil);
    return rc > 0 ? EIO : 0;
}

static int domain_init(struct am_domain* domain, unsigned int nsid,
                       size_t capacity, size_t total_logical_pages,
                       enum plane_allocate_scheme pa_scheme, int reset)
{
    char gtd_filename[20];
    size_t xlate_ents_per_page =
        FLASH_PG_SIZE / sizeof(struct xlate_entry_disk);
    size_t total_xlate_pages =
        (total_logical_pages + xlate_ents_per_page - 1) / xlate_ents_per_page;
    size_t gtd_size;
    FILINFO fno;
    int i, r = 0;

    memset(domain, 0, sizeof(*domain));
    kref_init(&domain->kref);
    domain->nsid = nsid;
    domain->total_logical_pages = total_logical_pages;
    domain->xlate_ents_per_page = xlate_ents_per_page;
    domain->total_xlate_pages = total_xlate_pages;
    domain->pa_scheme = pa_scheme;

    xpc_init(&domain->pcache, capacity);

    gtd_size = total_xlate_pages * sizeof(mppn_t);
    gtd_size = roundup(gtd_size, ARCH_PG_SIZE);

    domain->gtd = alloc_vmpages(gtd_size >> ARCH_PG_SHIFT, ZONE_PS_DDR);
    if (!domain->gtd) goto fail_free_xpc;
    domain->gtd_size = gtd_size;

    snprintf(gtd_filename, sizeof(gtd_filename), GTD_FILENAME, domain->nsid);

    if (f_stat(gtd_filename, &fno) != 0 || reset) {
        xil_printf(
            NAME
            " Initializing new global translation directory for namespace %d ...",
            nsid);

        for (i = 0; i < total_xlate_pages; i++) {
            domain->gtd[i] = NO_MPPN;
        }

        r = save_gtd(domain, gtd_filename);
        if (r) {
            r = EIO;
        } else {
            xil_printf("OK\n");
        }
    } else {
        xil_printf(
            NAME
            " Restoring global translation directory for namespace %d (%lu bytes) ...",
            nsid, fno.fsize);
        r = restore_gtd(domain, gtd_filename);
        if (r) {
            r = EIO;
        } else {
            xil_printf("OK\n");
        }
    }

    if (r != 0) {
        xil_printf("FAILED (%d)\n", r);
        goto fail_free_gtd;
    }

    xil_printf(NAME " Initialized namespace %d with %lu logical pages\n", nsid,
               total_logical_pages);

    return 0;

fail_free_gtd:
    free_mem(__pa(domain->gtd), domain->gtd_size);
fail_free_xpc:
    xpc_free(&domain->pcache);

    return r;
}

int amu_attach_domain(unsigned int nsid, size_t capacity_bytes,
                      size_t total_logical_pages, int reset)
{
    struct am_domain* domain;
    int r;

    /* Bad NSID. */
    if (nsid <= 0 || nsid > NAMESPACE_MAX) return EINVAL;

    /* Domain already attached. */
    if (active_domains[NSID2IDX(nsid)] != NULL) return EINVAL;

    SLABALLOC(domain);
    if (!domain) return ENOMEM;

    r = domain_init(domain, nsid, capacity_bytes / XLATE_PG_SIZE,
                    total_logical_pages, DEFAULT_PLANE_ALLOCATE_SCHEME, reset);
    if (r) goto out_free;

    active_domains[NSID2IDX(nsid)] = domain;
    return 0;

out_free:
    SLABFREE(domain);
    return r;
}

static void flush_domain(struct am_domain* domain)
{
    struct xlate_page start_key, *xpg;
    struct avl_iter iter;

    start_key.mvpn = 0;
    xpc_avl_start_iter(&domain->pcache, &iter, &start_key, AVL_GREATER_EQUAL);
    for (xpg = xpc_avl_get_iter(&iter); xpg;) {
        if (xpg->dirty) {
            xpc_pin(xpg);
            mutex_lock(&xpg->mutex);

            xpc_flush_page(domain, xpg);

            mutex_unlock(&xpg->mutex);
            xpc_unpin(&domain->pcache, xpg);
        }

        avl_inc_iter(&iter);
        xpg = xpc_avl_get_iter(&iter);
    }
}

static int save_domain(struct am_domain* domain)
{
    char gtd_filename[20];
    snprintf(gtd_filename, sizeof(gtd_filename), GTD_FILENAME, domain->nsid);

    flush_domain(domain);
    return save_gtd(domain, gtd_filename);
}

int amu_save_domain(unsigned int nsid)
{
    struct am_domain* domain = domain_get_by_nsid(nsid);
    int r;

    if (!domain) return EINVAL;

    r = save_domain(domain);
    domain_put(domain);
    return r;
}

static void domain_free(struct kref* kref)
{
    struct am_domain* domain = list_entry(kref, struct am_domain, kref);

    xpc_free(&domain->pcache);

    if (domain->gtd_size > 0) {
        free_mem(__pa(domain->gtd), domain->gtd_size);
    }

    SLABFREE(domain);
}

int amu_detach_domain(unsigned int nsid)
{
    struct am_domain* domain;

    /* Bad NSID. */
    if (nsid <= 0 || nsid > NAMESPACE_MAX) return EINVAL;

    /* Domain not attached. */
    domain = active_domains[NSID2IDX(nsid)];
    if (!domain) return EINVAL;

    active_domains[NSID2IDX(nsid)] = NULL;

    save_domain(domain);
    domain_put(domain);
    return 0;
}

int amu_delete_domain(unsigned int nsid)
{
    char gtd_filename[20];
    FILINFO fno;
    int r;

    if (nsid <= 0 || nsid > NAMESPACE_MAX) return EINVAL;

    snprintf(gtd_filename, sizeof(gtd_filename), GTD_FILENAME, nsid);

    r = f_stat(gtd_filename, &fno);

    if (r != FR_OK) {
        if (r == FR_NO_FILE) return 0;
        return EIO;
    }

    f_unlink(gtd_filename);

    return 0;
}
