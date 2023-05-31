#ifndef _SMP_H_
#define _SMP_H_

#include <cpulocals.h>

void smp_init(void);
int ipi_setup_cpu(void);

unsigned int smp_start_cpu(unsigned int hwid);

void smp_send_reschedule(int cpu);
void smp_send_storpu_completion(void);

DECLARE_CPULOCAL(unsigned long, cpu_phys_id);

#endif
