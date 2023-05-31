#include <string.h>
#include <errno.h>
#include <xil_assert.h>
#include <xil_printf.h>

#include <types.h>
#include <storpu/vm.h>
#include <idr.h>
#include <stackframe.h>
#include <spinlock.h>
#include <cpulocals.h>
#include <slab.h>

#include "region.h"
#include "cache.h"

static struct idr context_idr;
static spinlock_t idr_lock;

static DEFINE_CPULOCAL(struct vm_context*, current_ctx) = NULL;

void vm_init(void)
{
    spinlock_init(&idr_lock);
    idr_init(&context_idr);

    page_cache_init();
}

struct vm_context* vm_find_get_context(unsigned int cid)
{
    struct vm_context* ctx;

    spin_lock(&idr_lock);

    ctx = (struct vm_context*)idr_find(&context_idr, (unsigned long)cid);
    if (ctx) {
        kref_get(&ctx->kref);
    }

    spin_unlock(&idr_lock);

    return ctx;
}

struct vm_context* vm_get_context(struct vm_context* ctx)
{
    kref_get(&ctx->kref);
    return ctx;
}

static void release_context(struct kref* kref)
{
    struct vm_context* ctx = list_entry(kref, struct vm_context, kref);
    struct vm_region *vr, *tmp;

    list_for_each_entry_safe(vr, tmp, &ctx->mem_regions, list)
    {
        list_del(&vr->list);
        pgd_free_range(&ctx->pgd, vr->vir_addr, vr->vir_addr + vr->length, 0UL,
                       0UL);
        region_free(vr);
    }

    INIT_LIST_HEAD(&ctx->mem_regions);
    region_init_avl(ctx);

    pgd_free(&ctx->pgd);

    SLABFREE(ctx);
}

void vm_put_context(struct vm_context* ctx)
{
    kref_put(&ctx->kref, release_context);
}

struct vm_context* vm_create_context(void)
{
    struct vm_context* ctx;
    int r;

    SLABALLOC(ctx);
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(*ctx));

    kref_init(&ctx->kref);
    spinlock_init(&ctx->pgd_lock);
    mutex_init(&ctx->mmap_lock, NULL);

    r = pgd_new(&ctx->pgd);
    if (r) goto out_free;

    INIT_LIST_HEAD(&ctx->mem_regions);
    region_init_avl(ctx);

    spin_lock(&idr_lock);
    ctx->cid = idr_alloc(&context_idr, ctx, 1, 0);
    spin_unlock(&idr_lock);
    if (ctx->cid < 0) goto out_free_pgd;

    return ctx;

out_free_pgd:
    pgd_free(&ctx->pgd);

out_free:
    SLABFREE(ctx);
    return NULL;
}

void vm_delete_context(struct vm_context* ctx)
{
    spin_lock(&idr_lock);
    idr_remove(&context_idr, ctx->cid);
    spin_unlock(&idr_lock);

    vm_put_context(ctx);
}

void vm_switch_context(struct vm_context* ctx)
{
    struct vm_context* old_ctx = get_cpulocal_var(current_ctx);

    if (old_ctx != ctx) {
        if (ctx) {
            get_cpulocal_var(current_ctx) = vm_get_context(ctx);
            switch_address_space(&ctx->pgd);
        } else {
            get_cpulocal_var(current_ctx) = NULL;
        }

        if (old_ctx) vm_put_context(old_ctx);
    }
}

static struct vm_region* mmap_region(struct vm_context* ctx, unsigned long addr,
                                     int mmap_flags, size_t len, int vrflags,
                                     const struct region_operations* rops)
{
    struct vm_region* vr = NULL;
    int mrflags = 0;

    if (mmap_flags & MAP_POPULATE) mrflags |= MRF_PREALLOC;

    if (len <= 0) return NULL;

    len = roundup(len, ARCH_PG_SIZE);

    /* first unmap the region */
    if (addr && (mmap_flags & MAP_FIXED)) {
        if (region_unmap_range(ctx, addr, len)) return NULL;
    }

    if (addr || (mmap_flags & MAP_FIXED)) {
        vr = region_map(ctx, addr, 0, len, vrflags, mrflags, rops);
        if (!vr && (mmap_flags & MAP_FIXED)) return NULL;
    }

    if (!vr) {
        vr = region_map(ctx, VM_USER_START, VM_STACK_TOP, len, vrflags, mrflags,
                        rops);
    }

    return vr;
}

