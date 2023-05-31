#include <xil_assert.h>
#include <errno.h>
#include <string.h>

#include <types.h>
#include <storpu/vm.h>
#include <memalloc.h>

#include "region.h"

static int anon_shrink_low(struct vm_region* vr, size_t len) { return 0; }

static int anon_resize(struct vm_context* ctx, struct vm_region* vr, size_t len)
{
    Xil_AssertNonvoid(vr);
    if (vr->length >= len) return 0;

    Xil_AssertNonvoid(vr->flags & RF_ANON);
    Xil_AssertNonvoid(!(len % ARCH_PG_SIZE));

    vr->length = len;
    return 0;
}

static void anon_split(struct vm_context* ctx, struct vm_region* vr,
                       struct vm_region* r1, struct vm_region* r2)
{}

static int anon_page_fault(struct vm_context* ctx, struct vm_region* vr,
                           struct phys_region* pr, unsigned int flags)
{
    phys_addr_t new_paddr;
    Xil_AssertNonvoid(pr->page->refcount > 0);

    new_paddr = alloc_pages(1, ZONE_PS_DDR);
    if (!new_paddr) return ENOMEM;

    if (!(vr->flags & RF_UNINITIALIZED)) {
        memset(__va(new_paddr), 0, ARCH_PG_SIZE);
    }

    if (pr->page->phys_addr == PHYS_NONE) {
        pr->page->phys_addr = new_paddr;
        return 0;
    }

    if (pr->page->refcount < 2 || !(flags & FAULT_FLAG_WRITE)) {
        free_mem(new_paddr, ARCH_PG_SIZE);

        return 0;
    }

    /* return page_cow(vr, pr, new_paddr); */
    return EINVAL;
}

static int anon_writable(const struct phys_region* pr)
{
    Xil_AssertNonvoid(pr->page->refcount > 0);
    if (pr->page->phys_addr == PHYS_NONE) return 0;

    return pr->page->refcount == 1;
}

static int anon_unreference(struct phys_region* pr)
{
    Xil_AssertNonvoid(pr->page->refcount == 0);
    if (pr->page->phys_addr != PHYS_NONE)
        free_mem(pr->page->phys_addr, ARCH_PG_SIZE);
    return 0;
}

const struct region_operations anon_map_ops = {
    .rop_shrink_low = anon_shrink_low,
    .rop_resize = anon_resize,
    .rop_split = anon_split,
    .rop_page_fault = anon_page_fault,
    .rop_writable = anon_writable,
    .rop_unreference = anon_unreference,
};
