#ifndef _KREF_H_
#define _KREF_H_

struct kref {
    volatile int refcount;
};

static inline void kref_init(struct kref* kref) { kref->refcount = 1; }

static inline void kref_get(struct kref* kref)
{
    __sync_add_and_fetch(&kref->refcount, 1);
}

static inline int kref_put(struct kref* kref,
                           void (*release)(struct kref* kref))
{
    if (__sync_sub_and_fetch(&kref->refcount, 1) == 0) {
        release(kref);
        return 1;
    }

    return 0;
}

#endif
