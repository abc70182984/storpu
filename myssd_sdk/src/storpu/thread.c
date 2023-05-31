#include <xil_assert.h>
#include <string.h>
#include <errno.h>

#include <types.h>
#include <storpu/thread.h>
#include <storpu/vm.h>
#include <page.h>
#include <idr.h>
#include <sysreg.h>
#include <memalloc.h>
#include <slab.h>
#include <utils.h>
#include <storpu.h>
#include <storpu/rwlock.h>

#define MAX_FREE_THREAD 128

static rwlock_t thread_idr_lock;
static struct idr thread_idr;

static spinlock_t freelist_lock;
static struct list_head free_threads;
static unsigned int nr_free_threads;

DEFINE_CPULOCAL(struct thread, main_thread);
DEFINE_CPULOCAL(struct thread, idle_thread);

#define THREAD_STACK_MIN 0x2000

static thread_attr_t __default_thread_attr = {
    .stacksize = THREAD_STACK_MIN,
    .stackaddr = NULL,
};

static void thread_trampoline(void);

static inline struct thread* thread_find(thread_id_t tid)
{
    struct thread* thread;

    rwlock_rdlock(&thread_idr_lock);
    thread = (struct thread*)idr_find(&thread_idr, (unsigned long)tid);
    rwlock_unlock(&thread_idr_lock);

    return thread;
}

int thread_attr_init(thread_attr_t* attr)
{
    attr->stackaddr = NULL;
    attr->stacksize = 0;

    return 0;
}

int thread_attr_setstacksize(thread_attr_t* attr, size_t stacksize)
{
    if (stacksize == 0) {
        return EINVAL;
    }

    attr->stacksize = stacksize;

    return 0;
}

int thread_attr_setstackaddr(thread_attr_t* attr, void* stackaddr)
{
    if (!stackaddr) {
        return EINVAL;
    }

    attr->stackaddr = stackaddr;

    return 0;
}

static void thread_reset(struct thread* thread)
{
    if (thread->vm_context) {
        vm_put_context(thread->vm_context);
        thread->vm_context = NULL;
    }

    if (thread->attr.stackaddr == NULL) {
        if (thread->ucontext.uc_stack.ss_sp != NULL) {
            free_mem(__pa(thread->ucontext.uc_stack.ss_sp),
                     thread->ucontext.uc_stack.ss_size);
        }
    }

    memset(thread, 0, sizeof(*thread));

    thread->id = NO_THREAD;
    thread->state = THREAD_DEAD;
}

void thread_init(void)
{
    static int initialized = FALSE;

    if (initialized) return;

    spinlock_init(&freelist_lock);
    INIT_LIST_HEAD(&free_threads);

    rwlock_init(&thread_idr_lock, NULL);
    idr_init(&thread_idr);

    initialized = TRUE;
}

static void idle_thread_func(void)
{
    for (;;) {
        schedule();
    }
}

static void thread_init_idle(struct thread* idle, int cpu)
{
    size_t stacksize;
    void* stackaddr;

    thread_reset(idle);

    idle->state = THREAD_RUNNING;

    stacksize = ARCH_PG_SIZE;
    stackaddr = alloc_vmpages(stacksize >> ARCH_PG_SHIFT, ZONE_PS_DDR);

    spinlock_init(&idle->pi_lock);
    __set_thread_cpu(idle, cpu);
    cpumask_set_cpu(&idle->cpus_mask, cpu);
    idle->cpus_ptr = &idle->cpus_mask;

    idle->ucontext.uc_stack.ss_sp = stackaddr;
    idle->ucontext.uc_stack.ss_size = stacksize;
    makecontext(&idle->ucontext, idle_thread_func, 0);
}

void thread_init_cpu(void)
{
    struct thread* thr_main = get_cpulocal_var_ptr(main_thread);
    struct thread* idle = get_cpulocal_var_ptr(idle_thread);
    int cpu = cpuid;

    thread_reset(thr_main);
    spinlock_init(&thr_main->pi_lock);
    thr_main->id = MAIN_THREAD;
    thr_main->cpu = cpu;
    if (getcontext(&thr_main->ucontext) == -1)
        panic("cannot get context for main thread");

    write_sysreg((unsigned long)thr_main, sp_el0);

    wake_up_new_thread(thr_main);

    thread_init_idle(idle, cpu);
}

static void thread_free(struct thread* thread)
{
    spin_lock(&freelist_lock);

    if (nr_free_threads >= MAX_FREE_THREAD)
        SLABFREE(thread);
    else {
        list_add(&thread->list, &free_threads);
        nr_free_threads++;
    }

    spin_unlock(&freelist_lock);
}

