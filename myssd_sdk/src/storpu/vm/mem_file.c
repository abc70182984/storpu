#include <xil_assert.h>
#include <errno.h>
#include <string.h>
#include <flash_config.h>

#include <types.h>
#include <storpu/vm.h>
#include <storpu/file.h>
#include <memalloc.h>

#include "region.h"
#include "cache.h"

static int file_map_pt_flags(const struct vm_region* vr) { return 0; }

static inline void fault_dirty_shared_page(struct vm_region* vr,
                                           struct phys_region* pr,
                                           unsigned int flags)
{
    struct page* page;

    Xil_AssertVoid(vr->flags & RF_MAP_SHARED);
    Xil_AssertVoid(flags & FAULT_FLAG_WRITE);
    Xil_AssertVoid(pr->page->phys_addr != PHYS_NONE);

    page = pr->page;
    page->flags |= PFF_DIRTY;

    if (page->flags & PFF_INCACHE) mark_cached_page_dirty(page);
}

static int file_map_page_fault(struct vm_context* ctx, struct vm_region* vr,
                               struct phys_region* pr, unsigned int flags)
{
    int fd = vr->param.file.fd;
    int r;

    Xil_AssertNonvoid(pr->page->refcount > 0);
    Xil_AssertNonvoid(fd != -1);
    Xil_AssertNonvoid(vr->param.file.inited);

    if (pr->page->phys_addr == PHYS_NONE) {
        struct cached_page* cp;
        int use_hugepage;
        size_t allocsize;
        ssize_t nbytes;
        unsigned long fd_offset = vr->param.file.offset + pr->offset;
        unsigned long ref_offset;

        /* Promote to huge page for flash pages. */
        use_hugepage = fd != FD_HOST_MEM;
        allocsize = use_hugepage ? FLASH_PG_SIZE : ARCH_PG_SIZE;

        ref_offset = rounddown(fd_offset, allocsize);

    retry:
        cp = find_cached_page(fd, ref_offset, FCP_LOCK);

        if (!cp) {
            phys_addr_t buf_phys;

            /* Need to block now. */
            if (!(flags & FAULT_FLAG_INTERRUPTIBLE)) return EFAULT;

            buf_phys = alloc_pages(allocsize >> ARCH_PG_SHIFT, ZONE_PS_DDR);
            if (!buf_phys) return ENOMEM;

            nbytes = spu_read(fd, __va(buf_phys), allocsize, ref_offset);
            if (nbytes != allocsize) {
                free_mem(buf_phys, allocsize);
                return EFAULT;
            }

            r = page_cache_add(fd, ref_offset, buf_phys, use_hugepage, &cp);
            if (r != 0) {
                free_mem(buf_phys, allocsize);
                if (r == EEXIST) goto retry;
                return r;
            }
        }

        /* Page must be in cache and locked by now. */
        Xil_AssertNonvoid(cp);

        page_unreference(vr, pr, FALSE);
        page_link(pr, find_subpage(cp, fd_offset), pr->offset, vr);

        unlock_cached_page(cp);

        if ((flags & FAULT_FLAG_WRITE) && (vr->flags & RF_MAP_SHARED)) {
            fault_dirty_shared_page(vr, pr, flags);
        }

        return 0;
    }

    if ((flags & FAULT_FLAG_WRITE) && (vr->flags & RF_MAP_SHARED)) {
        fault_dirty_shared_page(vr, pr, flags);
        return 0;
    }

    /* return page_cow(vr, pr, PHYS_NONE); */
    return EINVAL;
}

static int file_map_writable(const struct phys_region* pr)
{
    struct vm_region* vr = pr->parent;
    if (vr->flags & RF_MAP_SHARED) {
        return (vr->flags & RF_WRITE) && (pr->page->flags & PFF_DIRTY);
    }

    return 0;
}

static int file_map_unreference(struct phys_region* pr)
{
    Xil_AssertNonvoid(pr->page->refcount == 0);
    if (pr->page->phys_addr != PHYS_NONE && !(pr->page->flags & PFF_INCACHE))
        free_mem(pr->page->phys_addr, ARCH_PG_SIZE);
    return 0;
}

static void file_map_split(struct vm_context* ctx, struct vm_region* vr,
                           struct vm_region* r1, struct vm_region* r2)
{
    Xil_AssertVoid(!r1->param.file.inited);
    Xil_AssertVoid(!r2->param.file.inited);
    Xil_AssertVoid(vr->param.file.inited);
    Xil_AssertVoid(r1->length + r2->length == vr->length);
    Xil_AssertVoid(vr->rops == &file_map_ops);
    Xil_AssertVoid(r1->rops == &file_map_ops);
    Xil_AssertVoid(r2->rops == &file_map_ops);

    r1->param.file = vr->param.file;
    r2->param.file = vr->param.file;

    r2->param.file.offset += r1->length;

    Xil_AssertVoid(r1->param.file.inited);
    Xil_AssertVoid(r2->param.file.inited);
}

static int file_map_shrink_low(struct vm_region* vr, size_t len)
{
    Xil_AssertNonvoid(vr->param.file.inited);
    vr->param.file.offset += len;
    return 0;
}

static int file_map_sync_range(struct vm_region* vr, unsigned long start,
                               unsigned long end)
{
    unsigned long fstart, fend;

    Xil_AssertNonvoid(vr->param.file.inited);
    Xil_AssertNonvoid(vr->param.file.fd != -1);
    Xil_AssertNonvoid(start <= end);

    fstart = vr->param.file.offset + start;
    fend = vr->param.file.offset + end;

    return page_cache_sync_range(vr->param.file.fd, fstart, fend);
}

static void file_map_delete(struct vm_region* vr)
{
    Xil_AssertVoid(vr->rops == &file_map_ops);
    Xil_AssertVoid(vr->param.file.inited);
    Xil_AssertVoid(vr->param.file.fd != -1);
    vr->param.file.fd = -1;
    vr->param.file.inited = FALSE;
}

const struct region_operations file_map_ops = {
    .rop_delete = file_map_delete,
    .rop_pt_flags = file_map_pt_flags,
    .rop_shrink_low = file_map_shrink_low,
    .rop_split = file_map_split,
    .rop_page_fault = file_map_page_fault,

    .rop_writable = file_map_writable,
    .rop_unreference = file_map_unreference,

    .rop_sync_range = file_map_sync_range,
};

int file_map_set_file(struct vm_context* ctx, struct vm_region* vr, int fd,
                      unsigned long offset)
{
    Xil_AssertNonvoid(!vr->param.file.inited);

    vr->param.file.fd = fd;
    vr->param.file.offset = offset;
    vr->param.file.inited = TRUE;

    return 0;
}
