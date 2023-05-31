#include <types.h>
#include <storpu/vm.h>

#include "region.h"

static int region_key_node_comp(void* key, struct avl_node* node)
{
    struct vm_region* r1 = (struct vm_region*)key;
    struct vm_region* r2 = avl_entry(node, struct vm_region, avl);

    unsigned long vaddr1 = r1->vir_addr;
    unsigned long vaddr2 = r2->vir_addr;

    if (vaddr1 < vaddr2)
        return -1;
    else if (vaddr1 > vaddr2)
        return 1;
    return 0;
}

static int region_node_node_comp(struct avl_node* node1, struct avl_node* node2)
{
    struct vm_region* r1 = avl_entry(node1, struct vm_region, avl);
    struct vm_region* r2 = avl_entry(node2, struct vm_region, avl);

    unsigned long vaddr1 = r1->vir_addr;
    unsigned long vaddr2 = r2->vir_addr;

    if (vaddr1 < vaddr2)
        return -1;
    else if (vaddr1 > vaddr2)
        return 1;
    return 0;
}

void region_init_avl(struct vm_context* ctx)
{
    INIT_AVL_ROOT(&ctx->mem_avl, region_key_node_comp, region_node_node_comp);
}

struct vm_region* region_lookup(struct vm_context* ctx, unsigned long addr)
{
    struct avl_node* node = ctx->mem_avl.node;

    while (node) {
        struct vm_region* vr = avl_entry(node, struct vm_region, avl);

        if (addr >= vr->vir_addr && addr < vr->vir_addr + vr->length) {
            return vr;
        } else if (addr < vr->vir_addr) {
            node = node->left;
        } else if (addr > vr->vir_addr) {
            node = node->right;
        }
    }

    return NULL;
}

void region_avl_start_iter(struct avl_root* root, struct avl_iter* iter,
                           void* key, int flags)
{
    avl_start_iter(root, iter, key, flags);
}

struct vm_region* region_avl_get_iter(struct avl_iter* iter)
{
    struct avl_node* node = avl_get_iter(iter);
    if (!node) return NULL;
    return avl_entry(node, struct vm_region, avl);
}

void region_avl_inc_iter(struct avl_iter* iter) { avl_inc_iter(iter); }

void region_avl_dec_iter(struct avl_iter* iter) { avl_dec_iter(iter); }
