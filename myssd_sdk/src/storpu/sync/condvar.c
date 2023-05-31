#include <limits.h>

#include <types.h>
#include <storpu/thread.h>
#include <storpu/condvar.h>

#define COND_INC_STEP 0x4

int cond_init(cond_t* cond, const condattr_t* attr)
{
    cond->state = 0;
    futex_init(&cond->futex);
    return 0;
}

static int cond_pulse(cond_t* cond, unsigned int count)
{
    __atomic_fetch_add(&cond->state, COND_INC_STEP, __ATOMIC_RELAXED);

    futex_wake(&cond->futex, count);
    return 0;
}

int cond_signal(cond_t* cond) { return cond_pulse(cond, 1); }

int cond_broadcast(cond_t* cond) { return cond_pulse(cond, UINT_MAX); }

int cond_wait(cond_t* cond, mutex_t* mutex)
{
    unsigned int old_state = __atomic_load_n(&cond->state, __ATOMIC_ACQUIRE);
    int r;

    mutex_unlock(mutex);

    r = futex_wait(&cond->futex, &cond->state, old_state);

    mutex_lock(mutex);

    return r;
}
