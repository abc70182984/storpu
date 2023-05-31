#include <errno.h>
#include <stddef.h>
#include <xil_assert.h>

#include <config.h>
#include <types.h>
#include <list.h>
#include <const.h>
#include <page.h>
#include "proto.h"
#include <cpulocals.h>
#include <spinlock.h>

#define MINPAGES      1
#define MAXPAGES      12
#define MAX_FREEPAGES 128

#define freelist_index(nr_pages) ((nr_pages)-MINPAGES)

struct hole {
    struct hole* h_next;
    unsigned long h_base;
    size_t h_len;
};

#define NR_HOLES 512

struct freelist {
    struct list_head list;
    size_t count;
};

struct zonedat {
    unsigned int index;
    spinlock_t lock;
    phys_addr_t base;
    phys_addr_t limit;
    struct hole* hole_head;     /* pointer to first hole */
    struct hole* free_slots;    /* ptr to list of unused table slots */
    struct hole hole[NR_HOLES]; /* the hole table */
};

static struct zonedat zones[MEMZONE_MAX];

static struct freelist freelists[NR_CPUS][MEMZONE_MAX][MAXPAGES - MINPAGES + 1];

static void free_mem_zone(struct zonedat* zone, phys_addr_t base, size_t len);

static void delete_slot(struct zonedat* zone, struct hole* prev_ptr,
                        struct hole* hp);
static void merge_hole(struct zonedat* zone, struct hole* hp);

static inline void freelist_init(struct freelist* freelist)
{
    INIT_LIST_HEAD(&freelist->list);
    freelist->count = 0;
}

static inline phys_addr_t freelist_alloc(struct freelist* freelist)
{
    struct list_head* next;

    if (freelist->count == 0) return 0;
    Xil_AssertNonvoid(!list_empty(&freelist->list));

    next = freelist->list.next;
    list_del(next);
    freelist->count--;

    return __pa(next);
}

static inline struct freelist*
freelist_get(struct zonedat* zone, phys_addr_t base, unsigned long length)
{
    int nr_pages;
    struct freelist* freelist;

    if ((base % ARCH_PG_SIZE != 0) || (length % ARCH_PG_SIZE != 0)) return NULL;

    nr_pages = length >> ARCH_PG_SHIFT;

    if (nr_pages < MINPAGES || nr_pages > MAXPAGES) return NULL;

    freelist = &freelists[cpuid][zone->index][freelist_index(nr_pages)];
    if (freelist->count >= MAX_FREEPAGES) return NULL;

    return freelist;
}

static inline void freelist_free(struct freelist* freelist, phys_addr_t base)
{
    struct list_head* head = (struct list_head*)__va(base);
    list_add(head, &freelist->list);
    freelist->count++;
}

void mem_init(int mem_zone, phys_addr_t mem_start, size_t free_mem_size)
{
    struct hole* hp;
    struct zonedat* zone = &zones[mem_zone];
    int i, j, k;

    zone->index = mem_zone;
    for (hp = &zone->hole[0]; hp < &zone->hole[NR_HOLES]; hp++) {
        hp->h_next = hp + 1;
        hp->h_base = 0;
        hp->h_len = 0;
    }
    zone->hole[NR_HOLES - 1].h_next = NULL;
    zone->hole_head = NULL;
    zone->free_slots = &zone->hole[0];
    zone->base = mem_start;
    zone->limit = mem_start + free_mem_size;
    spinlock_init(&zone->lock);

    for (i = 0; i < NR_CPUS; i++)
        for (j = 0; j < MEMZONE_MAX; j++)
            for (k = MINPAGES; k <= MAXPAGES; k++)
                freelist_init(&freelists[i][j][freelist_index(k)]);

    free_mem_zone(zone, mem_start, free_mem_size);
}

static phys_addr_t alloc_mem_zone(struct zonedat* zone, size_t memsize,
                                  size_t alignment)
{
    struct hole *hp, *prev_ptr;
    phys_addr_t old_base;

    spin_lock(&zone->lock);

    prev_ptr = NULL;
    hp = zone->hole_head;
    while (hp != NULL) {
        size_t offset = 0;
        if (hp->h_base % alignment != 0)
            offset = alignment - (hp->h_base % alignment);
        if (hp->h_len >= memsize + offset) {
            old_base = hp->h_base + offset;
            hp->h_base += memsize + offset;
            hp->h_len -= (memsize + offset);
            if (prev_ptr && prev_ptr->h_base + prev_ptr->h_len == old_base)
                prev_ptr->h_len += offset;

            if (hp->h_len == 0) delete_slot(zone, prev_ptr, hp);

            spin_unlock(&zone->lock);
            return old_base;
        }

        prev_ptr = hp;
        hp = hp->h_next;
    }

    spin_unlock(&zone->lock);
    return 0;
}

