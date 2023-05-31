#ifndef _DMA_H_
#define _DMA_H_

#include <stddef.h>
#include <xil_cache.h>

enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,
    DMA_TO_DEVICE = 1,
    DMA_FROM_DEVICE = 2,
    DMA_NONE = 3,
};

#ifdef __UM__

static inline void dma_sync_single_for_cpu(void* addr, size_t size,
                                           enum dma_data_direction dir)
{}

static inline void dma_sync_single_for_device(void* addr, size_t size,
                                              enum dma_data_direction dir)
{}

#else

static inline void dma_sync_single_for_cpu(void* addr, size_t size,
                                           enum dma_data_direction dir)
{
    if (dir != DMA_TO_DEVICE) Xil_DCacheInvalidateRange((UINTPTR)addr, size);
}

static inline void dma_sync_single_for_device(void* addr, size_t size,
                                              enum dma_data_direction dir)
{
    if (dir == DMA_FROM_DEVICE)
        Xil_DCacheInvalidateRange((UINTPTR)addr, size);
    else
        Xil_DCacheFlushRange((UINTPTR)addr, size);
}

#endif

#endif
