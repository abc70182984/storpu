#ifndef _PGTEST_STORPU_INTERFACE_H_
#define _PGTEST_STORPU_INTERFACE_H_

struct storpu_scankey {
    uint16_t attr_num;
    uint16_t strategy;
    uint16_t flags;
    uint16_t arglen;
    uint32_t func;
    uint32_t __zero1;
    unsigned long arg;
} __attribute__((packed));

#define SSK_VARLEN_ARG 0x1000
#define SSK_REF_ARG    0x2000

struct storpu_table_beginscan_arg {
    void* relation;

    int __rsvd0;
    int num_scankeys;
    struct storpu_scankey scankey[];
} __attribute__((packed));

struct storpu_table_getnext_arg {
    void* scan_state;
    unsigned long buf;
    size_t buf_size;
} __attribute__((packed));

struct storpu_aggdesc {
    uint16_t attnum;
    uint16_t __rsvd0;
    uint32_t aggid;
} __attribute__((packed));

struct storpu_agg_init_arg {
    void* scan_state;
    size_t group_size;
    unsigned int num_aggs;
    uint32_t __rsvd0;
    struct storpu_aggdesc aggdesc[];
};

struct storpu_agg_getnext_arg {
    void* agg_state;
    unsigned long buf;
    size_t buf_size;
} __attribute__((packed));

struct storpu_index_beginscan_arg {
    void* heap_relation;
    void* index_relation;

    int __rsvd0;
    int num_scankeys;
} __attribute__((packed));

struct storpu_index_rescan_arg {
    void* scan_state;
    int __rsvd0;
    int num_scankeys;
    struct storpu_scankey scankey[];
} __attribute__((packed));

struct storpu_index_getnext_arg {
    void* scan_state;
    unsigned long buf;
    size_t buf_size;
    size_t limit;
} __attribute__((packed));

#endif
