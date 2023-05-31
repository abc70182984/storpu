#ifndef _FIL_FLASH_CONFIG_H_
#define _FIL_FLASH_CONFIG_H_

#include <config.h>

#define FLASH_PG_SHIFT    14U
#define FLASH_PG_SIZE     (1U << FLASH_PG_SHIFT)
#define FLASH_PG_OOB_SIZE (1872)
/* Full page size (user data + spare), rounded up to memory page size. */
#define FLASH_PG_BUFFER_SIZE \
    (roundup(FLASH_PG_SIZE + FLASH_PG_OOB_SIZE, 0x1000))

#define SECTORS_PER_FLASH_PG (FLASH_PG_SIZE >> SECTOR_SHIFT)

#ifdef __UM__

#define NR_CHANNELS       1
#define CHIPS_PER_CHANNEL 1
#define DIES_PER_CHIP     2
#define PLANES_PER_DIE    2
#define BLOCKS_PER_PLANE  1048
#define PAGES_PER_BLOCK   512

#else

#define NR_CHANNELS       8
#define CHIPS_PER_CHANNEL 2
#define DIES_PER_CHIP     2
#define PLANES_PER_DIE    2
#define BLOCKS_PER_PLANE  1048
#define PAGES_PER_BLOCK   512

#endif

#define CHANNEL_ENABLE_MASK       0b11111111
#define CHIPS_ENABLED_PER_CHANNEL CHIPS_PER_CHANNEL

/* #define ENABLE_MULTIPLANE */

/* clang-format off */

#if CHIPS_PER_CHANNEL == 4

#define CE_PINS \
    36, 20, 30, 27, \
    22, 28, 37, 43, \
    63, 58, 31, 21, \
    57, 65, 38, 54, \
    67, 64, 72, 61, \
    60, 66, 76, 73, \
    74, 71, 56, 59, \
    70, 69, 55, 75,

#elif CHIPS_PER_CHANNEL == 2

#define CE_PINS \
    30, 27, \
    37, 43, \
    31, 21, \
    38, 54, \
    67, 64,  \
    60, 66,  \
    56, 59, \
    55, 75,

#endif

/* clang-format on */

#define WP_PINS 29, 26, 52, 53, 14, 24, 62, 68

#define LSB_BITMAP                                                        \
    0xCCCCCCCCCCCCCDBFUL, 0xCCCCCCCCCCCCCCCCUL, 0xCCCCCCCCCCCCCCCCUL,     \
        0xCCCCCCCCCCCCCCCCUL, 0xCCCCCCCCCCCCCCCCUL, 0xCCCCCCCCCCCCCCCCUL, \
        0xCCCCCCCCCCCCCCCCUL, 0x6CCCCCCCCCCCCCCCUL

#define FLASH_READ_LATENCY_US    50
#define FLASH_PROGRAM_LATENCY_US 300
#define FLASH_ERASE_LATENCY_US   1200

#endif
