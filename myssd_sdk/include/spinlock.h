#ifndef _SPINLOCK_H_
#define _SPINLOCK_H_

#include <stdint.h>
#include <intr.h>

typedef struct spinlock {
    volatile uint32_t lock;
} spinlock_t;

#define DEFINE_SPINLOCK(x) spinlock_t x = {0}

static inline void spinlock_init(spinlock_t* lock) { lock->lock = 0; }

static inline void spin_lock(spinlock_t* lock)
{
    while (__sync_lock_test_and_set(&lock->lock, 1)) {
#ifdef __aarch64__
        asm volatile("yield" ::: "memory");
#endif
    }

#if 0
    sevl();
    wfe();
#endif
}

static inline void spin_unlock(spinlock_t* lock)
{
    __sync_lock_release(&lock->lock);
}

static inline void spin_lock_irq(spinlock_t* lock)
{
    local_irq_disable();
    spin_lock(lock);
}

static inline void spin_unlock_irq(spinlock_t* lock)
{
    spin_unlock(lock);
    local_irq_enable();
}

static inline void spin_lock_irqsave(spinlock_t* lock, unsigned long* flagsp)
{
    local_irq_save(flagsp);
    spin_lock(lock);
}

static inline void spin_unlock_irqrestore(spinlock_t* lock, unsigned long flags)
{
    spin_unlock(lock);
    local_irq_restore(flags);
}

#endif
