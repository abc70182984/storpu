#include <libcoro.h>
#include <errno.h>

#include "coro_internal.h"
#include <intr.h>

int coro_cond_init(coro_cond_t* cond, coro_condattr_t* attr)
{
    if (cond == NULL) {
        return EINVAL;
    }

    coro_queue_init(&cond->wait_queue);
    cond->tag = 0;

    if (attr) {
        cond->tag = attr->tag;
    }

    return 0;
}

int coro_cond_signal(coro_cond_t* cond)
{
    coro_thread_t thread;
    unsigned long flags;

    if (cond == NULL) {
        return EINVAL;
    }

    local_irq_save(&flags);
    thread = coro_queue_dequeue(&cond->wait_queue);
    if (thread != NO_THREAD) coro_unsuspend(thread);
    local_irq_restore(flags);

    return 0;
}

int coro_cond_broadcast(coro_cond_t* cond)
{
    coro_thread_t thread;
    unsigned long flags;

    if (cond == NULL) {
        return EINVAL;
    }

    local_irq_save(&flags);
    while (!coro_queue_empty(&cond->wait_queue)) {
        thread = coro_queue_dequeue(&cond->wait_queue);
        if (thread != NO_THREAD) coro_unsuspend(thread);
    }
    local_irq_restore(flags);

    return 0;
}

int coro_cond_wait(coro_cond_t* cond, coro_mutex_t* mutex)
{
    int retval;

    if (cond == NULL || mutex == NULL) {
        return EINVAL;
    }

    if ((retval = coro_mutex_unlock(mutex)) != 0) {
        return retval;
    }

    coro_queue_enqueue(&cond->wait_queue, current_thread);
    coro_suspend(CR_BLOCKED);

    if ((retval = coro_mutex_lock(mutex)) != 0) {
        return retval;
    }

    return 0;
}
