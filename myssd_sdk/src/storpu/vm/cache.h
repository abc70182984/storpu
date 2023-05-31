#ifndef _VM_CACHE_H_
#define _VM_CACHE_H_

#include <const.h>
#include <avl.h>
#include <page.h>
#include <storpu/mutex.h>

#define HP_NR_PAGES 4

/* Cached page flags */
#define CPF_HUGEPAGE BIT(0)
#define CPF_DIRTY    BIT(1)

/* Find cached page flags */
#define FCP_LOCK 1

struct cached_page {
    struct avl_node avl;
    mutex_t lock;

    int fd;
    unsigned long offset;
    int flags;

    struct page* pages[HP_NR_PAGES];
};

static inline int thp_nr_pages(struct cached_page* cp)
{
    return (cp->flags & CPF_HUGEPAGE) ? HP_NR_PAGES : 1;
}

static inline void lock_cached_page(struct cached_page* cp)
{
    mutex_lock(&cp->lock);
}

static inline void unlock_cached_page(struct cached_page* cp)
{
    mutex_unlock(&cp->lock);
}

static inline struct page* find_subpage(struct cached_page* cp,
                                        unsigned long offset)
{
    unsigned int index;

    if (!(cp->flags & CPF_HUGEPAGE)) return cp->pages[0];

    index = offset >> ARCH_PG_SHIFT;
    return cp->pages[index & (HP_NR_PAGES - 1)];
}

static inline void mark_cached_page_dirty(struct page* page)
{
    struct cached_page* cp = page->private;
    cp->flags |= CPF_DIRTY;
}

struct cached_page* find_cached_page(int fd, unsigned long offset, int flags);
int page_cache_add(int fd, unsigned long offset, phys_addr_t phys, int hugepage,
                   struct cached_page** cpp);

int page_cache_sync_range(int fd, unsigned long start, unsigned long end);

void page_cache_init(void);

#endif