phys_addr_t alloc_mem(size_t memsize, size_t alignment, int flags)
{
    int i;
    phys_addr_t addr;

    for (i = 0; i < MEMZONE_MAX; i++) {
        struct zonedat* zone;

        if (!(flags & (1 << i))) continue;
        zone = &zones[i];

        addr = alloc_mem_zone(zone, memsize, alignment);
        if (addr) return addr;
    }

    return 0;
}

/* ~alloc_pages~ returns physical memory addresses while ~alloc_vmpages~ returns
 * mapped virtual addresses. Currently we use identity mapping for the lower
 * half so VA == PA but we should not rely on that. */
phys_addr_t alloc_pages(unsigned long nr_pages, int flags)
{
    int i;
    phys_addr_t addr;

    if (nr_pages >= MINPAGES && nr_pages <= MAXPAGES) {
        for (i = 0; i < MEMZONE_MAX; i++) {

            if (!(flags & (1 << i))) continue;

            addr =
                freelist_alloc(&freelists[cpuid][i][freelist_index(nr_pages)]);
            if (addr) return addr;
        }
    }

    return alloc_mem(nr_pages << ARCH_PG_SHIFT, ARCH_PG_SIZE, flags);
}

void* alloc_vmpages(unsigned long nr_pages, int flags)
{
    phys_addr_t addr;

    addr = alloc_pages(nr_pages, flags);
    if (!addr) return NULL;

    return __va(addr);
}

static void free_mem_zone(struct zonedat* zone, phys_addr_t base, size_t len)
{
    struct hole *hp, *new_ptr, *prev_ptr;
    struct freelist* freelist;

    if (len == 0) return;

    freelist = freelist_get(zone, base, len);
    if (freelist) {
        freelist_free(freelist, base);
        return;
    }

    spin_lock(&zone->lock);

    if ((new_ptr = zone->free_slots) == NULL) {
        spin_unlock(&zone->lock);
        panic("hole table full");
    }

    new_ptr->h_base = base;
    new_ptr->h_len = len;
    zone->free_slots = new_ptr->h_next;
    hp = zone->hole_head;

    if (hp == NULL || base <= hp->h_base) {
        new_ptr->h_next = hp;
        zone->hole_head = new_ptr;
        merge_hole(zone, new_ptr);

        spin_unlock(&zone->lock);
        return;
    }

    prev_ptr = NULL;
    while (hp != NULL && base > hp->h_base) {
        prev_ptr = hp;
        hp = hp->h_next;
    }

    new_ptr->h_next = prev_ptr->h_next;
    prev_ptr->h_next = new_ptr;
    merge_hole(zone, prev_ptr);

    spin_unlock(&zone->lock);
}

void free_mem(phys_addr_t base, size_t len)
{
    struct zonedat* zone;

    for (zone = zones; zone < &zones[MEMZONE_MAX]; zone++) {
        if ((base >= zone->base) && (base < zone->limit)) {
            free_mem_zone(zone, base, len);
            return;
        }
    }

    panic("freeing memory outside of any zone");
}

static void delete_slot(struct zonedat* zone, struct hole* prev_ptr,
                        struct hole* hp)
{
    if (hp == zone->hole_head)
        zone->hole_head = hp->h_next;
    else
        prev_ptr->h_next = hp->h_next;

    hp->h_next = zone->free_slots;
    hp->h_base = hp->h_len = 0;
    zone->free_slots = hp;
}

static void merge_hole(struct zonedat* zone, struct hole* hp)
{
    struct hole* next_ptr;

    if ((next_ptr = hp->h_next) == NULL) return;
    if (hp->h_base + hp->h_len == next_ptr->h_base) {
        hp->h_len += next_ptr->h_len;
        delete_slot(zone, hp, next_ptr);
    } else {
        hp = next_ptr;
    }

    if ((next_ptr = hp->h_next) == NULL) return;
    if (hp->h_base + hp->h_len == next_ptr->h_base) {
        hp->h_len += next_ptr->h_len;
        delete_slot(zone, hp, next_ptr);
    }
}