static int thread_init_context(struct thread* thread,
                               struct vm_context* vm_context, void* task,
                               const thread_attr_t* attr, unsigned int cpu,
                               unsigned long (*proc)(unsigned long),
                               unsigned long arg)
{
    size_t stacksize;
    void* stackaddr;
    int r;

    memset(thread, 0, sizeof(*thread));

    rwlock_wrlock(&thread_idr_lock);
    thread->id = idr_alloc(&thread_idr, thread, 1, 0);
    rwlock_unlock(&thread_idr_lock);
    if (thread->id < 0) return -thread->id;

    thread->state = THREAD_DEAD;
    thread->vm_context = vm_get_context(vm_context);
    thread->task = task;
    thread->proc = proc;
    thread->arg = arg;

    if (attr) {
        thread->attr = *attr;
    } else {
        thread->attr = __default_thread_attr;
    }

    cond_init(&thread->exited, NULL);
    mutex_init(&thread->exitm, NULL);

    stacksize = thread->attr.stacksize;
    stackaddr = thread->attr.stackaddr;

    if (stacksize < THREAD_STACK_MIN) {
        stacksize = THREAD_STACK_MIN;
        stackaddr = thread->attr.stackaddr = NULL;
    }

    if (stackaddr == NULL) {
        stacksize = roundup(stacksize, ARCH_PG_SIZE);
        stackaddr = alloc_vmpages(stacksize >> ARCH_PG_SHIFT, ZONE_PS_DDR);
        if (!stackaddr) return ENOMEM;
    }

    if (vm_context) {
        r = ldso_allocate_tls(vm_context, &thread->tls_tcb);
        if (r) {
            rwlock_wrlock(&thread_idr_lock);
            idr_remove(&thread_idr, thread->id);
            rwlock_unlock(&thread_idr_lock);

            thread_reset(thread);
            return r;
        }
    }

    spinlock_init(&thread->pi_lock);
    __set_thread_cpu(thread, cpu);

    cpumask_copy(&thread->cpus_mask, cpu_possible_mask);
    thread->cpus_ptr = &thread->cpus_mask;

    thread->ucontext.uc_stack.ss_sp = stackaddr;
    thread->ucontext.uc_stack.ss_size = stacksize;
    makecontext(&thread->ucontext, thread_trampoline, 0);

    wake_up_new_thread(thread);

    return 0;
}

struct thread* thread_create_on_cpu(struct vm_context* vm_context, void* task,
                                    const thread_attr_t* attr, unsigned int cpu,
                                    unsigned long (*proc)(unsigned long),
                                    unsigned long arg)
{
    struct thread* thread;
    int r;

    if (proc == NULL) return NULL;

    spin_lock(&freelist_lock);
    if (list_empty(&free_threads)) {
        SLABALLOC(thread);
        if (!thread) {
            spin_unlock(&freelist_lock);
            return NULL;
        }
    } else {
        thread = list_entry(free_threads.next, struct thread, list);
        list_del(&thread->list);
        nr_free_threads--;
    }
    spin_unlock(&freelist_lock);

    r = thread_init_context(thread, vm_context, task, attr, cpu, proc, arg);
    if (r) goto out_free;

    return thread;

out_free:
    thread_free(thread);
    return NULL;
}

struct thread* thread_create(struct vm_context* vm_context, void* task,
                             const thread_attr_t* attr,
                             unsigned long (*proc)(unsigned long),
                             unsigned long arg)
{
    return thread_create_on_cpu(vm_context, task, attr, cpuid, proc, arg);
}

static void thread_stop(struct thread* thread)
{
    if (thread->state == THREAD_DEAD) {
        return;
    }

    if (thread != current_thread) {
        rwlock_wrlock(&thread_idr_lock);
        idr_remove(&thread_idr, thread->id);
        rwlock_unlock(&thread_idr_lock);

        thread_reset(thread);
        thread_free(thread);
    }
}

void thread_exit(unsigned long result)
{
    struct thread* thread = current_thread;

    if (thread->state == THREAD_EXITING) {
        return;
    }

    if (thread->task) {
        thread->result = result;
        thread->state = THREAD_REAPABLE;
    } else {
        mutex_lock(&thread->exitm);

        thread->result = result;
        thread->state = THREAD_EXITING;

        if (cond_signal(&thread->exited) != 0) {
            panic("cannot signal exit");
        }

        mutex_unlock(&thread->exitm);
    }

    schedule();
}

int thread_join(struct thread* thread, unsigned long* value)
{
    if (!thread)
        return EINVAL;
    else if (thread == current_thread)
        return EDEADLK;

    if (thread->state == THREAD_DEAD) return ESRCH;

    mutex_lock(&thread->exitm);
    while (thread->state != THREAD_EXITING) {
        cond_wait(&thread->exited, &thread->exitm);
    }
    mutex_unlock(&thread->exitm);

    if (value != NULL) *value = thread->result;

    thread_stop(thread);
    return 0;
}

void thread_reap(struct thread* thread)
{
    struct storpu_task* task;

    Xil_AssertVoid(thread->state == THREAD_REAPABLE);
    Xil_AssertVoid(thread != current_thread);
    Xil_AssertVoid(thread->task);

    task = (struct storpu_task*)thread->task;

    task->retval = 0;
    task->invoke.result = thread->result;

    enqueue_storpu_completion(task);

    thread_stop(thread);
}

static void thread_trampoline(void)
{
    struct thread* thread = current_thread;
    unsigned long result;

    schedule_tail();

    result = (thread->proc)(thread->arg);
    thread_exit(result);
}

thread_id_t sys_thread_self(void) { return current_thread->id; }

int sys_thread_create(thread_id_t* tid, const thread_attr_t* attr,
                      unsigned long (*proc)(unsigned long), unsigned long arg)
{
    struct thread* thread;

    thread = thread_create(current_thread->vm_context, NULL, attr, proc, arg);
    if (!thread) return EINVAL;

    *tid = thread->id;
    return 0;
}

int sys_thread_join(thread_id_t tid, unsigned long* retval)
{
    struct thread* thread;

    if ((thread = thread_find(tid)) == NULL) return ESRCH;

    return thread_join(thread, retval);
}

int sys_sched_setaffinity(thread_id_t tid, size_t cpusetsize,
                          const unsigned long* mask)
{
    struct thread* thread;
    struct cpumask newmask;

    if ((thread = thread_find(tid)) == NULL) return ESRCH;

    if (cpusetsize < cpumask_size())
        cpumask_clear(&newmask);
    else if (cpusetsize > cpumask_size())
        cpusetsize = cpumask_size();

    memcpy(&newmask, mask, cpusetsize);

    return sched_setaffinity(thread, &newmask);
}

void sys_thread_exit(unsigned long result) { thread_exit(result); }
