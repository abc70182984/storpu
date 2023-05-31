#ifndef _CPU_STOP_H_
#define _CPU_STOP_H_

#include <list.h>

typedef int (*cpu_stop_fn_t)(void* arg);

struct cpu_stop_work {
    struct list_head list;
    cpu_stop_fn_t fn;
    void* arg;
};

void cpu_stop_init(void);
void cpu_stop_init_cpu(void);

int stop_one_cpu_nowait(unsigned int cpu, cpu_stop_fn_t fn, void* arg,
                        struct cpu_stop_work* work_buf);

#endif
