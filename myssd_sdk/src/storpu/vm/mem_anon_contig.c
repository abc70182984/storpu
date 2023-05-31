#include <xil_assert.h>
#include <errno.h>
#include <string.h>

#include <types.h>
#include <storpu/vm.h>
#include <memalloc.h>
#include <utils.h>

#include "region.h"

static int anon_contig_pt_flags(const struct vm_region* vr) { return 0; }

static int anon_contig_resize(struct vm_context* ctx, struct vm_region* vr,
                              size_t len)
{
    return ENOMEM;
}

static int anon_contig_new(struct vm_region* vr)
{
    phys_addr_t new_paddr, paddr;
    size_t pages, i;

    pages = vr->length >> ARCH_PG_SHIFT;
    Xil_AssertNonvoid(pages > 0);

    for (i = 0; i < pages; i++) {
        struct page* page = page_new(PHYS_NONE);
        struct phys_region* pr = NULL;
        if (page)
            pr = page_reference(page, i << ARCH_PG_SHIFT, vr,
                                &anon_contig_map_ops);
        if (!pr) {
            if (page) page_free(page);
            region_free(vr);
            return ENOMEM;
        }
    }

    new_paddr = alloc_pages(pages, ZONE_PS_DDR);
    if (!new_paddr) {
        region_free(vr);
        return ENOMEM;
    }

    paddr = new_paddr;

    for (i = 0; i < pages; i++) {
        struct phys_region* pr;
        pr = phys_region_get(vr, i << ARCH_PG_SHIFT);
        Xil_AssertNonvoid(pr);
        Xil_AssertNonvoid(pr->page);
        Xil_AssertNonvoid(pr->page->phys_addr == PHYS_NONE);
        Xil_AssertNonvoid(pr->offset == i << ARCH_PG_SHIFT);

        memset(__va(paddr + pr->offset), 0, ARCH_PG_SIZE);

        pr->page->phys_addr = paddr + pr->offset;
    }

    return 0;
}

static int anon_contig_page_fault(struct vm_context* ctx, struct vm_region* vr,
                                  struct phys_region* pr, unsigned int flags)
{
    panic("page fault in anonymous contiguous mapping");
    return EFAULT;
}

static int anon_contig_reference(struct phys_region* pr,
                                 struct phys_region* new_pr)
{
    return ENOMEM;
}

static int anon_contig_unreference(struct phys_region* pr)
{
    return anon_map_ops.rop_unreference(pr);
}

static int anon_contig_writable(const struct phys_region* pr)
{
    return anon_map_ops.rop_writable(pr);
}

static void anon_contig_split(struct vm_context* ctx, struct vm_region* vr,
                              struct vm_region* r1, struct vm_region* r2)
{}

const struct region_operations anon_contig_map_ops = {
    .rop_new = anon_contig_new,
    .rop_pt_flags = anon_contig_pt_flags,
    .rop_resize = anon_contig_resize,
    .rop_split = anon_contig_split,
    .rop_page_fault = anon_contig_page_fault,

    .rop_writable = anon_contig_writable,
    .rop_reference = anon_contig_reference,
    .rop_unreference = anon_contig_unreference,
};
