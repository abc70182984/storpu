#ifndef _PROFILE_H_
#define _PROFILE_H_

#define PROF_TYPE_PC    0x1
#define PROF_TYPE_FLASH 0x2

struct prof_sample_buf {
    uint32_t mem_used;
    char sample_buf[];
};

struct prof_sample_pc {
    uint32_t pc;
};

struct prof_sample_flash {
    uint8_t channel_status;
    uint64_t die_status;
};

#endif
