#ifndef _CONFIG_H_
#define _CONFIG_H_

/* Number of APUs */
#define NR_CPUS       4
#define FTL_CPU_ID    (NR_CPUS - 1)
#define STORPU_CPU_ID 0

/* Uncomment this line to format EMMC on the first boot. */
/* #define FORMAT_EMMC */

/* Uncomment this line to wipe the entire SSD. */
/* #define WIPE_SSD */

/* Uncomment this line to perform a full bad block scan. */
/* #define FULL_BAD_BLOCK_SCAN */

/* Uncomment this line to reset FTL metadata. */
/* #define WIPE_MANIFEST */

/* Uncomment this line to wipe the mapping table. */
/* #define WIPE_MAPPING_TABLE */

/* Uncomment this line to perform self-test on the NAND controller. */
/* #define NFC_SELFTEST */

/* Whether to enable volatile write cache. */
#define USE_WRITE_CACHE 1

/* Use histograms to track request statistics. */
#define USE_HISTOGRAM 1

/* Use hardware ECC engine. */
#define USE_HARDWARE_ECC 0

/* Defer DMA initialization until PCIe link is up. */
#define LAZY_DMA_INIT 1

/* System clock frequency. */
#define SYSTEM_HZ 25

#define CONFIG_NVME_IO_QUEUE_MAX 16

#define NR_WORKER_THREADS 16
#define NR_FLUSHERS       8
#define NR_FTL_THREADS    (NR_WORKER_THREADS + NR_FLUSHERS)

#define CONFIG_STORAGE_CAPACITY_BYTES (512ULL << 30) /* 512 GiB */

#define SECTOR_SHIFT 12
#define SECTOR_SIZE  (1UL << SECTOR_SHIFT)

#define MAX_DATA_TRANSFER_SIZE 8 /* 256 pages */

#define CONFIG_DATA_CACHE_CAPACITY (512UL << 20) /* 512MB */

#define CONFIG_MAPPING_TABLE_CAPACITY (1UL << 30) /* 1GB */

#define DEFAULT_PLANE_ALLOCATE_SCHEME PAS_CWDP

#define NAMESPACE_MAX 32

#define FILE_MAX 1

#endif
