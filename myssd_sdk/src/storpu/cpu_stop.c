#include <types.h>
#include <storpu/thread.h>
#include <cpulocals.h>
#include <storpu/cpu_stop.h>
#include <spinlock.h>
#include <utils.h>

struct cpu_stopper {
    struct thread* thread;

    spinlock_t lock;
    struct list_head works;
};

static DEFINE_CPULOCAL(struct cpu_stopper, cpu_stopper);

static unsigned long cpu_stopper_thread(unsigned long arg)
{
    unsigned int cpu = (unsigned int)arg;
    struct cpu_stopper* stopper = get_cpu_var_ptr(cpu, cpu_stopper);

    for (;;) {
        struct cpu_stop_work* work = NULL;

        for (;;) {
            set_current_state(THREAD_BLOCKED);

            spin_lock_irq(&stopper->lock);
            if (!list_empty(&stopper->works)) {
                work =
                    list_entry(stopper->works.next, struct cpu_stop_work, list);
                list_del(&work->list);
            }
            spin_unlock_irq(&stopper->lock);

            if (work) break;

            schedule();
        }
        __set_current_state(THREAD_RUNNING);

        (work->fn)(work->arg);
    }

    return 0;
}

void cpu_stop_init(void)
{
    int i;

    for (i = 0; i < NR_CPUS; i++) {
        struct cpu_stopper* stopper = get_cpu_var_ptr(i, cpu_stopper);

        INIT_LIST_HEAD(&stopper->works);
        spinlock_init(&stopper->lock);
    }
}

void cpu_stop_init_cpu(void)
{
    struct cpu_stopper* stopper = get_cpulocal_var_ptr(cpu_stopper);

    stopper->thread = thread_create(NULL, NULL, NULL, cpu_stopper_thread,
                                    (unsigned long)cpuid);
    if (!stopper->thread) panic("Failed to create migration thread");
}

static int cpu_stop_queue_work(unsigned int cpu, struct cpu_stop_work* work)
{
    struct cpu_stopper* stopper = get_cpu_var_ptr(cpu, cpu_stopper);
    unsigned long flags;

    spin_lock_irqsave(&stopper->lock, &flags);
    list_add_tail(&work->list, &stopper->works);
    wake_up_thread(stopper->thread);
    spin_unlock_irqrestore(&stopper->lock, flags);

    return TRUE;
}

int stop_one_cpu_nowait(unsigned int cpu, cpu_stop_fn_t fn, void* arg,
                        struct cpu_stop_work* work_buf)
{
    *work_buf = (struct cpu_stop_work){
        .fn = fn,
        .arg = arg,
    };
    return cpu_stop_queue_work(cpu, work_buf);
}
