#ifndef _STORPU_FUTEX_H_
#define _STORPU_FUTEX_H_

#include <spinlock.h>

struct futex {
    spinlock_t queue_lock;
    struct list_head wait_queue;
};

void futex_init(struct futex* futex);
int futex_wait(struct futex* futex, const unsigned int* uval,
               unsigned int old_val);
int futex_wake(struct futex* futex, unsigned int count);

#endif
