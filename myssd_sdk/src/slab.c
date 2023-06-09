#include <string.h>

#include <config.h>
#include <types.h>
#include <const.h>
#include <bitmap.h>
#include <list.h>
#include "proto.h"
#include <page.h>
#include <spinlock.h>
#include <cpulocals.h>
#include <memalloc.h>

#define OBJ_ALIGN 4

#define SLABSIZE 256
#define MINSIZE  8
#define MAXSIZE  ((SLABSIZE - 1 + MINSIZE / OBJ_ALIGN) * OBJ_ALIGN)

struct slabdata;

struct slabheader {
    struct list_head list;
    bitchunk_t used_mask[BITCHUNKS(ARCH_PG_SIZE / MINSIZE)];
    unsigned short freeguess;
    unsigned short used;
    struct slabdata* data;
};

#define DATABYTES (ARCH_PG_SIZE - sizeof(struct slabheader))

struct slabdata {
    unsigned char data[DATABYTES];
    struct slabheader header;
};

struct slab {
    spinlock_t lock;
    struct list_head list;
};

static struct slab slabs[SLABSIZE];

#define ARRAY_CACHE_SIZE 128

struct array_cache {
    unsigned int avail;
    unsigned int limit;
    void** entry;
};

static DEFINE_CPULOCAL(struct array_cache, cpucache[SLABSIZE]);

#define SLAB_INDEX(bytes) ((roundup(bytes, OBJ_ALIGN) - MINSIZE) / OBJ_ALIGN)

void slabs_init()
{
    int i, j;
    for (i = 0; i < SLABSIZE; i++) {
        spinlock_init(&slabs[i].lock);
        INIT_LIST_HEAD(&slabs[i].list);
    }

    for (i = 0; i < NR_CPUS; i++) {
        size_t entry_size;
        void** ptr;

        entry_size = ARRAY_CACHE_SIZE * sizeof(void*) * SLABSIZE;
        entry_size = roundup(entry_size, ARCH_PG_SIZE);
        ptr = alloc_vmpages(entry_size >> ARCH_PG_SHIFT, ZONE_PS_DDR);
        if (!ptr) panic("failed to allocate CPU cache entries");

        for (j = 0; j < SLABSIZE; j++) {
            struct array_cache* ac = &get_cpu_var(i, cpucache)[j];
            ac->avail = 0;
            ac->limit = ARRAY_CACHE_SIZE;
            ac->entry = ptr;
            ptr += ARRAY_CACHE_SIZE;
        }
    }
}

static struct slabdata* alloc_slabdata()
{
    struct slabdata* sd = (struct slabdata*)alloc_pages(1, ZONE_PS_DDR);
    if (!sd) return NULL;

    memset(&sd->header.used_mask, 0, sizeof(sd->header.used_mask));
    sd->header.used = 0;
    sd->header.freeguess = 0;
    INIT_LIST_HEAD(&(sd->header.list));
    sd->header.data = sd;

    return sd;
}

void* slaballoc(size_t bytes)
{
    struct array_cache* ac;
    void* ptr = NULL;

    if (bytes > MAXSIZE || bytes < MINSIZE) {
        /* printk("mm: slaballoc: invalid size(%d bytes)\n", bytes); */
        return NULL;
    }

    ac = &get_cpulocal_var(cpucache)[SLAB_INDEX(bytes)];
    if (ac->avail) {
        void* ptr = ac->entry[ac->avail - 1];
        ac->avail--;
        return ptr;
    }

    struct slab* slab = &slabs[SLAB_INDEX(bytes)];
    struct slabdata* sd = NULL;
    struct slabheader* header;

    spin_lock(&slab->lock);

    bytes = roundup(bytes, OBJ_ALIGN);

    if (list_empty(&slab->list)) {
        sd = alloc_slabdata();
        if (!sd) goto out;

        list_add(&sd->header.list, &slab->list);
    }

    int i;
    int max_objs = DATABYTES / bytes;
    list_for_each_entry(header, &slab->list, list)
    {
        if (header->used == max_objs) continue;

        i = find_next_zero_bit(header->used_mask, max_objs, header->freeguess);
        if (i < max_objs) {
            sd = header->data;
            SET_BIT(header->used_mask, i);
            header->freeguess = i + 1;
            header->used++;

            ptr = (void*)&sd->data[i * bytes];
            goto out;
        }

        i = find_next_zero_bit(header->used_mask, header->freeguess, 0);
        if (i < header->freeguess) {
            sd = header->data;
            SET_BIT(header->used_mask, i);
            header->freeguess = i + 1;
            header->used++;

            ptr = (void*)&sd->data[i * bytes];
            goto out;
        }
    }

    sd = alloc_slabdata();
    if (!sd) goto out;

    header = &sd->header;
    list_add(&header->list, &slab->list);

    i = find_next_zero_bit(header->used_mask, max_objs, 0);

    if (i < max_objs) {
        SET_BIT(header->used_mask, i);
        header->freeguess = i + 1;
        header->used++;

        ptr = (void*)&sd->data[i * bytes];
        goto out;
    }

out:
    spin_unlock(&slab->lock);
    return ptr;
}

void slabfree(void* mem, size_t bytes)
{
    struct array_cache* ac;

    if (bytes > MAXSIZE || bytes < MINSIZE) {
        return;
    }

    ac = &get_cpulocal_var(cpucache)[SLAB_INDEX(bytes)];
    if (ac->avail < ac->limit) {
        ac->entry[ac->avail++] = mem;
        return;
    }

    struct slab* slab = &slabs[SLAB_INDEX(bytes)];
    struct slabdata* sd;
    struct slabheader* header;

    bytes = roundup(bytes, OBJ_ALIGN);

    spin_lock(&slab->lock);

    if (list_empty(&slab->list)) goto out;

    list_for_each_entry(header, &slab->list, list)
    {
        if (header->used == 0) continue;
        sd = header->data;

        if ((mem >= (void*)&sd->data) && (mem < (void*)&sd->data + DATABYTES)) {
            int i = (mem - (void*)&sd->data) / bytes;
            UNSET_BIT(header->used_mask, i);
            header->used--;
            header->freeguess = i;
            goto out;
        }
    }

out:
    spin_unlock(&slab->lock);
}
