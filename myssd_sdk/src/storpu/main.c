#include <xil_assert.h>
#include <errno.h>

#include <types.h>
#include <llist.h>
#include <storpu.h>
#include <smp.h>
#include <intr.h>
#include <barrier.h>
#include <utils.h>

#include <storpu/vm.h>
#include <storpu/thread.h>
#include <storpu/cpu_stop.h>

static LLIST_HEAD(avail_queue);
static LLIST_HEAD(used_queue);

static LLIST_HEAD(ftl_avail_queue);
static DEFINE_CPULOCAL(struct llist_head, ftl_used_queue) = {0};

void enqueue_storpu_request(struct storpu_task* req)
{
    llist_add(&req->llist, &avail_queue);
    smp_send_reschedule(STORPU_CPU_ID);
}

struct llist_node* dequeue_storpu_completions(void)
{
    return llist_del_all(&used_queue);
}

void enqueue_storpu_completion(struct storpu_task* resp)
{
    llist_add(&resp->llist, &used_queue);
}

static inline void send_storpu_completion(void)
{
    if (!llist_empty(&used_queue)) smp_send_storpu_completion();
}

static int process_create_context(struct storpu_task* req)
{
    struct vm_context* ctx;
    int r;

    Xil_AssertNonvoid(req->type == SPU_TYPE_CREATE_CONTEXT);

    ctx = vm_create_context();
    if (!ctx) return ENOMEM;

    vm_switch_context(ctx);
    r = vm_exec(ctx, req->create_context.so_addr);

    if (r == 0) {
        req->create_context.cid = ctx->cid;
    }

    return r;
}

static int process_delete_context(struct storpu_task* req)
{
    struct vm_context* ctx;

    Xil_AssertNonvoid(req->type == SPU_TYPE_DELETE_CONTEXT);

    ctx = vm_find_get_context(req->delete_context.cid);
    if (!ctx) return ESRCH;

    vm_delete_context(ctx);
    vm_put_context(ctx);

    return 0;
}

static int process_invoke_function(struct storpu_task* req)
{
    struct vm_context* ctx;
    struct thread* thread;
    int r = 0;

    Xil_AssertNonvoid(req->type == SPU_TYPE_INVOKE);

    ctx = vm_find_get_context(req->invoke.cid);
    if (!ctx) return EINVAL;

    thread = thread_create(ctx, (void*)req, NULL,
                           ctx->load_base + req->invoke.entry, req->invoke.arg);
    if (!thread) r = ENOMEM;

    vm_put_context(ctx);
    return r;
}

static void process_request_queue(void)
{
    struct llist_node *entry, *prev;
    struct storpu_task *req, *req_next;
    int r;

    if (llist_empty(&avail_queue)) return;

    entry = llist_del_all(&avail_queue);

    prev = NULL;
    llist_for_each_entry_safe(req, req_next, entry, llist)
    {
        if (req->type == SPU_TYPE_CREATE_CONTEXT ||
            req->type == SPU_TYPE_DELETE_CONTEXT) {
            if (prev) {
                prev->next = &req_next->llist;
            } else {
                entry = &req_next->llist;
            }

            switch (req->type) {
            case SPU_TYPE_CREATE_CONTEXT:
                req->retval = process_create_context(req);
                break;
            case SPU_TYPE_DELETE_CONTEXT:
                req->retval = process_delete_context(req);
                break;
            }

            enqueue_storpu_completion(req);
        } else {
            prev = &req->llist;
        }
    }

    if (!entry) return;

    prev = NULL;
    llist_for_each_entry_safe(req, req_next, entry, llist)
    {
        if (req->type == SPU_TYPE_INVOKE) {
            if (prev) {
                prev->next = &req_next->llist;
            } else {
                entry = &req_next->llist;
            }

            r = process_invoke_function(req);

            if (r != 0) {
                req->retval = r;
                enqueue_storpu_completion(req);
            }
            /* If there is no error then reply to FTL when the thread exits. */
        } else {
            prev = &req->llist;
        }
    }
}

void enqueue_storpu_ftl_task(struct storpu_ftl_task* task)
{
    llist_add(&task->llist, &ftl_avail_queue);
    if (cpuid != FTL_CPU_ID) smp_send_reschedule(FTL_CPU_ID);
}

struct storpu_ftl_task* dequeue_storpu_ftl_task(void)
{
    struct llist_node* entry = llist_del_first(&ftl_avail_queue);
    if (!entry) return NULL;
    return llist_entry(entry, struct storpu_ftl_task, llist);
}

void enqueue_storpu_ftl_completion(struct storpu_ftl_task* task)
{
    llist_add(&task->llist, get_cpu_var_ptr(task->src_cpu, ftl_used_queue));
}

void handle_storpu_ftl_completion(void)
{
    struct llist_node* entry;
    struct storpu_task* task;

    entry = llist_del_all(get_cpulocal_var_ptr(ftl_used_queue));
    if (!entry) return;

    llist_for_each_entry(task, entry, llist)
    {
        struct thread* thread = (struct thread*)task->opaque;

        if (thread) {
            wake_up_thread(thread);
        }
    }
}

void smp_bsp_main(void)
{
    vm_init();
    thread_init();
    sched_init();

    smp_init();
}

void finish_bsp_booting(void)
{
    thread_init_cpu();

    cpu_stop_init();
    cpu_stop_init_cpu();

    intr_setup_cpu();
    ipi_setup_cpu();

    sevl();

    while (TRUE) {
        local_irq_enable();
        wfe();

        process_request_queue();
        send_storpu_completion();

        handle_storpu_ftl_completion();

        schedule();
        send_storpu_completion();
    }
}

void smp_ap_main(void)
{
    thread_init_cpu();
    cpu_stop_init_cpu();

    intr_setup_cpu();
    ipi_setup_cpu();

    sevl();

    while (TRUE) {
        local_irq_enable();
        wfe();

        handle_storpu_ftl_completion();

        schedule();
    }
}
