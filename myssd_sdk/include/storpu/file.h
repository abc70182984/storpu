#ifndef _STORPU_FILE_H_
#define _STORPU_FILE_H_

#include <sys/types.h>

#define FD_HOST_MEM   (-2)
#define FD_SCRATCHPAD (-3)

ssize_t spu_read(int fd, void* buf, size_t count, unsigned long offset);
ssize_t spu_write(int fd, const void* buf, size_t count, unsigned long offset);

int sys_fsync(int fd);
int sys_fdatasync(int fd);

void sys_sync(void);

#endif
