#include <string.h>

#include <iov_iter.h>

int zdma_memcpy(void* dst, const void* src, size_t n)
{
    memcpy(dst, src, n);
    return 0;
}

ssize_t zdma_iter_copy_from(struct iov_iter* iter, void* buf, size_t bytes,
                            int sync_cache)
{
    return iov_iter_copy_from(iter, buf, bytes);
}

ssize_t zdma_iter_copy_to(struct iov_iter* iter, const void* buf, size_t bytes,
                          int sync_cache)
{
    return iov_iter_copy_to(iter, buf, bytes);
}
