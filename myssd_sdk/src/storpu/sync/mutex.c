#include <errno.h>

#include <types.h>
#include <storpu/thread.h>
#include <storpu/mutex.h>

#define UNLOCKED           0
#define LOCKED_UNCONTENDED 1
#define LOCKED_CONTENDED   2

int mutex_init(mutex_t* mutex, const mutexattr_t* attr)
{
    mutex->state = UNLOCKED;
    futex_init(&mutex->futex);

    return 0;
}

int mutex_trylock(mutex_t* mutex)
{
    if (__sync_val_compare_and_swap(&mutex->state, UNLOCKED,
                                    LOCKED_UNCONTENDED) == UNLOCKED)
        return 0;

    return EBUSY;
}

int mutex_lock(mutex_t* mutex)
{
    if (mutex_trylock(mutex) == 0) return 0;

    while (__atomic_exchange_n(&mutex->state, LOCKED_CONTENDED,
                               __ATOMIC_ACQUIRE) != UNLOCKED) {
        futex_wait(&mutex->futex, &mutex->state, LOCKED_CONTENDED);
    }

    return 0;
}

int mutex_unlock(mutex_t* mutex)
{
    if (__atomic_exchange_n(&mutex->state, UNLOCKED, __ATOMIC_RELEASE) ==
        LOCKED_CONTENDED) {
        futex_wake(&mutex->futex, 1);
    }

    return 0;
}
