#ifndef _THREAD_H_
#define _THREAD_H_

#include <ucontext.h>
#include <list.h>
#include <spinlock.h>
#include <cpulocals.h>
#include <cpumask.h>

#include <storpu/mutex.h>
#include <storpu/condvar.h>

struct vm_context;

#define THREAD_RUNNING  0x000
#define THREAD_BLOCKED  0x001
#define THREAD_EXITING  0x002
#define THREAD_DEAD     0x004
#define THREAD_WAKING   0x008
#define THREAD_REAPABLE 0x010

typedef unsigned int thread_id_t;

typedef struct {
    size_t stacksize;
    void* stackaddr;
} thread_attr_t;

struct tls_tcb {
    void** tcb_dtv;
    void* tcb_pthread;
};

struct thread {
    struct list_head list;
    thread_id_t id;
    unsigned int state;
    thread_attr_t attr;
    ucontext_t ucontext;
    struct vm_context* vm_context;
    struct tls_tcb* tls_tcb;

    /* Runqueue head */
    struct list_head queue;
    /* Wait queue head */
    struct list_head wait_list;

    cond_t exited;
    mutex_t exitm;

    unsigned long (*proc)(unsigned long);
    unsigned long arg;
    unsigned long result;

    void* task;

    spinlock_t pi_lock;
    unsigned int cpu;
    unsigned int wake_cpu;
    int on_cpu;

    int on_rq;
#define THREAD_ON_RQ_QUEUED    1
#define THREAD_ON_RQ_MIGRATING 2

    const cpumask_t* cpus_ptr;
    cpumask_t cpus_mask;

    void* migration_pending;
};

#define MAIN_THREAD ((thread_id_t)-1)
#define NO_THREAD   ((thread_id_t)-2)

DECLARE_CPULOCAL(struct thread, main_thread);
DECLARE_CPULOCAL(struct thread, idle_thread);

static inline struct thread* get_current_thread(void)
{
    unsigned long sp_el0;
    asm("mrs %0, sp_el0" : "=r"(sp_el0));
    return (struct thread*)sp_el0;
}
#define current_thread get_current_thread()

#define __set_current_state(state_value) \
    __atomic_store_n(&current_thread->state, (state_value), __ATOMIC_RELAXED)

#define set_current_state(state_value) \
    __atomic_store_n(&current_thread->state, (state_value), __ATOMIC_RELEASE)

static inline unsigned int thread_cpu(struct thread* thread)
{
    return __atomic_load_n(&thread->cpu, __ATOMIC_RELAXED);
}

static inline int thread_on_rq_queued(struct thread* thread)
{
    return thread->on_rq == THREAD_ON_RQ_QUEUED;
}

static inline int thread_on_rq_migrating(struct thread* thread)
{
    return __atomic_load_n(&thread->on_rq, __ATOMIC_RELAXED) ==
           THREAD_ON_RQ_MIGRATING;
}

static inline void __set_thread_cpu(struct thread* thread, unsigned int cpu)
{
    __atomic_store_n(&thread->cpu, cpu, __ATOMIC_RELEASE);
    thread->wake_cpu = cpu;
}

void thread_init(void);
void thread_init_cpu(void);

void sched_init(void);

int thread_attr_init(thread_attr_t* attr);
int thread_attr_setstacksize(thread_attr_t* attr, size_t stacksize);
int thread_attr_setstackaddr(thread_attr_t* attr, void* stackaddr);

struct thread* thread_create(struct vm_context* vm_context, void* task,
                             const thread_attr_t* attr,
                             unsigned long (*proc)(unsigned long),
                             unsigned long arg);
struct thread* thread_create_on_cpu(struct vm_context* vm_context, void* task,
                                    const thread_attr_t* attr, unsigned int cpu,
                                    unsigned long (*proc)(unsigned long),
                                    unsigned long arg);

void schedule(void);
void schedule_tail(void);

void wake_up_new_thread(struct thread* thread);
int wake_up_thread(struct thread* thread);

int sched_setaffinity(struct thread* thread, const struct cpumask* newmask);

int thread_join(struct thread* thread, unsigned long* value);
void thread_exit(unsigned long result) __attribute__((noreturn));

void thread_reap(struct thread* thread);

thread_id_t sys_thread_self(void);
int sys_thread_create(thread_id_t* tid, const thread_attr_t* attr,
                      unsigned long (*proc)(unsigned long), unsigned long arg);
int sys_thread_join(thread_id_t tid, unsigned long* retval);
void sys_thread_exit(unsigned long result) __attribute__((noreturn));

int sys_sched_setaffinity(thread_id_t tid, size_t cpusetsize,
                          const unsigned long* mask);

#endif
