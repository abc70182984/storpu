#include <string.h>

#include <config.h>
#include <types.h>
#include <storpu/thread.h>
#include <storpu/vm.h>
#include <sysreg.h>
#include <cpulocals.h>
#include <spinlock.h>
#include <barrier.h>
#include <storpu/cpu_stop.h>
#include <storpu/completion.h>
#include <smp.h>
#include <utils.h>

struct rq {
    unsigned int cpu;

    spinlock_t lock;
    struct list_head queue;
};

static DEFINE_CPULOCAL(struct rq, run_queue);
static DEFINE_CPULOCAL(struct thread*, prev_thread);

#define thread_rq(thread) (get_cpu_var_ptr((thread)->cpu, run_queue))

static void rq_init(struct rq* rq, unsigned int cpu)
{
    rq->cpu = cpu;
    spinlock_init(&rq->lock);
    INIT_LIST_HEAD(&rq->queue);
}

static inline void rq_lock(struct rq* rq) { spin_lock(&rq->lock); }

static inline void rq_unlock(struct rq* rq) { spin_unlock(&rq->lock); }

void sched_init(void)
{
    int i;

    for (i = 0; i < NR_CPUS; i++) {
        rq_init(get_cpu_var_ptr(i, run_queue), i);
    }
}

static inline void enqueue_thread(struct rq* rq, struct thread* thread)
{
    list_add_tail(&thread->queue, &rq->queue);
}

static inline void dequeue_thread(struct rq* rq, struct thread* thread)
{
    list_del(&thread->queue);
}

static inline void activate_thread(struct rq* rq, struct thread* thread)
{
    thread->on_rq = THREAD_ON_RQ_QUEUED;
    enqueue_thread(rq, thread);
}

static inline void deactivate_thread(struct rq* rq, struct thread* thread,
                                     int sleep)
{
    thread->on_rq = sleep ? 0 : THREAD_ON_RQ_MIGRATING;
    dequeue_thread(rq, thread);
}

static inline void prepare_thread(struct thread* next)
{
    __atomic_store_n(&next->on_cpu, 1, __ATOMIC_RELAXED);
}

static inline void finish_thread(struct thread* prev)
{
    __atomic_store_n(&prev->on_cpu, 0, __ATOMIC_RELEASE);
}

static struct rq* __thread_rq_lock(struct thread* thread, unsigned long* flags)
{
    struct rq* rq;

    for (;;) {
        rq = thread_rq(thread);
        rq_lock(rq);
        if (rq == thread_rq(thread) && !thread_on_rq_migrating(thread))
            return rq;
        rq_unlock(rq);

        while (thread_on_rq_migrating(thread))
            isb();
    }

    /* Unreachable */
    return NULL;
}

static struct rq* thread_rq_lock(struct thread* thread, unsigned long* flags)
{
    struct rq* rq;

    for (;;) {
        spin_lock_irqsave(&thread->pi_lock, flags);
        rq = thread_rq(thread);
        rq_lock(rq);
        if (rq == thread_rq(thread) && !thread_on_rq_migrating(thread))
            return rq;
        rq_unlock(rq);
        spin_unlock_irqrestore(&thread->pi_lock, *flags);

        while (thread_on_rq_migrating(thread))
            isb();
    }

    /* Unreachable */
    return NULL;
}

static inline void __thread_rq_unlock(struct rq* rq, unsigned long flags)
{
    spin_unlock(&rq->lock);
}

static inline void thread_rq_unlock(struct rq* rq, struct thread* thread,
                                    unsigned long flags)
{
    spin_unlock(&rq->lock);
    spin_unlock_irqrestore(&thread->pi_lock, flags);
}

static struct rq* finish_thread_switch(struct thread* prev)
{
    struct rq* rq = get_cpulocal_var_ptr(run_queue);

    finish_thread(prev);
    rq_unlock(rq);
    local_irq_enable();

    if (prev->state == THREAD_REAPABLE) thread_reap(prev);

    return rq;
}

static struct rq* context_switch(struct thread* prev, struct thread* next)
{
    prepare_thread(next);

    if (next->vm_context && prev->vm_context != next->vm_context)
        vm_switch_context(next->vm_context);

    write_sysreg((unsigned long)next, sp_el0);
    write_sysreg((unsigned long)next->tls_tcb, tpidr_el0);

    get_cpulocal_var(prev_thread) = prev;

    if (swapcontext(&prev->ucontext, &next->ucontext) == -1) {
        panic("failed to swap context");
    }

