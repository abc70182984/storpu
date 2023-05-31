#ifndef _SLAB_H_
#define _SLAB_H_

void slabs_init();
void* slaballoc(size_t bytes);
void slabfree(void* mem, size_t bytes);
#define SLABALLOC(p)               \
    do {                           \
        p = slaballoc(sizeof(*p)); \
    } while (0)
#define SLABFREE(p)              \
    do {                         \
        slabfree(p, sizeof(*p)); \
        p = NULL;                \
    } while (0)

#endif