static int mmap_file(struct vm_context* ctx, void* addr, size_t len, int prot,
                     int flags, int fd, unsigned long offset, void** ret_addr)
{
    struct vm_region* vr;
    unsigned long page_off;
    int vr_flags;

    vr_flags = region_get_prot_bits(prot);
    if (flags & MAP_SHARED) vr_flags |= RF_MAP_SHARED;

    if ((page_off = (offset % ARCH_PG_SIZE)) != 0) {
        offset -= page_off;
        len += page_off;
    }

    len = roundup(len, ARCH_PG_SIZE);

    Xil_AssertNonvoid(!(len % ARCH_PG_SIZE));
    Xil_AssertNonvoid(!(offset % ARCH_PG_SIZE));

    mutex_lock(&ctx->mmap_lock);
    vr = mmap_region(ctx, (unsigned long)addr, flags, len, vr_flags,
                     &file_map_ops);
    mutex_unlock(&ctx->mmap_lock);

    if (!vr) return ENOMEM;

    *ret_addr = (void*)(vr->vir_addr + page_off);

    return file_map_set_file(ctx, vr, fd, offset);
}

int vm_map(struct vm_context* ctx, void* addr, size_t len, int prot, int flags,
           int fd, unsigned long offset, void** out_addr)
{
    struct vm_region* vr;
    int vr_flags = 0;
    const struct region_operations* rops = NULL;

    if (len <= 0) return EINVAL;

    if ((flags & (MAP_PRIVATE | MAP_SHARED)) == 0 ||
        (flags & (MAP_PRIVATE | MAP_SHARED)) == (MAP_PRIVATE | MAP_SHARED))
        return EINVAL;

    if ((fd == -1) || (flags & MAP_ANONYMOUS)) {
        if (fd != -1) return EINVAL;

        if ((flags & (MAP_CONTIG | MAP_POPULATE)) == MAP_CONTIG) return EINVAL;

        vr_flags = region_get_prot_bits(prot);
        if (flags & MAP_SHARED) vr_flags |= RF_MAP_SHARED;

        vr_flags |= RF_ANON;

        if (flags & MAP_CONTIG)
            rops = &anon_contig_map_ops;
        else
            rops = &anon_map_ops;

        mutex_lock(&ctx->mmap_lock);
        vr = mmap_region(ctx, (unsigned long)addr, flags, len, vr_flags, rops);
        mutex_unlock(&ctx->mmap_lock);

        if (!vr) return ENOMEM;

        if (out_addr) *out_addr = (void*)vr->vir_addr;

        return 0;
    }

    return mmap_file(ctx, addr, len, prot, flags, fd, offset, out_addr);
}

int vm_unmap(struct vm_context* ctx, void* addr, size_t length)
{
    int r;

    length = roundup(length, ARCH_PG_SIZE);

    mutex_lock(&ctx->mmap_lock);
    r = region_unmap_range(ctx, (unsigned long)addr, length);
    mutex_unlock(&ctx->mmap_lock);

    return r;
}

int vm_handle_page_fault(unsigned long addr, unsigned int flags,
                         unsigned int vr_flags, struct stackframe* regs)
{
    struct vm_context* ctx = get_cpulocal_var(current_ctx);
    struct vm_region* vr;
    unsigned long offset;
    int ret;

    mutex_lock(&ctx->mmap_lock);

    vr = region_lookup(ctx, addr);

    if (!vr) {
        xil_printf("context %d bad address 0x%016lx, pc=0x%016lx\n", ctx->cid,
                   addr, regs->pc);
        mutex_unlock(&ctx->mmap_lock);
        return FALSE;
    }

    if (!(vr->flags & vr_flags)) {
        xil_printf("context %d bad access 0x%016lx, pc=0x%016lx\n", ctx->cid,
                   addr, regs->pc);
        mutex_unlock(&ctx->mmap_lock);
        return FALSE;
    }

    Xil_AssertNonvoid(addr >= vr->vir_addr);
    Xil_AssertNonvoid(addr < vr->vir_addr + vr->length);

    offset = addr - vr->vir_addr;

    ret = region_handle_pf(ctx, vr, offset, flags);

    mutex_unlock(&ctx->mmap_lock);

    return ret == 0;
}

