#include <types.h>
#include <storpu/vm.h>
#include <storpu/thread.h>
#include <storpu/file.h>
#include <utils.h>

/* clang-format off */
#define _SYM_LIST                                               \
    _SYM("spu_printf", printk),                                 \
        _SYM("sys_brk", sys_brk),                               \
        _SYM("sys_mmap", sys_mmap),                             \
        _SYM("sys_munmap", sys_munmap),                         \
        _SYM("sys_msync", sys_msync),                           \
        _SYM("spu_thread_self", sys_thread_self),               \
        _SYM("spu_thread_create", sys_thread_create),           \
        _SYM("spu_thread_join", sys_thread_join),               \
        _SYM("spu_thread_exit", sys_thread_exit),               \
        _SYM("spu_sched_setaffinity", sys_sched_setaffinity),   \
        _SYM("spu_mutex_init", mutex_init),                     \
        _SYM("spu_mutex_trylock", mutex_trylock),               \
        _SYM("spu_mutex_lock", mutex_lock),                     \
        _SYM("spu_mutex_unlock", mutex_unlock),                 \
        _SYM("spu_read", spu_read),                             \
        _SYM("spu_write", spu_write),                           \
        _SYM("sys_fsync", sys_fsync),                           \
        _SYM("sys_fdatasync", sys_fdatasync),                   \
        _SYM("sys_sync", sys_sync),
/* clang-format on */
