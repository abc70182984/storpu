#ifndef _IOV_ITER_H_
#define _IOV_ITER_H_

#include <stddef.h>

#ifdef __UM__

#include <sys/uio.h>

#else

struct iovec {
    void* iov_base;
    size_t iov_len;
};

#endif

struct iov_iter {
    size_t iov_offset;
    size_t count;
    const struct iovec* iov;
    size_t nr_segs;
};

void iov_iter_init(struct iov_iter* iter, const struct iovec* iov,
                   size_t nr_segs, size_t count);

/* Copy data from ITER to BUF */
size_t iov_iter_copy_from(struct iov_iter* iter, void* buf, size_t bytes);
/* Copy data from BUF to ITER */
size_t iov_iter_copy_to(struct iov_iter* iter, const void* buf, size_t bytes);

int iov_iter_get_bufaddr(struct iov_iter* iter, void** buf, size_t* bytes);
void iov_iter_consume(struct iov_iter* iter, size_t bytes);

#endif
