#include <errno.h>

#include <types.h>
#include <spinlock.h>
#include <storpu/vm.h>
#include <storpu/file.h>
#include <storpu/thread.h>
#include <storpu/mutex.h>
#include <slab.h>
#include <utils.h>

#include "region.h"
#include "cache.h"

struct address_space {
    spinlock_t tree_lock;
    struct avl_root pages;
    unsigned long nrpages;
};

static struct address_space file_caches[FILE_MAX];
static struct address_space host_mem_cache;

static struct address_space* cache_get_by_fd(int fd)
{
    if (fd == FD_HOST_MEM) return &host_mem_cache;

    if (fd < 0 || fd >= FILE_MAX) return NULL;
    return &file_caches[fd];
}

static int cache_key_node_comp(void* key, struct avl_node* node)
{
    struct cached_page* r1 = (struct cached_page*)key;
    struct cached_page* r2 = avl_entry(node, struct cached_page, avl);

    unsigned long off1 = r1->offset;
    unsigned long off2 = r2->offset;

    if (off1 < off2)
        return -1;
    else if (off1 > off2)
        return 1;
    return 0;
}

static int cache_node_node_comp(struct avl_node* node1, struct avl_node* node2)
{
    struct cached_page* r1 = avl_entry(node1, struct cached_page, avl);
    struct cached_page* r2 = avl_entry(node2, struct cached_page, avl);
    unsigned long off1 = r1->offset;
    unsigned long off2 = r2->offset;

    if (off1 < off2)
        return -1;
    else if (off1 > off2)
        return 1;
    return 0;
}

struct cached_page* cache_lookup(struct address_space* cache,
                                 unsigned long offset)
{
    struct avl_node* node = cache->pages.node;

    while (node) {
        struct cached_page* cp = avl_entry(node, struct cached_page, avl);

        if (offset == cp->offset) {
            return cp;
        } else if (offset < cp->offset) {
            node = node->left;
        } else if (offset > cp->offset) {
            node = node->right;
        }
    }

    return NULL;
}

static void cache_init_one(struct address_space* cache)
{
    INIT_AVL_ROOT(&cache->pages, cache_key_node_comp, cache_node_node_comp);
    spinlock_init(&cache->tree_lock);
    cache->nrpages = 0;
}

void page_cache_init(void)
{
    int i;

    cache_init_one(&host_mem_cache);

    for (i = 0; i < FILE_MAX; i++)
        cache_init_one(&file_caches[i]);
}

struct cached_page* find_cached_page(int fd, unsigned long offset, int flags)
{
    struct address_space* cache = cache_get_by_fd(fd);
    struct cached_page* cp;

    if (!cache) return NULL;

    spin_lock(&cache->tree_lock);
    cp = cache_lookup(cache, offset);
    spin_unlock(&cache->tree_lock);

    if (!cp) return NULL;

    if (flags & FCP_LOCK) {
        lock_cached_page(cp);

        if (cp->offset != offset) {
            unlock_cached_page(cp);
            return NULL;
        }
    }

    return cp;
}

int page_cache_add(int fd, unsigned long offset, phys_addr_t phys, int hugepage,
                   struct cached_page** cpp)
{
    struct address_space* cache = cache_get_by_fd(fd);
    struct cached_page *cp, *cp_exist;
    struct page* pages[HP_NR_PAGES];
    int nr_pages = 0;
    int i, r;

    if (!cache) return EINVAL;

    r = ENOMEM;
    for (i = 0; i < (hugepage ? HP_NR_PAGES : 1); i++) {
        pages[i] = page_new(phys + i * ARCH_PG_SIZE);
        if (!pages[i]) goto free_pages;

        pages[i]->flags |= PFF_INCACHE;
        nr_pages++;
    }

    SLABALLOC(cp);
    if (!cp) goto free_pages;

    memset(cp, 0, sizeof(*cp));
    cp->fd = fd;
    cp->offset = offset;
    if (hugepage) cp->flags |= CPF_HUGEPAGE;
    mutex_init(&cp->lock, NULL);

    for (i = 0; i < nr_pages; i++) {
        cp->pages[i] = pages[i];
        pages[i]->private = cp;
        pages[i]->refcount++;
    }

    mutex_lock(&cp->lock);

    spin_lock(&cache->tree_lock);

    cp_exist = cache_lookup(cache, offset);
    if (cp_exist) {
        r = EEXIST;
        spin_unlock(&cache->tree_lock);
        goto free_cp;
    }

    avl_insert(&cp->avl, &cache->pages);
    cache->nrpages++;

    spin_unlock(&cache->tree_lock);

    if (cpp) *cpp = cp;

    return 0;

free_cp:
    SLABFREE(cp);
free_pages:
    for (i = 0; i < nr_pages; i++) {
        pages[i]->phys_addr = PHYS_NONE;
        page_free(pages[i]);
    }

    return r;
}

static unsigned int pagevec_lookup_range(struct address_space* cache,
                                         unsigned long* offset,
                                         unsigned long end, unsigned int tag,
                                         struct cached_page** pages,
                                         unsigned int nr_pages)
{
    struct cached_page *cp, key;
    struct avl_iter iter;
    struct avl_node* node;
    unsigned int ret = 0;

    if (!nr_pages) return 0;

    key.offset = *offset;

    spin_lock(&cache->tree_lock);
    avl_start_iter(&cache->pages, &iter, &key, AVL_GREATER_EQUAL);
    for (; (node = avl_get_iter(&iter)); avl_inc_iter(&iter)) {
        cp = avl_entry(node, struct cached_page, avl);

        if (cp->offset >= end) break;
        if (!(cp->flags & tag)) continue;

        pages[ret] = cp;
        if (++ret == nr_pages) {
            *offset = cp->offset + (thp_nr_pages(cp) << ARCH_PG_SHIFT);
            goto out;
        }
    }

    if (end == (unsigned long)-1)
        *offset = (unsigned long)-1;
    else
        *offset = end;

out:
    spin_unlock(&cache->tree_lock);
    return ret;
}

int page_cache_sync_range(int fd, unsigned long start, unsigned long end)
{
    struct address_space* cache = cache_get_by_fd(fd);
    struct cached_page* pvec[16];
    unsigned long index = start;
    int i, nr_pages;
    ssize_t err;
    int r = 0;

    if (!cache->nrpages) return 0;

    while (index < end) {
        nr_pages =
            pagevec_lookup_range(cache, &index, end, CPF_DIRTY, pvec, 16);
        if (nr_pages == 0) break;

        for (i = 0; i < nr_pages; i++) {
            struct cached_page* cp = pvec[i];
            int j;

            lock_cached_page(cp);

            /* Page changed */
            if (!(cp->flags & CPF_DIRTY) ||
                unlikely((cp->offset < start) || (cp->offset >= end))) {
                unlock_cached_page(cp);
                continue;
            }

            err = spu_write(fd, __va(cp->pages[0]->phys_addr),
                            thp_nr_pages(cp) << ARCH_PG_SHIFT, cp->offset);

            if (err < 0) {
                unlock_cached_page(cp);
                r = -err;
                goto out;
            }

            for (j = 0; j < thp_nr_pages(cp); j++) {
                cp->pages[j]->flags &= ~PFF_DIRTY;
            }
            cp->flags &= ~CPF_DIRTY;

            unlock_cached_page(cp);
        }

        schedule();
    }

out:
    return r;
}
