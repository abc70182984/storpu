#include <stdlib.h>

void* slaballoc(size_t bytes) { return malloc(bytes); }

void slabfree(void* mem, size_t bytes) { free(mem); }
