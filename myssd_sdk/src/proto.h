#ifndef _PROTO_H_
#define _PROTO_H_

#include <stddef.h>

#include <types.h>
#include <list.h>

struct iov_iter;
struct flash_transaction;
struct flash_address;

/* amu.c */
int amu_attach_domain(unsigned int nsid, size_t capacity_bytes,
                      size_t total_logical_pages, int reset);
int amu_submit_transaction(struct flash_transaction* txn);
void amu_shutdown(void);
int amu_dispatch(struct list_head* txn_list);
int amu_save_domain(unsigned int nsid);
int amu_detach_domain(unsigned int nsid);
int amu_delete_domain(unsigned int nsid);

/* block_mananger.c */
void bm_init(int wipe, int full_scan);
void bm_shutdown(void);
void bm_alloc_page(unsigned int nsid, struct flash_address* addr, int for_gc,
                   int for_mapping);
void bm_invalidate_page(struct flash_address* addr);
void bm_report_stats(void);
void bm_command_mark_bad(int argc, const char** argv);
void bm_command_save_bad(int argc, const char** argv);

/* data_cache.c */
void dc_init(size_t capacity);
int dc_process_request(struct user_request* req);
void dc_flush_ns(unsigned int nsid);
void dc_report_stats(void);

void flusher_main(int index);

/* ftl.c */
void ftl_init(void);
int ftl_process_request(struct user_request* req);
void ftl_shutdown(int abrupt);
void ftl_report_stats(void);
int ftl_get_namespace(unsigned int nsid, struct namespace_info* info);
int ftl_create_namespace(struct namespace_info* info);
int ftl_delete_namespace(unsigned int nsid);
int ftl_attach_namespace(unsigned int nsid);
int ftl_detach_namespace(unsigned int nsid);

/* ipi.c */
int ipi_setup_cpu(void);
void ipi_clear_status(void);

/* main.c */
void panic(const char* fmt, ...);
int submit_flash_transaction(struct flash_transaction* txn);
int ecc_calculate(const u8* data, size_t data_length, u8* code,
                  size_t code_length, size_t offset);
int ecc_correct(u8* data, size_t data_length, const u8* code,
                size_t code_length, uint64_t err_bitmap);

/* nvme.c */
struct nvme_command;
struct storpu_ftl_task;

void nvme_init(void);
void nvme_worker_main(void);
int nvme_worker_dispatch(u16 qid, struct nvme_command* sqe);
int nvme_worker_dispatch_storpu(struct storpu_ftl_task* task);
int nvme_dma_read(struct user_request* req, struct iov_iter* iter, size_t count,
                  size_t max_size);
int nvme_dma_write(struct user_request* req, struct iov_iter* iter,
                   size_t count, size_t max_size);
void nvme_request_shutdown(void);

#ifdef __UM__
struct ublksrv_aio;
int nvme_worker_dispatch_aio(struct ublksrv_aio* aio);
int nvme_shutdown_busy(void);
#endif

/* pcie.c */
int pcie_setup(void);
void pcie_stop(void);
int pcie_dma_read_iter(unsigned long addr, struct iov_iter* iter, size_t count);
int pcie_dma_write_iter(unsigned long addr, struct iov_iter* iter,
                        size_t count);
int pcie_dma_read(unsigned long addr, u8* buffer, size_t count);
int pcie_dma_write(unsigned long addr, const u8* buffer, size_t count);
int pcie_send_completion(unsigned long addr, u16 requester_id, u8 tag,
                         const u8* buffer, size_t count);
int pcie_send_msi(u16 vector);

/* tls.c */
void tls_init(void);

/* uart.c */
int uart_setup(void);
void uart_set_recv_data_handler(void (*handler)(const u8*, size_t));

/* dbgcon.c */
void dbgcon_setup(void);

/* cpulocals.c */
void cpulocals_init(void);

/* profile.c */
void profile_init(void);
void profile_dump(int argc, const char** argv);

/* zdma.c */
int zdma_setup(void);
int zdma_memcpy(void* dst, const void* src, size_t n);
ssize_t zdma_iter_copy_from(struct iov_iter* iter, void* buf, size_t bytes,
                            int sync_cache);
ssize_t zdma_iter_copy_to(struct iov_iter* iter, const void* buf, size_t bytes,
                          int sync_cache);

#endif
