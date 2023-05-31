#include <string.h>

#include <types.h>
#include <const.h>
#include <storpu/vm.h>
#include <storpu/thread.h>

#include "ldso.h"

#define LDSO_STATIC_TLS_RESERVATION 64

int ldso_tls_allocate_offset(struct so_info* si)
{
    size_t offset, next_offset;

    if (si->tls_done) return 0;
    if (si->tls_size == 0) {
        si->tls_offset = 0;
        si->tls_done = 1;
        return 0;
    }

    offset = si->ctx->tls_static_offset;
    if (offset % si->tls_align)
        offset += si->tls_align - (offset % si->tls_align);
    next_offset = offset + si->tls_size;

    si->tls_offset = offset;
    si->ctx->tls_static_offset = next_offset;
    si->tls_done = 1;

    return 0;
}

void ldso_tls_initial_allocation(struct vm_context* ctx)
{
    if (ctx->tls_static_offset == 0) {
        ctx->tls_static_space = 0;
        return;
    }

    ctx->tls_static_space =
        ctx->tls_static_offset + LDSO_STATIC_TLS_RESERVATION;

    if (ctx->tls_static_space % sizeof(void*))
        ctx->tls_static_space +=
            sizeof(void*) - (ctx->tls_static_space % sizeof(void*));
}

int ldso_allocate_tls(struct vm_context* ctx, struct tls_tcb** tcbp)
{
    struct so_info* si;
    struct tls_tcb* tcb;
    size_t dtv_size;
    void *p, *q;
    int r;

    if (ctx->tls_static_space == 0) {
        *tcbp = NULL;
        return 0;
    }

    dtv_size = sizeof(*tcb->tcb_dtv) * (2 + ctx->tls_max_index);

    r = vm_map(ctx, NULL,
               ctx->tls_static_space + sizeof(struct tls_tcb) + dtv_size,
               PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, &p);
    if (r) return r;

    tcb = (struct tls_tcb*)p;
    p += sizeof(struct tls_tcb);
    tcb->tcb_dtv = (void**)(p + ctx->tls_static_space);
    ++tcb->tcb_dtv;

    tcb->tcb_dtv[0] = (void*)(unsigned long)ctx->tls_max_index;

    si = ctx->so_info;
    if (si->tls_done) {
        q = p + si->tls_offset;

        if (si->tls_init_size) memcpy(q, si->tls_init, si->tls_init_size);
        tcb->tcb_dtv[si->tls_index] = q;
    }

    *tcbp = tcb;
    return 0;
}
