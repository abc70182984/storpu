#ifndef _STORPU_THREAD_H_
#define _STORPU_THREAD_H_

#include <stddef.h>

typedef unsigned int spu_thread_t;

typedef struct {
    unsigned long __state[4];
} spu_mutex_t;
typedef int spu_mutexattr_t;

typedef struct {
    unsigned long __state[4];
} spu_cond_t;
typedef int spu_condattr_t;

typedef struct {
    size_t stacksize;
    void* stackaddr;
} spu_thread_attr_t;

#ifdef __cplusplus
extern "C"
{
#endif

    spu_thread_t spu_thread_self(void);

    int spu_thread_create(spu_thread_t* tid, const spu_thread_attr_t* attr,
                          unsigned long (*proc)(unsigned long),
                          unsigned long arg) __attribute__((weak));
    int spu_thread_join(spu_thread_t tid, unsigned long* retval)
        __attribute__((weak));
    void spu_thread_exit(unsigned long result) __attribute__((weak, noreturn));

    int spu_mutex_init(spu_mutex_t* mutex, const spu_mutexattr_t* attr)
        __attribute__((weak));
    int spu_mutex_trylock(spu_mutex_t* mutex) __attribute__((weak));
    int spu_mutex_lock(spu_mutex_t* mutex) __attribute__((weak));
    int spu_mutex_unlock(spu_mutex_t* mutex) __attribute__((weak));

#ifdef __cplusplus
}
#endif

#endif
