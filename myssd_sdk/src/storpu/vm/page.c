#include <xil_assert.h>
#include <string.h>

#include <types.h>
#include <const.h>
#include <storpu/vm.h>
#include <memalloc.h>
#include <slab.h>
#include <utils.h>

#include "region.h"

struct page* page_new(phys_addr_t phys)
{
    struct page* page;

    SLABALLOC(page);
    if (!page) return NULL;
    memset(page, 0, sizeof(*page));

    if (phys != PHYS_NONE) Xil_AssertNonvoid(!(phys % ARCH_PG_SIZE));

    page->phys_addr = phys;
    page->refcount = 0;
    INIT_LIST_HEAD(&page->regions);
    page->flags = 0;

    return page;
}

void page_free(struct page* page)
{
    if (page->phys_addr != PHYS_NONE) free_mem(page->phys_addr, ARCH_PG_SIZE);
    SLABFREE(page);
}

void page_link(struct phys_region* pr, struct page* page, unsigned long offset,
               struct vm_region* parent)
{
    pr->offset = offset;
    pr->page = page;
    pr->parent = parent;
    list_add(&pr->page_link, &page->regions);
    page->refcount++;
}

struct phys_region* page_reference(struct page* page, unsigned long offset,
                                   struct vm_region* vr,
                                   const struct region_operations* rops)
{
    struct phys_region* pr;
    SLABALLOC(pr);
    if (!pr) return NULL;
    memset(pr, 0, sizeof(*pr));

    pr->rops = rops;
    page_link(pr, page, offset, vr);

    phys_region_set(vr, offset, pr);

    return pr;
}

void page_unreference(struct vm_region* vr, struct phys_region* pr, int remove)
{
    struct page* page = pr->page;
    int retval;

    Xil_AssertVoid(page->refcount > 0);
    page->refcount--;

    Xil_AssertVoid(!list_empty(&pr->page_link));
    list_del(&pr->page_link);

    if (page->refcount == 0) {
        Xil_AssertVoid(list_empty(&page->regions));

        if ((retval = pr->rops->rop_unreference(pr)) != 0)
            panic("mm: rop_unreference failed");

        SLABFREE(page);
    }

    pr->page = NULL;

    if (remove) phys_region_set(vr, pr->offset, NULL);
}
