#include <xil_assert.h>
#include <string.h>
#include <errno.h>

#include <types.h>
#include <const.h>
#include <storpu/vm.h>
#include <pagetable.h>
#include <memalloc.h>
#include <slab.h>

#include "region.h"

#define FREE_REGION_FAILED ((unsigned long)-1)

static inline size_t phys_slot(unsigned long offset)
{
    Xil_AssertNonvoid(!(offset % ARCH_PG_SIZE));
    return offset >> ARCH_PG_SHIFT;
}

static inline size_t phys_regions_size(struct vm_region* vr)
{
    return phys_slot(vr->length) * sizeof(struct phys_region*);
}

static struct phys_region** phys_regions_alloc(size_t size)
{
    void* ptr;

    size = roundup(size, 16);
    size += 16;
    ptr = slaballoc(size);

    if (!ptr) {
        size = roundup(size, ARCH_PG_SIZE);
        ptr = alloc_vmpages(size >> ARCH_PG_SHIFT, ZONE_PS_DDR);
        if (!ptr) return NULL;

        size |= 1;
    }

    *(u32*)ptr = (u32)size;
    return ptr + 16;
}

void phys_regions_free(struct phys_region** prs)
{
    void* p = (void*)prs - 16;
    u32 size = *(u32*)p;

    if (size & 1)
        free_mem(__pa(p), size & ~1);
    else
        slabfree(p, size);
}

struct phys_region* phys_region_get(struct vm_region* vr, unsigned long offset)
{
    size_t i;
    struct phys_region* pr;

    Xil_AssertNonvoid(offset < vr->length);
    Xil_AssertNonvoid(vr->phys_regions);
    Xil_AssertNonvoid(!(offset % ARCH_PG_SIZE));

    i = phys_slot(offset);
    pr = vr->phys_regions[i];

    if (pr) Xil_AssertNonvoid(pr->offset == offset);

    return pr;
}

void phys_region_set(struct vm_region* vr, unsigned long offset,
                     struct phys_region* pr)
{
    size_t i;
    struct vm_context* ctx;

    Xil_AssertVoid(offset < vr->length);
    Xil_AssertVoid(vr->phys_regions);
    Xil_AssertVoid(!(offset % ARCH_PG_SIZE));

    i = phys_slot(offset);

    ctx = vr->ctx;
    Xil_AssertVoid(ctx);

    if (pr) {
        Xil_AssertVoid(!vr->phys_regions[i]);
        Xil_AssertVoid(pr->offset == offset);
        ctx->vm_total += ARCH_PG_SIZE;
    } else {
        Xil_AssertVoid(vr->phys_regions[i]);
        ctx->vm_total -= ARCH_PG_SIZE;
    }

    vr->phys_regions[i] = pr;
}

int phys_region_resize(struct vm_region* vr, size_t new_length)
{
    size_t cur_slots, new_slots;
    size_t cur_capacity, new_capacity;
    struct phys_region** new_prs;

    Xil_AssertNonvoid(vr->length);
    Xil_AssertNonvoid(new_length);

    cur_capacity = vr->pr_capacity;
    cur_slots = phys_slot(vr->length);
    new_slots = phys_slot(new_length);

    if (cur_capacity < new_slots) {
        new_capacity = cur_capacity;
        while (new_capacity < new_slots)
            new_capacity <<= 1;

        new_prs =
            phys_regions_alloc(new_capacity * sizeof(struct phys_region*));
        if (!new_prs) return ENOMEM;

        memcpy(new_prs, vr->phys_regions,
               min(cur_slots, new_slots) * sizeof(struct phys_region*));

        phys_regions_free(vr->phys_regions);
        vr->phys_regions = new_prs;
        vr->pr_capacity = new_capacity;
    }

    if (new_slots > cur_slots)
        memset(vr->phys_regions + cur_slots, 0,
               (new_slots - cur_slots) * sizeof(struct phys_region*));

    return 0;
}

static inline int phys_region_writable(struct vm_region* vr,
                                       struct phys_region* pr)
{
    Xil_AssertNonvoid(pr->rops->rop_writable);
    return (vr->flags & RF_WRITE) && (pr->rops->rop_writable(pr));
}

static inline pgprot_t phys_region_page_prot(struct vm_region* vr,
                                             struct phys_region* pr)
{
    static const pgprot_t protection_map[] = {
        __P000, __P001, __P010, __P011, __P100, __P101, __P110, __P111,
    };
    pgprot_t prot;

