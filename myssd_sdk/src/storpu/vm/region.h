#ifndef _VM_REGION_H_
#define _VM_REGION_H_

#include <avl.h>
#include <list.h>

/**
 * Physical page frame
 */
struct page {
    phys_addr_t phys_addr;
    struct list_head regions; /* physical regions that reference this page */
    u16 refcount;
    u16 flags;
    void* private;
};

/* Physical page frame flags */
#define PFF_INCACHE 0x1
#define PFF_DIRTY   0x2

struct vm_context;
struct vm_region;

struct phys_region {
    struct page* page;
    struct vm_region* parent;
    unsigned long offset;

    struct list_head page_link;
    const struct region_operations* rops;
};

struct region_operations {
    int (*rop_new)(struct vm_region* vr);
    void (*rop_delete)(struct vm_region* vr);

    int (*rop_pt_flags)(const struct vm_region* vr);
    int (*rop_resize)(struct vm_context* ctx, struct vm_region* vr, size_t len);
    int (*rop_shrink_low)(struct vm_region* vr, unsigned long len);
    void (*rop_split)(struct vm_context* ctx, struct vm_region* vr,
                      struct vm_region* r1, struct vm_region* r2);

    int (*rop_page_fault)(struct vm_context* ctx, struct vm_region* vr,
                          struct phys_region* pr, unsigned int flags);

    int (*rop_writable)(const struct phys_region* pr);
    int (*rop_reference)(struct phys_region* pr, struct phys_region* new_pr);
    int (*rop_unreference)(struct phys_region* pr);

    int (*rop_sync_range)(struct vm_region* vr, unsigned long start,
                          unsigned long end);
};

struct vm_region {
    struct list_head list;
    struct avl_node avl;

    struct vm_context* ctx;

    unsigned long vir_addr;
    size_t length;
    int flags;

    struct phys_region** phys_regions;
    size_t pr_capacity;

    const struct region_operations* rops;

    union {
        struct {
            int fd;
            unsigned long offset;
            int inited;
        } file;
    } param;
};

/* Map region flags */
#define MRF_PREALLOC 0x01

extern const struct region_operations anon_map_ops;
extern const struct region_operations anon_contig_map_ops;
extern const struct region_operations file_map_ops;

static inline int region_get_prot_bits(int prot)
{
    return ((prot & PROT_READ) ? RF_READ : 0) |
           ((prot & PROT_WRITE) ? RF_WRITE : 0) |
           ((prot & PROT_EXEC) ? RF_EXEC : 0);
}

struct page* page_new(phys_addr_t phys);
void page_free(struct page* page);
void page_link(struct phys_region* pr, struct page* page, unsigned long offset,
               struct vm_region* parent);
struct phys_region* page_reference(struct page* page, unsigned long offset,
                                   struct vm_region* vr,
                                   const struct region_operations* rops);
void page_unreference(struct vm_region* vr, struct phys_region* pr, int remove);

struct phys_region* phys_region_get(struct vm_region* vr, unsigned long offset);
void phys_region_set(struct vm_region* vr, unsigned long offset,
                     struct phys_region* pr);

struct vm_region* region_map(struct vm_context* ctx, unsigned long minv,
                             unsigned long maxv, unsigned long length,
                             int flags, int map_flags,
                             const struct region_operations* rops);

int region_unmap_range(struct vm_context* ctx, unsigned long start, size_t len);

int region_free(struct vm_region* vr);

void region_init_avl(struct vm_context* ctx);
void region_avl_start_iter(struct avl_root* root, struct avl_iter* iter,
                           void* key, int flags);
struct vm_region* region_avl_get_iter(struct avl_iter* iter);
void region_avl_inc_iter(struct avl_iter* iter);
void region_avl_dec_iter(struct avl_iter* iter);

struct vm_region* region_lookup(struct vm_context* ctx, unsigned long addr);

int region_write_map_range(struct vm_context* ctx, struct vm_region* vr,
                           unsigned long start, unsigned long end);

int region_handle_pf(struct vm_context* ctx, struct vm_region* vr,
                     unsigned long offset, unsigned int flags);
int region_handle_memory(struct vm_context* ctx, struct vm_region* vr,
                         unsigned long offset, size_t len, unsigned int flags);

int region_extend_up_to(struct vm_context* ctx, unsigned long addr);

int file_map_set_file(struct vm_context* ctx, struct vm_region* vr, int fd,
                      unsigned long offset);

#endif
