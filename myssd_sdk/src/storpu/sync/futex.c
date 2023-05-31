#include <errno.h>

#include <types.h>
#include <storpu/thread.h>
#include <storpu/futex.h>

void futex_init(struct futex* futex)
{
    spinlock_init(&futex->queue_lock);
    INIT_LIST_HEAD(&futex->wait_queue);
}

int futex_wait(struct futex* futex, const unsigned int* uval,
               unsigned int old_val)
{
    struct thread* thread = current_thread;

    spin_lock(&futex->queue_lock);
    if (__atomic_load_n(uval, __ATOMIC_RELAXED) != old_val) {
        spin_unlock(&futex->queue_lock);
        return 0;
    }

    thread->state = THREAD_BLOCKED;
    list_add_tail(&thread->wait_list, &futex->wait_queue);
    spin_unlock(&futex->queue_lock);
    schedule();

    return 0;
}

int futex_wake(struct futex* futex, unsigned int count)
{
    spin_lock(&futex->queue_lock);

    while (!list_empty(&futex->wait_queue) && count > 0) {
        struct thread* thread;

        thread = list_entry(futex->wait_queue.next, struct thread, wait_list);
        list_del(&thread->wait_list);
        wake_up_thread(thread);

        count--;
    }

    spin_unlock(&futex->queue_lock);

    return 0;
}
