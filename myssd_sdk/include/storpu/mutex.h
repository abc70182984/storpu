#ifndef _STORPU_MUTEX_H_
#define _STORPU_MUTEX_H_

#include <storpu/futex.h>

typedef struct {
    unsigned int state;
    struct futex futex;
} mutex_t;
typedef int mutexattr_t;

int mutex_init(mutex_t* mutex, const mutexattr_t* attr);
int mutex_trylock(mutex_t* mutex);
int mutex_lock(mutex_t* mutex);
int mutex_unlock(mutex_t* mutex);

#endif
