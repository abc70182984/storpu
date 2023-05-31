#include "bch.h"
#include "bch_engine.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <utils.h>

#define ATCM_BASE_ADDR XPAR_PSU_R5_1_ATCM_S_AXI_BASEADDR
#define ATCM_LIMIT     (XPAR_PSU_R5_1_ATCM_S_AXI_HIGHADDR + 1)
#define BTCM_BASE_ADDR XPAR_PSU_R5_1_BTCM_S_AXI_BASEADDR
#define BTCM_LIMIT     (XPAR_PSU_R5_1_BTCM_S_AXI_HIGHADDR + 1)

struct bch_soft_engine {
    struct bch_engine base;

    struct bch_control* bch;
    unsigned char* ecc_mask;
    unsigned int* err_loc;
    unsigned int step_size;
    unsigned int code_size;
};

#define to_soft_engine(base) ((struct bch_soft_engine*)(base))

static struct bch_soft_engine soft_engine;

static void bch_soft_cleanup(struct bch_engine* base)
{
    struct bch_soft_engine* engine = to_soft_engine(base);

    bch_free(engine->bch);
    free(engine->err_loc);
    free(engine->ecc_mask);
}

int calculate_block(struct bch_soft_engine* engine, const unsigned char* buf,
                    unsigned char* code)
{
    unsigned int i;

    memset(code, 0, engine->code_size);
    bch_encode(engine->bch, buf, engine->step_size, code);

    for (i = 0; i < engine->code_size; i++)
        code[i] ^= engine->ecc_mask[i];

    return 0;
}

int correct_block(struct bch_soft_engine* engine, unsigned char* buf,
                  const unsigned char* read_ecc, const unsigned char* calc_ecc)
{
    unsigned int step_size = engine->step_size;
    unsigned int* errloc = engine->err_loc;
    int i, count;

    count = bch_decode(engine->bch, NULL, step_size, read_ecc, calc_ecc, NULL,
                       errloc);

    if (count > 0) {
        for (i = 0; i < count; i++) {
            if (errloc[i] < (step_size * 8))
                buf[errloc[i] >> 3] ^= (1 << (errloc[i] & 7));
        }
    } else if (count < 0) {
        count = -EBADMSG;
    }

    return count;
}

static int bch_soft_calculate(struct bch_engine* base, const u8* data,
                              size_t data_length, u8* code,
                              uint32_t* code_length, size_t offset)
{
    struct bch_soft_engine* engine = to_soft_engine(base);
    unsigned int i;
    int start = offset / engine->step_size;
    int nblocks = ((data_length + engine->step_size - 1) / engine->step_size);
    size_t code_size = (start + nblocks) * engine->code_size;

    if (*code_length < code_size) {
        return -ENOMEM;
    }

    for (i = start; i < start + nblocks; i++) {
        calculate_block(engine, data + i * engine->step_size,
                        code + i * engine->code_size);
    }

    *code_length = code_size;
    return 0;
}

static int bch_soft_correct(struct bch_engine* base, u8* data,
                            size_t data_length, const u8* code,
                            uint32_t* code_length, uint64_t err_bitmap)
{
    struct bch_soft_engine* engine = to_soft_engine(base);
    unsigned int i;
    int nblocks = ((data_length + engine->step_size - 1) / engine->step_size);
    size_t code_size = nblocks * engine->code_size;
    unsigned char* calc_ecc =
        (unsigned char*)BTCM_BASE_ADDR; /* Use BTCM to hold calculated ECC. */
    int r = 0;

    if (unlikely(!err_bitmap)) return 0;

    if (*code_length < code_size) {
        return -ENOMEM;
    }

    for (i = 0; i < nblocks; i++) {
        if (err_bitmap & (1ULL << i)) {
            calculate_block(engine, data + i * engine->step_size,
                            calc_ecc + i * engine->code_size);
        }
    }

    for (i = 0; i < nblocks; i++) {
        if (err_bitmap & (1ULL << i)) {
            r = correct_block(engine, data + i * engine->step_size,
                              code + i * engine->code_size,
                              calc_ecc + i * engine->code_size);
            if (r < 0) break;
        }
    }

    *code_length = code_size;

    return r;
}

int bch_soft_is_ready(struct bch_engine* engine) { return TRUE; }

static const struct bch_ops bch_soft_ops = {
    .calculate = bch_soft_calculate,
    .correct = bch_soft_correct,
    .is_ready = bch_soft_is_ready,
    .cleanup = bch_soft_cleanup,
};

struct bch_engine* bch_engine_init_soft(unsigned int ecc_size,
                                        unsigned int ecc_bytes)
{
    struct bch_soft_engine* engine = &soft_engine;
    unsigned int m, t, i;
    unsigned char* erased_page;

    memset(engine, 0, sizeof(*engine));

    m = fls(1 + (8 * ecc_size));
    t = (ecc_bytes * 8) / m;

    engine->step_size = ecc_size;
    engine->code_size = ecc_bytes;

    engine->bch = bch_init(m, t, 0, 0);
    if (!engine->bch) return NULL;

    engine->ecc_mask = malloc(ecc_bytes);
    engine->err_loc = calloc(t, sizeof(*engine->err_loc));
    if (!engine->ecc_mask || !engine->err_loc) return NULL;

    memset(engine->ecc_mask, 0, ecc_bytes);

    erased_page = malloc(ecc_size);
    if (!erased_page) goto cleanup;

    memset(erased_page, 0xff, ecc_size);
    bch_encode(engine->bch, erased_page, ecc_size, engine->ecc_mask);
    free(erased_page);

    for (i = 0; i < ecc_bytes; i++)
        engine->ecc_mask[i] ^= 0xff;

    engine->base.ops = &bch_soft_ops;

    return &engine->base;

cleanup:
    free(engine->ecc_mask);
    free(engine->err_loc);
    return NULL;
}