    unsigned long flags = 0;
    int vr_flags = vr->flags & (RF_READ | RF_EXEC);
    if (phys_region_writable(vr, pr)) vr_flags |= RF_WRITE;

    if (vr->rops->rop_pt_flags) flags |= vr->rops->rop_pt_flags(vr);

    prot = __pgprot(pgprot_val(protection_map[vr_flags]) | flags);

    if (vr->flags & RF_IO) prot = pgprot_noncached(prot);

    return prot;
}

struct vm_region* region_new(struct vm_context* ctx, unsigned long base,
                             size_t length, int flags,
                             const struct region_operations* rops)
{
    struct vm_region* region;
    struct phys_region** prs;
    size_t capacity = 4;

    SLABALLOC(region);
    if (!region) return NULL;

    memset(region, 0, sizeof(*region));
    region->vir_addr = base;
    region->length = length;
    region->flags = flags;
    region->ctx = ctx;
    region->rops = rops;

    while (capacity < phys_slot(region->length))
        capacity <<= 1;

    if ((prs = phys_regions_alloc(capacity * sizeof(struct phys_region*))) ==
        NULL) {
        SLABFREE(region);
        return NULL;
    }

    memset(prs, 0, phys_regions_size(region));
    region->phys_regions = prs;
    region->pr_capacity = capacity;

    return region;
}

static unsigned long region_find_free_region(struct vm_context* ctx,
                                             unsigned long minv,
                                             unsigned long maxv, size_t len)
{
    int found = 0;
    unsigned long vaddr;

    if (!maxv) maxv = minv + len;
    if (minv + len > maxv) return FREE_REGION_FAILED;

    struct avl_iter iter;
    struct vm_region vr_max;
    vr_max.vir_addr = maxv;
    region_avl_start_iter(&ctx->mem_avl, &iter, &vr_max, AVL_GREATER_EQUAL);
    struct vm_region* last = region_avl_get_iter(&iter);

#define TRY_ALLOC_REGION(start, end)                              \
    do {                                                          \
        unsigned long rstart = ((start) > minv) ? (start) : minv; \
        unsigned long rend = ((end) < maxv) ? (end) : maxv;       \
        if (rend > rstart && (rend - rstart >= len)) {            \
            vaddr = rend - len;                                   \
            found = 1;                                            \
        }                                                         \
    } while (0)

#define ALLOC_REGION(start, end)                                      \
    do {                                                              \
        TRY_ALLOC_REGION((start) + ARCH_PG_SIZE, (end)-ARCH_PG_SIZE); \
        if (!found) {                                                 \
            TRY_ALLOC_REGION(start, end);                             \
        }                                                             \
    } while (0)

    if (!last) {
        region_avl_start_iter(&ctx->mem_avl, &iter, &vr_max, AVL_LESS);
        last = region_avl_get_iter(&iter);
        ALLOC_REGION(last ? (last->vir_addr + last->length) : VM_USER_START,
                     VM_STACK_TOP);
    }

    if (!found) {
        struct vm_region* vr;
        while ((vr = region_avl_get_iter(&iter)) && !found) {
            struct vm_region* nextvr;
            region_avl_dec_iter(&iter);
            nextvr = region_avl_get_iter(&iter);
            ALLOC_REGION(nextvr ? nextvr->vir_addr + nextvr->length
                                : VM_USER_START,
                         vr->vir_addr);
        }
    }

    return found ? vaddr : FREE_REGION_FAILED;
}

struct vm_region* region_map(struct vm_context* ctx, unsigned long minv,
                             unsigned long maxv, unsigned long length,
                             int flags, int map_flags,
                             const struct region_operations* rops)
{
    unsigned long startv;
    struct vm_region* vr;

    startv = region_find_free_region(ctx, minv, maxv, length);
    if (startv == FREE_REGION_FAILED) return NULL;

    if ((vr = region_new(ctx, startv, length, flags, rops)) == NULL)
        return NULL;

    if (vr->rops->rop_new && vr->rops->rop_new(vr) != 0) {
        region_free(vr);
        return NULL;
    }

    if (map_flags & MRF_PREALLOC) {
        if (region_handle_memory(ctx, vr, 0, length, FAULT_FLAG_WRITE) != 0) {
            region_free(vr);
            return NULL;
        }
    }

    vr->flags &= ~RF_UNINITIALIZED;

    list_add(&vr->list, &ctx->mem_regions);
    avl_insert(&vr->avl, &ctx->mem_avl);

    return vr;
}

