#ifndef _STORPU_RWLOCK_H_
#define _STORPU_RWLOCK_H_

#include <storpu/mutex.h>

typedef struct {
    unsigned int state;

    mutex_t pending_mutex;
    unsigned int pending_reader_count;
    unsigned int pending_writer_count;

    struct futex pending_reader_wq;
    unsigned int pending_reader_serial;
    struct futex pending_writer_wq;
    unsigned int pending_writer_serial;
} rwlock_t;
typedef int rwlockattr_t;

int rwlock_init(rwlock_t* rwlock, const rwlockattr_t* attr);
int rwlock_rdlock(rwlock_t* rwlock);
int rwlock_wrlock(rwlock_t* rwlock);
int rwlock_unlock(rwlock_t* rwlock);

#endif
