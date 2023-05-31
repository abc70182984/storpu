#ifndef _THREAD_H_
#define _THREAD_H_

#include <stdint.h>
#include <libcoro.h>

#include <intr.h>

#define thread_t    coro_thread_t
#define mutex_t     coro_mutex_t
#define mutexattr_t coro_mutexattr_t
#define cond_t      coro_cond_t
#define rwlock_t    coro_rwlock_t

#define mutex_init    coro_mutex_init
#define mutex_trylock coro_mutex_trylock
#define _mutex_lock   coro_mutex_lock
#define mutex_unlock  coro_mutex_unlock

#define cond_init      coro_cond_init
#define _cond_wait     coro_cond_wait
#define cond_signal    coro_cond_signal
#define cond_broadcast coro_cond_broadcast

#define rwlock_init   coro_rwlock_init
#define rwlock_rdlock coro_rwlock_rdlock
#define rwlock_wrlock coro_rwlock_wrlock
#define rwlock_unlock coro_rwlock_unlock

struct worker_thread {
    thread_t tid;
    mutex_t event_mutex;
    cond_t event;
    int err_code;
    int blocked_on;
    unsigned int lock_tag;
    void* cur_request;
    unsigned long intr_flags;

    uint64_t wait_start;
    uint64_t timeout_ns;
    int timed_out;

    int pending_rqs;
    int pending_rcs;
    int rq_error;
    int rc_error;

    void* opaque;
};

/* Reasons of blocking */
#define WT_BLOCKED_ON_NONE     0
#define WT_BLOCKED_ON_LOCK     1
#define WT_BLOCKED_ON_PCIE_CPL 2
#define WT_BLOCKED_ON_NVME_SQ  3
#define WT_BLOCKED_ON_FIL      4
#define WT_BLOCKED_ON_COND     5
#define WT_BLOCKED_ON_ECC      6
#define WT_BLOCKED_ON_PCIE_RQ  7
#define WT_BLOCKED_ON_STORPU   8
#define WT_BLOCKED_ON_ZDMA     9
#define WT_BLOCKED_ON_FLUSH    10

/* Lock tags */
#define LKT_NONE        -1
#define LKT_UNSPECIFIED 0
#define LKT_DATA_CACHE  1
#define LKT_AMU         2
#define LKT_NVME_PCIE   3

struct worker_thread* worker_self(void);
void worker_init(void (*init_func)(void));
void worker_yield(void);
void worker_wait(int why);
int worker_wait_timeout(int why, uint32_t timeout_ms);
void worker_wake(struct worker_thread* worker, int why);
struct worker_thread* worker_suspend(int why);
void worker_resume(struct worker_thread* worker);
struct worker_thread* worker_get(thread_t tid);

void worker_check_timeout(void);

static inline int mutex_lock(mutex_t* mutex)
{
    struct worker_thread* old_self = worker_suspend(WT_BLOCKED_ON_LOCK);
    int r;

    old_self->lock_tag = mutex->tag;
    r = _mutex_lock(mutex);
    old_self->lock_tag = LKT_NONE;
    worker_resume(old_self);

    return r;
}

static inline int cond_wait(cond_t* cond, mutex_t* mutex)
{
    struct worker_thread* old_self = worker_suspend(WT_BLOCKED_ON_COND);

    old_self->lock_tag = cond->tag;
    int r = _cond_wait(cond, mutex);
    old_self->lock_tag = LKT_NONE;
    worker_resume(old_self);

    return r;
}

#endif