static int region_split(struct vm_context* ctx, struct vm_region* vr,
                        size_t len, struct vm_region** v1,
                        struct vm_region** v2)
{
    struct vm_region *vr1 = NULL, *vr2 = NULL;
    size_t rem_len = vr->length - len;
    size_t slot1, slot2;
    unsigned long off;

    if (!vr->rops->rop_split) return EINVAL;

    Xil_AssertNonvoid(!(len % ARCH_PG_SIZE));
    Xil_AssertNonvoid(!(rem_len % ARCH_PG_SIZE));
    Xil_AssertNonvoid(!(vr->vir_addr % ARCH_PG_SIZE));
    Xil_AssertNonvoid(!(vr->length % ARCH_PG_SIZE));

    slot1 = phys_slot(len);
    slot2 = phys_slot(rem_len);
    Xil_AssertNonvoid(slot1 + slot2 == phys_slot(vr->length));

    if (!(vr1 = region_new(ctx, vr->vir_addr, len, vr->flags, vr->rops)))
        goto failed;
    if (!(vr2 = region_new(ctx, vr->vir_addr + len, rem_len, vr->flags,
                           vr->rops)))
        goto failed;

    for (off = 0; off < vr1->length; off += ARCH_PG_SIZE) {
        struct phys_region* pr;
        if (!(pr = phys_region_get(vr, off))) continue;
        if (!page_reference(pr->page, off, vr1, pr->rops)) goto failed;
    }

    for (off = 0; off < vr2->length; off += ARCH_PG_SIZE) {
        struct phys_region* pr;
        if (!(pr = phys_region_get(vr, len + off))) continue;
        if (!page_reference(pr->page, off, vr2, pr->rops)) goto failed;
    }

    vr->rops->rop_split(ctx, vr, vr1, vr2);

    list_del(&vr->list);
    avl_erase(&vr->avl, &ctx->mem_avl);
    region_free(vr);

    list_add(&vr1->list, &ctx->mem_regions);
    avl_insert(&vr1->avl, &ctx->mem_avl);
    list_add(&vr2->list, &ctx->mem_regions);
    avl_insert(&vr2->avl, &ctx->mem_avl);

    *v1 = vr1;
    *v2 = vr2;

    return 0;

failed:
    if (vr1) region_free(vr1);
    if (vr2) region_free(vr2);

    return ENOMEM;
}

static int region_subfree(struct vm_region* rp, unsigned long start, size_t len)
{
    struct phys_region* pr;
    unsigned long end = start + len;
    unsigned long offset;

    Xil_AssertNonvoid(!(start % ARCH_PG_SIZE));

    for (offset = start; offset < end; offset += ARCH_PG_SIZE) {
        if ((pr = phys_region_get(rp, offset)) == NULL) continue;

        Xil_AssertNonvoid(pr->offset >= start);
        Xil_AssertNonvoid(pr->offset < end);
        page_unreference(rp, pr, TRUE /* remove */);
        SLABFREE(pr);
    }

    return 0;
}

static int region_unmap(struct vm_context* ctx, struct vm_region* vr,
                        unsigned long offset, size_t len)
{
    int retval;
    size_t new_slots;
    struct phys_region* pr;
    size_t free_slots = phys_slot(len);
    unsigned long unmap_start;
    unsigned long voff;

    Xil_AssertNonvoid(offset + len <= vr->length);
    Xil_AssertNonvoid(!(len % ARCH_PG_SIZE));

    region_subfree(vr, offset, len);

    unmap_start = vr->vir_addr + offset;

    if (len == vr->length) {
        list_del(&vr->list);
        avl_erase(&vr->avl, &ctx->mem_avl);
        region_free(vr);
    } else if (offset == 0) {
        if (!vr->rops->rop_shrink_low) return EINVAL;

        if ((retval = vr->rops->rop_shrink_low(vr, len)) != 0) return EINVAL;

        list_del(&vr->list);
        avl_erase(&vr->avl, &ctx->mem_avl);

        vr->vir_addr += len;

        Xil_AssertNonvoid(vr->length > len);
        new_slots = phys_slot(vr->length - len);
        Xil_AssertNonvoid(new_slots);

        list_add(&vr->list, &ctx->mem_regions);
        avl_insert(&vr->avl, &ctx->mem_avl);

        for (voff = len; voff < vr->length; voff += ARCH_PG_SIZE) {
            if (!(pr = phys_region_get(vr, voff))) continue;
            Xil_AssertNonvoid(pr->offset >= offset);
            Xil_AssertNonvoid(pr->offset >= len);
            pr->offset -= len;
        }

        memmove(vr->phys_regions, vr->phys_regions + free_slots,
                new_slots * sizeof(struct phys_region*));

        vr->length -= len;
    } else if (offset + len == vr->length) {
        if ((retval = phys_region_resize(vr, vr->length - len)) != 0)
            return retval;

        vr->length -= len;
    }

    pgd_unmap_memory(&ctx->pgd, unmap_start, len);

    return 0;
}

