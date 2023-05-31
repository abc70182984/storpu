#include <errno.h>
#include <string.h>
#include <limits.h>

#include <types.h>
#include <storpu/thread.h>
#include <storpu/rwlock.h>

#define RWS_PD_WRITERS (1 << 0)
#define RWS_PD_READERS (1 << 1)
#define RCNT_SHIFT     2
#define RCNT_INC_STEP  (1 << RCNT_SHIFT)
#define RWS_WRLOCKED   (1 << 31)

#define RW_RDLOCKED(l) ((l) >= RCNT_INC_STEP)
#define RW_WRLOCKED(l) ((l)&RWS_WRLOCKED)

int rwlock_init(rwlock_t* rwlock, const rwlockattr_t* attr)
{
    memset(rwlock, 0, sizeof(*rwlock));
    mutex_init(&rwlock->pending_mutex, NULL);
    return 0;
}

static int __can_rdlock(unsigned int state) { return !RW_WRLOCKED(state); }

static int rwlock_tryrdlock(rwlock_t* lock)
{
    unsigned int old_state = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);

    while (__can_rdlock(old_state)) {
        unsigned int new_state = old_state + RCNT_INC_STEP;
        if (!RW_RDLOCKED(new_state)) return EAGAIN;

        if (__atomic_compare_exchange_n(&lock->state, &old_state, new_state, 1,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
    }
    return EBUSY;
}

int rwlock_rdlock(rwlock_t* lock)
{
    int r;

    if (rwlock_tryrdlock(lock) == 0) return 0;

    while (TRUE) {
        unsigned int old_state;
        unsigned int old_serial;

        r = rwlock_tryrdlock(lock);
        if (r == 0 || r == EAGAIN) return r;

        old_state = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);
        if (__can_rdlock(old_state)) continue;

        mutex_lock(&lock->pending_mutex);
        lock->pending_reader_count++;

        old_state =
            __atomic_fetch_or(&lock->state, RWS_PD_READERS, __ATOMIC_RELAXED);
        old_serial =
            __atomic_load_n(&lock->pending_reader_serial, __ATOMIC_RELAXED);
        mutex_unlock(&lock->pending_mutex);

        r = 0;
        if (!__can_rdlock(old_state))
            r = futex_wait(&lock->pending_reader_wq,
                           &lock->pending_reader_serial, old_serial);

        mutex_lock(&lock->pending_mutex);
        lock->pending_reader_count--;
        if (lock->pending_reader_count == 0) {
            __atomic_fetch_and(&lock->state, ~RWS_PD_READERS, __ATOMIC_RELAXED);
        }
        mutex_unlock(&lock->pending_mutex);
        if (r) return r;
    }

    return 0;
}

static int __can_wrlock(int state)
{
    return !(RW_WRLOCKED(state) && RW_RDLOCKED(state));
}

static int rwlock_trywrlock(rwlock_t* lock)
{
    unsigned int old_state = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);

    while (__can_wrlock(old_state)) {
        if (__atomic_compare_exchange_n(&lock->state, &old_state,
                                        old_state | RWS_WRLOCKED, 1,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return 0;
        }
    }
    return EBUSY;
}

int rwlock_wrlock(rwlock_t* lock)
{
    int r;

    if (rwlock_trywrlock(lock) == 0) return 0;

    while (TRUE) {
        unsigned int old_state;
        unsigned int old_serial;

        r = rwlock_trywrlock(lock);
        if (r == 0) return r;

        old_state = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);
        if (__can_wrlock(old_state)) continue;

        mutex_lock(&lock->pending_mutex);
        lock->pending_writer_count++;

        old_state =
            __atomic_fetch_or(&lock->state, RWS_PD_WRITERS, __ATOMIC_RELAXED);
        old_serial =
            __atomic_load_n(&lock->pending_writer_serial, __ATOMIC_RELAXED);
        mutex_unlock(&lock->pending_mutex);

        r = 0;
        if (!__can_wrlock(old_state))
            r = futex_wait(&lock->pending_writer_wq,
                           &lock->pending_writer_serial, old_serial);

        mutex_lock(&lock->pending_mutex);
        lock->pending_writer_count--;
        if (lock->pending_writer_count == 0) {
            __atomic_fetch_and(&lock->state, ~RWS_PD_WRITERS, __ATOMIC_RELAXED);
        }
        mutex_unlock(&lock->pending_mutex);
        if (r) return r;
    }

    return 0;
}

int rwlock_unlock(rwlock_t* lock)
{
    unsigned int old_state = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);

    if (RW_WRLOCKED(old_state)) {
        old_state =
            __atomic_fetch_and(&lock->state, ~RWS_WRLOCKED, __ATOMIC_RELEASE);

        if (!(old_state & RWS_PD_WRITERS) && !(old_state & RWS_PD_READERS))
            return 0;
    } else if (RW_RDLOCKED(old_state)) {
        old_state =
            __atomic_fetch_sub(&lock->state, RCNT_INC_STEP, __ATOMIC_RELEASE);

        if ((old_state >> RCNT_SHIFT) != 1 ||
            !(old_state & RWS_PD_WRITERS && old_state & RWS_PD_READERS))
            return 0;
    } else
        return EPERM;

    mutex_lock(&lock->pending_mutex);
    if (lock->pending_writer_count) {
        lock->pending_writer_serial++;
        mutex_unlock(&lock->pending_mutex);

        futex_wake(&lock->pending_writer_wq, 1);
    } else if (lock->pending_reader_count) {
        lock->pending_reader_serial++;
        mutex_unlock(&lock->pending_mutex);

        futex_wake(&lock->pending_reader_wq, UINT_MAX);
    } else {
        mutex_unlock(&lock->pending_mutex);
    }

    return 0;
}