    prev = get_cpulocal_var(prev_thread);

    return finish_thread_switch(prev);
}

static struct thread* pick_next_thread(struct rq* rq)
{
    struct thread* next;

    if (list_empty(&rq->queue)) return NULL;

    next = list_entry(rq->queue.next, struct thread, queue);
    list_del(&next->queue);
    /* Add back for RR */
    list_add_tail(&next->queue, &rq->queue);

    return next;
}

void schedule(void)
{
    struct thread *prev, *next;
    struct thread* idle = get_cpulocal_var_ptr(idle_thread);
    struct rq* rq = get_cpulocal_var_ptr(run_queue);
    unsigned int prev_state;

    prev = current_thread;

    local_irq_disable();
    rq_lock(rq);

    prev_state = __atomic_load_n(&prev->state, __ATOMIC_RELAXED);
    if (prev_state != THREAD_RUNNING) deactivate_thread(rq, prev, TRUE);

    next = pick_next_thread(rq);
    if (!next) {
        next = idle;
    }

    if (prev != next) {
        context_switch(prev, next);
    } else {
        rq_unlock(rq);
        local_irq_enable();
    }
}

void schedule_tail(void)
{
    struct thread* prev = get_cpulocal_var(prev_thread);
    finish_thread_switch(prev);
}

static void resched_curr(struct rq* rq)
{
    int cpu;

    cpu = rq->cpu;
    if (cpu != cpuid) smp_send_reschedule(cpu);
}

void wake_up_new_thread(struct thread* thread)
{
    struct rq* rq;
    unsigned long flags;

    spin_lock_irqsave(&thread->pi_lock, &flags);
    __atomic_store_n(&thread->state, THREAD_RUNNING, __ATOMIC_RELAXED);

    rq = __thread_rq_lock(thread, &flags);

    activate_thread(rq, thread);

    thread_rq_unlock(rq, thread, flags);
}

static int ttwu_runnable(struct thread* thread)
{
    unsigned long flags;
    struct rq* rq;
    int r = 0;

    rq = __thread_rq_lock(thread, &flags);
    if (thread_on_rq_queued(thread)) {
        __atomic_store_n(&thread->state, THREAD_RUNNING, __ATOMIC_RELAXED);
        r = 1;
    }
    __thread_rq_unlock(rq, flags);

    return r;
}

static void ttwu_queue(struct thread* thread, int cpu)
{
    struct rq* rq = get_cpu_var_ptr(cpu, run_queue);

    rq_lock(rq);
    activate_thread(rq, thread);

    resched_curr(rq);
    __atomic_store_n(&thread->state, THREAD_RUNNING, __ATOMIC_RELAXED);
    rq_unlock(rq);
}

static inline int select_thread_rq(struct thread* p, int cpu) { return cpu; }

static int try_to_wake_up(struct thread* thread, unsigned int state)
{
    int cpu;
    int success = FALSE;

    if (thread == current_thread) {
        if (!(__atomic_load_n(&thread->state, __ATOMIC_RELAXED) & state))
            return FALSE;

        thread->state = THREAD_RUNNING;
        return TRUE;
    }

    spin_lock(&thread->pi_lock);
    if (!(__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) & state))
        goto unlock;

    success = TRUE;

    if (__atomic_load_n(&thread->on_rq, __ATOMIC_ACQUIRE) &&
        ttwu_runnable(thread))
        goto unlock;

    smp_rmb();
    __atomic_store_n(&thread->state, THREAD_WAKING, __ATOMIC_RELAXED);

    while (thread->on_cpu)
        isb();
    smp_rmb();

    cpu = select_thread_rq(thread, thread->wake_cpu);
    if (thread_cpu(thread) != cpu) {
        __set_thread_cpu(thread, cpu);
    }

    ttwu_queue(thread, cpu);

unlock:
    spin_unlock(&thread->pi_lock);

    return success;
}

int wake_up_thread(struct thread* thread)
{
    return try_to_wake_up(thread, THREAD_BLOCKED);
}

struct set_affinity_task;

struct migration {
    struct thread* thread;
    int dest_cpu;
    struct set_affinity_task* pending;
};

struct set_affinity_task {
    int stop_pending;
    struct completion done;
    struct migration arg;
    struct cpu_stop_work stop_work;
};