int region_unmap_range(struct vm_context* ctx, unsigned long start, size_t len)
{
    unsigned long offset = start % ARCH_PG_SIZE;
    struct vm_region *vr, *next_vr;
    struct avl_iter iter;
    struct vm_region vr_start;
    unsigned long limit;
    int ret;

    start -= offset;
    len += offset;
    len = roundup(len, ARCH_PG_SIZE);
    limit = start + len;

    vr_start.vir_addr = start;
    region_avl_start_iter(&ctx->mem_avl, &iter, &vr_start, AVL_LESS_EQUAL);
    if (!(vr = region_avl_get_iter(&iter))) {
        region_avl_start_iter(&ctx->mem_avl, &iter, &vr_start, AVL_GREATER);
        if (!(vr = region_avl_get_iter(&iter))) {
            return 0;
        }
    }

    Xil_AssertNonvoid(vr);

    for (; vr && vr->vir_addr < limit; vr = next_vr) {
        region_avl_inc_iter(&iter);
        next_vr = region_avl_get_iter(&iter);

        unsigned long cur_start = max(start, vr->vir_addr);
        unsigned long cur_limit = min(limit, vr->vir_addr + vr->length);
        if (cur_start >= cur_limit) continue;

        /* need spliting */
        if (cur_start > vr->vir_addr && cur_limit < vr->vir_addr + vr->length) {
            struct vm_region *v1, *v2;
            size_t split_len = cur_limit - vr->vir_addr;

            Xil_AssertNonvoid(split_len > 0);
            Xil_AssertNonvoid(split_len < vr->length);

            if ((ret = region_split(ctx, vr, split_len, &v1, &v2))) {
                return ret;
            }
            vr = v1;
        }

        Xil_AssertNonvoid(cur_start >= vr->vir_addr);
        Xil_AssertNonvoid(cur_limit > vr->vir_addr);
        Xil_AssertNonvoid(cur_limit <= vr->vir_addr + vr->length);

        ret = region_unmap(ctx, vr, cur_start - vr->vir_addr,
                           cur_limit - cur_start);

        if (ret) return ret;

        if (next_vr) {
            region_avl_start_iter(&ctx->mem_avl, &iter, next_vr, AVL_EQUAL);
            Xil_AssertNonvoid(region_avl_get_iter(&iter) == next_vr);
        }
    }

    return 0;
}

int region_free(struct vm_region* vr)
{
    int retval;

    Xil_AssertNonvoid(vr->phys_regions);

    if ((retval = region_subfree(vr, 0, vr->length)) != 0) return retval;

    if (vr->rops->rop_delete) vr->rops->rop_delete(vr);
    phys_regions_free(vr->phys_regions);
    vr->phys_regions = NULL;
    SLABFREE(vr);

    return 0;
}

int region_write_map_page(struct vm_context* ctx, struct vm_region* vr,
                          struct phys_region* pr)
{
    struct page* page = pr->page;
    pgprot_t prot;
    int r;

    Xil_AssertNonvoid(vr);
    Xil_AssertNonvoid(pr);
    Xil_AssertNonvoid(page);
    Xil_AssertNonvoid(vr->rops);

    Xil_AssertNonvoid(!(vr->vir_addr % ARCH_PG_SIZE));
    Xil_AssertNonvoid(!(pr->offset % ARCH_PG_SIZE));
    Xil_AssertNonvoid(page->refcount);

    prot = phys_region_page_prot(vr, pr);

    spin_lock(&ctx->pgd_lock);
    r = pgd_writemap(&ctx->pgd, page->phys_addr, vr->vir_addr + pr->offset,
                     ARCH_PG_SIZE, prot);
    spin_unlock(&ctx->pgd_lock);
    if (r != 0) return ENOMEM;

