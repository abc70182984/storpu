#ifndef _STORPU_CONDVAR_H_
#define _STORPU_CONDVAR_H_

#include <storpu/futex.h>

typedef struct {
    unsigned int state;
    struct futex futex;
} cond_t;
typedef int condattr_t;

int cond_init(cond_t* cond, const condattr_t* attr);
int cond_signal(cond_t* cond);
int cond_broadcast(cond_t* cond);
int cond_wait(cond_t* cond, mutex_t* mutex);

#endif
