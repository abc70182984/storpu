#ifndef _STORPU_H_
#define _STORPU_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    void spu_printf(const char* fmt, ...) __attribute__((weak));

    int brk(void* addr);
    void* sbrk(ptrdiff_t nbytes);

/* Mapping protection */
#define PROT_NONE  0x00 /* no permissions */
#define PROT_READ  0x01 /* pages can be read */
#define PROT_WRITE 0x02 /* pages can be written */
#define PROT_EXEC  0x04 /* pages can be executed */

/*
 * Flags contain sharing type and options.
 * Sharing types; choose one.
 */
#define MAP_SHARED  0x0001 /* share changes */
#define MAP_PRIVATE 0x0002 /* changes are private */

/*
 * Mapping type
 */
#define MAP_ANONYMOUS 0x0004 /* anonymous memory */
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FIXED    0x0008
#define MAP_POPULATE 0x0010
#define MAP_CONTIG   0x0020

/*
 * Error indicator returned by mmap(2)
 */
#define MAP_FAILED ((void*)-1) /* mmap() failed */

    void* mmap(void* addr, size_t length, int prot, int flags, int fd,
               unsigned long offset);
    int munmap(void* addr, size_t length);

    /* Flags to `msync'.  */
#define MS_ASYNC      1 /* Sync memory asynchronously.  */
#define MS_SYNC       4 /* Synchronous memory sync.  */
#define MS_INVALIDATE 2 /* Invalidate the caches.  */

    int msync(void* addr, size_t len, int flags);

    int fsync(int fd);
    int fdatasync(int fd);
    void sync(void);

#ifdef __cplusplus
}
#endif

#endif
