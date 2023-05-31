#ifndef _BCH_ENGINE_H_
#define _BCH_ENGINE_H_

#include <xil_types.h>
#include <stddef.h>
#include <errno.h>

struct bch_engine;

struct bch_ops {
    int (*calculate)(struct bch_engine* engine, const u8* data,
                     size_t data_length, u8* code, uint32_t* code_length,
                     size_t offset);

    int (*correct)(struct bch_engine* engine, u8* data, size_t data_length,
                   const u8* code, uint32_t* code_length, uint64_t err_bitmap);

    int (*is_ready)(struct bch_engine* engine);

    void (*cleanup)(struct bch_engine* engine);
};

struct bch_engine {
    const struct bch_ops* ops;
};

struct bch_engine* bch_engine_init_soft(unsigned int ecc_size,
                                        unsigned int ecc_bytes);
struct bch_engine* bch_engine_init_hard(unsigned int ecc_size,
                                        unsigned int ecc_bytes, u16 dma_id,
                                        void* rx_bdbuf, void* tx_bdbuf,
                                        size_t bd_size);

static inline int bch_engine_calculate(struct bch_engine* engine,
                                       const u8* data, size_t data_length,
                                       u8* code, uint32_t* code_length,
                                       size_t offset)
{
    if (!engine->ops->calculate) return -ENOSYS;

    return engine->ops->calculate(engine, data, data_length, code, code_length,
                                  offset);
}

static inline int bch_engine_correct(struct bch_engine* engine, u8* data,
                                     size_t data_length, const u8* code,
                                     uint32_t* code_length, uint64_t err_bitmap)
{
    if (!engine->ops->correct) return -ENOSYS;

    return engine->ops->correct(engine, data, data_length, code, code_length,
                                err_bitmap);
}

static inline int bch_engine_is_ready(struct bch_engine* engine)
{
    return engine->ops->is_ready(engine);
}

#endif
