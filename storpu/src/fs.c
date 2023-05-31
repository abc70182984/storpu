#include <storpu.h>
#include <errno.h>

extern int sys_fsync(int fd) __attribute__((weak));

extern int sys_fdatasync(int fd) __attribute__((weak));

extern void sys_sync(void) __attribute__((weak));

int fsync(int fd)
{
    int ret;

    ret = sys_fsync(fd);
    if (ret) {
        errno = ret;
        return -1;
    }

    return 0;
}

int fdatasync(int fd)
{
    int ret;

    ret = sys_fdatasync(fd);
    if (ret) {
        errno = ret;
        return -1;
    }

    return 0;
}

void sync(void) { sys_sync(); }
