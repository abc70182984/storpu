#ifndef _STORPU_FILE_H_
#define _STORPU_FILE_H_

#include <stddef.h>

#define FD_HOST_MEM   (-2)
#define FD_SCRATCHPAD (-3)

#ifdef __cplusplus
extern "C"
{
#endif

    ssize_t spu_read(int fd, void* buf, size_t count, unsigned long offset)
        __attribute__((weak));
    ssize_t spu_write(int fd, const void* buf, size_t count,
                      unsigned long offset) __attribute__((weak));

#ifdef __cplusplus
}
#endif

#endif