    return 0;
}

int region_write_map_range(struct vm_context* ctx, struct vm_region* vr,
                           unsigned long start, unsigned long end)
{
    struct phys_region* pr;
    unsigned long off;
    int retval;

    Xil_AssertNonvoid(start < end);
    Xil_AssertNonvoid(start < vr->length);
    Xil_AssertNonvoid(end <= vr->length);
    Xil_AssertNonvoid(!(start % ARCH_PG_SIZE));

    for (off = start; off < end; off += ARCH_PG_SIZE) {
        if (!(pr = phys_region_get(vr, off))) continue;

        if ((retval = region_write_map_page(ctx, vr, pr)) != 0) return retval;
    }

    return 0;
}

int region_handle_pf(struct vm_context* ctx, struct vm_region* vr,
                     unsigned long offset, unsigned int flags)
{
    struct phys_region* pr;
    int ret;

    offset = rounddown(offset, ARCH_PG_SIZE);

    Xil_AssertNonvoid(offset < vr->length);
    Xil_AssertNonvoid(!(vr->vir_addr % ARCH_PG_SIZE));
    Xil_AssertNonvoid(!((flags & FAULT_FLAG_WRITE) && !(vr->flags & RF_WRITE)));

    if (!(pr = phys_region_get(vr, offset))) {
        struct page* page;

        if (!(page = page_new(PHYS_NONE))) return ENOMEM;

        if (!(pr = page_reference(page, offset, vr, vr->rops))) {
            page_free(page);
            return ENOMEM;
        }
    }

    Xil_AssertNonvoid(pr);
    Xil_AssertNonvoid(pr->page);
    Xil_AssertNonvoid(pr->rops->rop_writable);

    if (!(flags & FAULT_FLAG_WRITE) || !pr->rops->rop_writable(pr) ||
        pr->page->phys_addr == PHYS_NONE) {
        Xil_AssertNonvoid(pr->rops->rop_page_fault);

        ret = pr->rops->rop_page_fault(ctx, vr, pr, flags);

        if (ret) {
            if (pr) page_unreference(vr, pr, TRUE);
            return ret;
        }

        Xil_AssertNonvoid(pr->page);
        Xil_AssertNonvoid(pr->page->phys_addr != PHYS_NONE);
    }

    Xil_AssertNonvoid(pr->page);
    Xil_AssertNonvoid(pr->page->phys_addr != PHYS_NONE);

    if ((ret = region_write_map_page(ctx, vr, pr)) != 0) return ret;

    return 0;
}

int region_handle_memory(struct vm_context* ctx, struct vm_region* vr,
                         unsigned long offset, size_t len, unsigned int flags)
{
    unsigned long end = offset + len;
    unsigned long off;
    int retval;

    Xil_AssertNonvoid(len > 0);
    Xil_AssertNonvoid(end > offset);

    for (off = offset; off < end; off += ARCH_PG_SIZE) {
        if ((retval = region_handle_pf(ctx, vr, off, flags)) != 0)
            return retval;
    }

    return 0;
}

int region_extend_up_to(struct vm_context* ctx, unsigned long addr)
{
    unsigned offset = ~0;
    struct vm_region *vr, *rb = NULL;
    unsigned long limit, extra;
    int retval;

    addr = roundup(addr, ARCH_PG_SIZE);

    list_for_each_entry(vr, &ctx->mem_regions, list)
    {
        /* need no extend */
        if (addr >= vr->vir_addr && addr <= vr->vir_addr + vr->length) {
            return 0;
        }

        if (addr < vr->vir_addr) continue;
        unsigned roff = addr - vr->vir_addr;
        if (roff < offset) {
            offset = roff;
            rb = vr;
        }
    }

    if (!rb) return EINVAL;

    limit = rb->vir_addr + rb->length;
    extra = addr - limit;

    if (!rb->rops->rop_resize) {
        if (!region_map(ctx, limit, 0, extra, RF_READ | RF_WRITE | RF_ANON, 0,
                        &anon_map_ops))
            return ENOMEM;
        return 0;
    }

    if ((retval = phys_region_resize(rb, addr - rb->vir_addr)) != 0)
        return retval;

    return rb->rops->rop_resize(ctx, rb, addr - rb->vir_addr);
}
