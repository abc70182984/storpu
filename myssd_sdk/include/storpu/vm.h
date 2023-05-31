#ifndef _VM_H_
#define _VM_H_

#include <avl.h>
#include <list.h>
#include <page.h>
#include <spinlock.h>
#include <storpu/mutex.h>
#include <kref.h>

#define PHYS_NONE ((phys_addr_t)-2)

/* Region flags */
#define RF_READ          0x0001
#define RF_WRITE         0x0002
#define RF_EXEC          0x0004
#define RF_SHARED        0x0008
#define RF_UNINITIALIZED 0x0010
#define RF_MAP_SHARED    0x0020
#define RF_ANON          0x0100
#define RF_DIRECT        0x0200
#define RF_IO            0x0400

/* Mapping protection */
#define PROT_NONE  0x00 /* no permissions */
#define PROT_READ  0x01 /* pages can be read */
#define PROT_WRITE 0x02 /* pages can be written */
#define PROT_EXEC  0x04 /* pages can be executed */

/*
 * Flags contain sharing type and options.
 * Sharing types; choose one.
 */
#define MAP_SHARED  0x0001 /* share changes */
#define MAP_PRIVATE 0x0002 /* changes are private */

/*
 * Mapping type
 */
#define MAP_ANONYMOUS 0x0004 /* anonymous memory */
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FIXED    0x0008
#define MAP_POPULATE 0x0010
#define MAP_CONTIG   0x0020

/*
 * Error indicator returned by mmap(2)
 */
#define MAP_FAILED ((void*)-1) /* mmap() failed */

/* Flags to `msync'.  */
#define MS_ASYNC      1 /* Sync memory asynchronously.  */
#define MS_SYNC       4 /* Synchronous memory sync.  */
#define MS_INVALIDATE 2 /* Invalidate the caches.  */

enum fault_flag {
    FAULT_FLAG_WRITE = 1 << 0,
    FAULT_FLAG_USER = 1 << 1,
    FAULT_FLAG_INSTRUCTION = 1 << 2,
    FAULT_FLAG_INTERRUPTIBLE = 1 << 3,
};

struct vumap_vir {
    unsigned long addr;
    size_t size;
};

struct vumap_phys {
    phys_addr_t addr;
    size_t size;
};

struct stackframe;
struct so_info;

struct vm_context {
    unsigned int cid;
    struct kref kref;

    spinlock_t pgd_lock;
    pgdir_t pgd;

    mutex_t mmap_lock;
    struct list_head mem_regions;
    struct avl_root mem_avl;

    void* load_base;

    struct so_info* so_info;
    unsigned int tls_max_index;
    size_t tls_static_offset;
    size_t tls_static_space;

    size_t vm_total;
};

void vm_init(void);
struct vm_context* vm_find_get_context(unsigned int cid);
struct vm_context* vm_get_context(struct vm_context* ctx);
void vm_put_context(struct vm_context* ctx);
struct vm_context* vm_create_context(void);
void vm_delete_context(struct vm_context* ctx);
void vm_switch_context(struct vm_context* ctx);
int vm_handle_page_fault(unsigned long addr, unsigned int flags,
                         unsigned int vr_flags, struct stackframe* regs);
int vm_vumap(struct vm_context* ctx, const struct vumap_vir* vvec,
             unsigned int vcount, size_t offset, int write,
             struct vumap_phys* pvec, unsigned int pmax);

int vm_map(struct vm_context* ctx, void* addr, size_t len, int prot, int flags,
           int fd, unsigned long offset, void** out_addr);
int vm_unmap(struct vm_context* ctx, void* addr, size_t length);

int vm_exec(struct vm_context* ctx, void* elf_base);

struct tls_tcb;
int ldso_init_context(struct vm_context* ctx);
int ldso_allocate_tls(struct vm_context* ctx, struct tls_tcb** tcbp);

int sys_brk(void* addr);
int sys_munmap(void* addr, size_t length);
int sys_mmap(void* addr, size_t length, int prot, int flags, int fd,
             unsigned long offset, void** out_addr);
int sys_msync(void* addr, size_t length, int flags);

#endif
