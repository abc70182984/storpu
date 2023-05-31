#include <storpu.h>
#include <errno.h>

extern int sys_mmap(void* addr, size_t length, int prot, int flags, int fd,
                    unsigned long offset, void** out_addr)
    __attribute__((weak));

extern int sys_munmap(void* addr, size_t length) __attribute__((weak));

extern int sys_msync(void* addr, size_t length, int flags)
    __attribute__((weak));

void* mmap(void* addr, size_t length, int prot, int flags, int fd,
           unsigned long offset)
{
    void* out_addr;
    int ret;

    ret = sys_mmap(addr, length, prot, flags, fd, offset, &out_addr);
    if (ret != 0) {
        errno = ret;
        return MAP_FAILED;
    }

    return out_addr;
}

int munmap(void* addr, size_t length)
{
    int ret;

    ret = sys_munmap(addr, length);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    return 0;
}

int msync(void* addr, size_t length, int flags)
{
    int ret;

    ret = sys_msync(addr, length, flags);
    if (ret != 0) {
        errno = ret;
        return -1;
    }

    return 0;
}