static struct rq* move_queued_thread(struct rq* rq, struct thread* thread,
                                     unsigned long* flags, int new_cpu)
{
    deactivate_thread(rq, thread, FALSE);
    __set_thread_cpu(thread, new_cpu);
    rq_unlock(rq);

    rq = get_cpu_var_ptr(new_cpu, run_queue);

    rq_lock(rq);
    activate_thread(rq, thread);
    resched_curr(rq);

    return rq;
}

static int migration_cpu_stop(void* data)
{
    struct migration* arg = data;
    struct set_affinity_task* pending = arg->pending;
    struct thread* thread = arg->thread;
    struct rq* rq = get_cpulocal_var_ptr(run_queue);
    unsigned long flags;
    int complete = FALSE;

    local_irq_save(&flags);

    spin_lock(&thread->pi_lock);
    rq_lock(rq);

    if (thread_rq(thread) == rq) {
        if (pending) {
            thread->migration_pending = NULL;
            complete = TRUE;

            if (cpumask_test_cpu(&thread->cpus_mask, thread_cpu(thread)))
                goto out;
        }

        if (thread_on_rq_queued(thread)) {
            rq = move_queued_thread(rq, thread, &flags, arg->dest_cpu);
        } else
            thread->wake_cpu = arg->dest_cpu;
    } else if (pending) {
        if (cpumask_test_cpu(thread->cpus_ptr, thread_cpu(thread))) {
            thread->migration_pending = NULL;
            complete = TRUE;
            goto out;
        }

        thread_rq_unlock(rq, thread, flags);
        stop_one_cpu_nowait(thread_cpu(thread), migration_cpu_stop,
                            &pending->arg, &pending->stop_work);
        return 0;
    }

out:
    if (pending) pending->stop_pending = FALSE;
    thread_rq_unlock(rq, thread, flags);

    if (complete) complete_all(&pending->done);

    return 0;
}

static int affine_move_task(struct rq* rq, struct thread* thread,
                            unsigned long flags, int dest_cpu)
{
    struct set_affinity_task my_pending = {}, *pending = NULL;
    int stop_pending, complete = FALSE;

    if (cpumask_test_cpu(&thread->cpus_mask, thread_cpu(thread))) {

        pending = thread->migration_pending;
        if (pending && !pending->stop_pending) {
            thread->migration_pending = NULL;
            complete = TRUE;
        }

        thread_rq_unlock(rq, thread, flags);

        if (complete) complete_all(&pending->done);

        return 0;
    }

    if (!thread->migration_pending) {
        init_completion(&my_pending.done);
        my_pending.arg.thread = thread;
        my_pending.arg.dest_cpu = dest_cpu;
        my_pending.arg.pending = &my_pending;

        thread->migration_pending = &my_pending;
    } else {
        pending = thread->migration_pending;
        pending->arg.dest_cpu = dest_cpu;
    }
    pending = thread->migration_pending;

    if (thread->on_cpu ||
        __atomic_load_n(&thread->state, __ATOMIC_RELAXED) == THREAD_WAKING) {

        stop_pending = pending->stop_pending;
        if (!stop_pending) pending->stop_pending = TRUE;

        thread_rq_unlock(rq, thread, flags);

        if (!stop_pending) {
            stop_one_cpu_nowait(rq->cpu, migration_cpu_stop, &pending->arg,
                                &pending->stop_work);
        }
    } else {
        if (thread_on_rq_queued(thread))
            rq = move_queued_thread(rq, thread, &flags, dest_cpu);

        if (pending && !pending->stop_pending) {
            thread->migration_pending = NULL;
            complete = TRUE;
        }

        thread_rq_unlock(rq, thread, flags);

        if (complete) complete_all(&pending->done);
    }

    wait_for_completion(&pending->done);

    return 0;
}

static int set_cpus_allowed(struct thread* thread,
                            const struct cpumask* new_mask)
{
    struct rq* rq;
    unsigned long flags;
    unsigned int dest_cpu;
    int ret = 0;

    rq = thread_rq_lock(thread, &flags);

    if (cpumask_equal(&thread->cpus_mask, new_mask)) goto out;

    dest_cpu = cpumask_any(new_mask);

    cpumask_copy(&thread->cpus_mask, new_mask);

    return affine_move_task(rq, thread, flags, dest_cpu);

out:
    thread_rq_unlock(rq, thread, flags);
    return ret;
}

int sched_setaffinity(struct thread* thread, const struct cpumask* in_mask)
{
    struct cpumask new_mask;
    int r;

    cpumask_and(&new_mask, in_mask, cpu_online_mask);

    r = set_cpus_allowed(thread, &new_mask);
    return r;
}