/* Translate virtual address vector into physical addresses. */
int vm_vumap(struct vm_context* ctx, const struct vumap_vir* vvec,
             unsigned int vcount, size_t offset, int write,
             struct vumap_phys* pvec, unsigned int pmax)
{
    unsigned long vir_addr;
    phys_addr_t phys_addr;
    unsigned int pcount = 0;
    size_t size, chunk;
    int i, r;

    if (vcount == 0 || pmax == 0) return -EINVAL;

    spin_lock(&ctx->pgd_lock);

    for (i = 0; i < vcount && pcount < pmax; i++) {
        size = vvec[i].size;
        if (size <= offset) {
            r = -EINVAL;
            goto out;
        }
        size -= offset;

        vir_addr = vvec[i].addr + offset;

        while (size > 0 && pcount < pmax) {
            chunk = pgd_va2pa_range(&ctx->pgd, vir_addr, &phys_addr, size);

            if (!chunk) {
                r = -EFAULT;
                goto out;
            }

            pvec[pcount].addr = phys_addr;
            pvec[pcount].size = chunk;
            pcount++;

            vir_addr += chunk;
            size -= chunk;
        }

        offset = 0;
    }

    Xil_AssertNonvoid(pcount > 0);
    r = pcount;

out:
    spin_unlock(&ctx->pgd_lock);
    return r;
}

int sys_brk(void* addr)
{
    struct vm_context* ctx = get_cpulocal_var(current_ctx);
    int r;

    mutex_lock(&ctx->mmap_lock);
    r = region_extend_up_to(ctx, (unsigned long)addr);
    mutex_unlock(&ctx->mmap_lock);

    return r;
}

int sys_mmap(void* addr, size_t length, int prot, int flags, int fd,
             unsigned long offset, void** out_addr)
{
    struct vm_context* ctx = get_cpulocal_var(current_ctx);

    return vm_map(ctx, addr, length, prot, flags, fd, offset, out_addr);
}

int sys_munmap(void* addr, size_t length)
{
    struct vm_context* ctx = get_cpulocal_var(current_ctx);

    return vm_unmap(ctx, addr, length);
}

int sys_msync(void* addr, size_t len, int flags)
{
    struct vm_context* ctx = get_cpulocal_var(current_ctx);
    unsigned long start, end;
    struct vm_region* vr;
    int unmapped_error = 0;
    int error = EINVAL;

    start = (unsigned long)addr;

    if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) goto out;
    if (start % ARCH_PG_SIZE) goto out;
    if ((flags & MS_ASYNC) && (flags & MS_SYNC)) goto out;

    error = ENOMEM;
    len = roundup(len, ARCH_PG_SIZE);
    end = start + len;
    if (end < start) goto out;
    error = 0;
    if (end == start) goto out;

    mutex_lock(&ctx->mmap_lock);
    vr = region_lookup(ctx, start);
    for (;;) {
        unsigned long start_off, end_off;

        error = ENOMEM;
        if (!vr) goto out_unlock;

        if (start < vr->vir_addr) {
            if (flags == MS_ASYNC) goto out_unlock;
            start = vr->vir_addr;
            if (start >= end) goto out_unlock;
            unmapped_error = ENOMEM;
        }

        start_off = start - vr->vir_addr;
        end_off = min(end - vr->vir_addr, vr->length);

        start = vr->vir_addr + vr->length;
        if ((flags & MS_SYNC) && (vr->flags & RF_MAP_SHARED) &&
            vr->rops->rop_sync_range) {
            /* mutex_unlock(&ctx->mmap_lock); */
            error = vr->rops->rop_sync_range(vr, start_off, end_off);
            if (!error) {
                error = region_write_map_range(ctx, vr, start_off, end_off);
            }
            if (error || start >= end) goto out_unlock;
            /* mutex_lock(&ctx->mmap_lock); */
        } else {
            if (start >= end) {
                error = 0;
                goto out_unlock;
            }
        }
        vr = region_lookup(ctx, start);
    }
out_unlock:
    mutex_unlock(&ctx->mmap_lock);
out:
    return error ?: unmapped_error;
}
