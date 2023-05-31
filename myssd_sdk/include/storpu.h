#ifndef _STORPU_H_
#define _STORPU_H_

#include <llist.h>

/* FTL -> StorPU task */

#define SPU_TYPE_CREATE_CONTEXT 1
#define SPU_TYPE_DELETE_CONTEXT 2
#define SPU_TYPE_INVOKE         3

struct storpu_task {
    struct llist_node llist;
    int type;
    int retval;

    union {
        struct {
            void* so_addr;
            u32 cid;
        } create_context;

        struct {
            u32 cid;
        } delete_context;

        struct {
            u32 cid;
            unsigned long entry;
            unsigned long arg;
            unsigned long result;
        } invoke;
    };

    void* opaque;
};

void enqueue_storpu_request(struct storpu_task* req);
void enqueue_storpu_completion(struct storpu_task* resp);
struct llist_node* dequeue_storpu_completions(void);

int submit_storpu_task(struct storpu_task* task, u32 timeout_ms);

void handle_storpu_completion(void);

/* StorPU -> FTL task */

#define FTL_TYPE_FLASH_READ  1
#define FTL_TYPE_FLASH_WRITE 2
#define FTL_TYPE_HOST_READ   3
#define FTL_TYPE_HOST_WRITE  4
#define FTL_TYPE_FLUSH       5
#define FTL_TYPE_FLUSH_DATA  6
#define FTL_TYPE_SYNC        7

struct storpu_ftl_task {
    struct llist_node llist;
    int type;
    int src_cpu;
    int retval;

    /* Namespace for flash access. */
    unsigned int nsid;

    /* XXX: We can't use virtual address here because StorPU user address space
     * (ttbr1) is not mapped from the FTL core. Maybe change this to
     * vumap_phys[] to handle non-contiguous physical memory.*/
    phys_addr_t buf_phys;

    /* The target address for this I/O, e.g., host memory address or flash LPA
     */
    unsigned long addr;
    size_t count;

    void* opaque;
};

void enqueue_storpu_ftl_task(struct storpu_ftl_task* task);
struct storpu_ftl_task* dequeue_storpu_ftl_task(void);
void enqueue_storpu_ftl_completion(struct storpu_ftl_task* task);

#endif
