#include "libunvme/memory_space.h"
#include "libunvme/nvme_driver.h"
#include "libunvme/pcie_link_vfio.h"

#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"

#include "pgtest/catalog.h"
#include "pgtest/types.h"

#include "libunvme/capi.h"

#include <storpu_interface.h>

#include <libtest_symbols.h>

#define roundup(x, align) \
    (((x) % align == 0) ? (x) : (((x) + align) - ((x) % align)))

static NVMeDriver* g_nvme_driver;
static MemorySpace* g_memory_space;
static PCIeLink* g_pcie_link;

static unsigned int g_storpu_context;

extern "C"
{

    void unvme_init_driver(unsigned int num_workers, const char* group,
                           const char* device_id)
    {
        spdlog::cfg::load_env_levels();

        g_memory_space = new VfioMemorySpace(0x1000, 2 * 1024 * 1024);
        g_pcie_link =
            new PCIeLinkVfio(std::string(group), std::string(device_id));

        g_pcie_link->init();

        g_pcie_link->map_dma(*g_memory_space);
        g_pcie_link->start();

        g_nvme_driver = new NVMeDriver(num_workers, 1024, g_pcie_link,
                                       g_memory_space, false);
        g_nvme_driver->start();
    }

    void unvme_shutdown_driver()
    {
        if (g_nvme_driver) {
            g_nvme_driver->shutdown();
        }

        if (g_pcie_link) {
            g_pcie_link->stop();
        }

        delete g_memory_space;
        delete g_pcie_link;
        delete g_nvme_driver;

        g_memory_space = nullptr;
        g_pcie_link = nullptr;
        g_nvme_driver = nullptr;
    }

    void unvme_set_queue(unsigned int qid)
    {
        g_nvme_driver->set_thread_id(qid);
    }

    int unvme_read(unsigned int nsid, char* buf, size_t size, loff_t offset)
    {
        auto* mem_space = g_nvme_driver->get_dma_space();
        MemorySpace::Address dma_buf = mem_space->allocate_pages(size);
        int r = 0;

        spdlog::debug("Read buffer nsid={} offset={} size={}", nsid, offset,
                      size);

        try {
            g_nvme_driver->read(nsid, offset, dma_buf, size);

            mem_space->read(dma_buf, buf, size);
        } catch (const NVMeDriver::DeviceIOError&) {
            r = -1;
        }

        mem_space->free(dma_buf, size);

        return r;
    }

    void storpu_create_context(const char* library)
    {
        g_storpu_context = g_nvme_driver->create_context(std::string(library));
        spdlog::debug("Created StorPU context {}", g_storpu_context);
    }

    void storpu_destroy_context()
    {
        if (g_storpu_context) g_nvme_driver->delete_context(g_storpu_context);
    }

    storpu_handle_t storpu_open_relation(int relid)
    {
        storpu_handle_t handle;

        handle = (storpu_handle_t)g_nvme_driver->invoke_function(
            g_storpu_context, ENTRY_storpu_open_relation, relid);

        spdlog::debug("Open StorPU relation {}, handle: {:#x}", relid, handle);

        return handle;
    }

    void storpu_close_relation(storpu_handle_t rel)
    {
        spdlog::debug("Close StorPU relation, handle: {:#x}", rel);

        g_nvme_driver->invoke_function(
            g_storpu_context, ENTRY_storpu_close_relation, (unsigned long)rel);
    }

    struct storpu_tablescan* storpu_table_beginscan(storpu_handle_t rel,
                                                    struct storpu_scankey* skey,
                                                    int num_skeys)
    {
        struct storpu_table_beginscan_arg* arg;
        auto* scratchpad = g_nvme_driver->get_scratchpad();
        size_t argsize =
            sizeof(*arg) + num_skeys * sizeof(struct storpu_scankey);
        auto argbuf = scratchpad->allocate(argsize);

        arg = (struct storpu_table_beginscan_arg*)malloc(argsize);

        arg->relation = (void*)rel;
        arg->num_scankeys = num_skeys;
        if (num_skeys > 0) {
            for (int i = 0; i < num_skeys; i++) {
                arg->scankey[i].attr_num = skey[i].attr_num;
                arg->scankey[i].strategy = skey[i].strategy;
                arg->scankey[i].flags = skey[i].flags;
                arg->scankey[i].func = skey[i].func;

                if (skey[i].flags & SSK_REF_ARG) {
                    size_t attlen;

                    if (skey[i].flags & SSK_VARLEN_ARG)
                        attlen = VARSIZE_ANY(skey[i].arg);
                    else
                        attlen = skey[i].arglen;

                    attlen = roundup(attlen, 8);
                    auto attbuf = scratchpad->allocate(attlen);

                    scratchpad->write(attbuf, (void*)skey[i].arg, attlen);

                    arg->scankey[i].arglen = attlen;
                    arg->scankey[i].arg = attbuf;
                } else {
                    arg->scankey[i].arglen = 0;
                    arg->scankey[i].arg = skey[i].arg;
                }
            }
        }

        scratchpad->write(argbuf, arg, argsize);

        storpu_handle_t handle = g_nvme_driver->invoke_function(
            g_storpu_context, ENTRY_storpu_table_beginscan, argbuf);

        scratchpad->free(argbuf, argsize);

        if (num_skeys > 0) {
            for (int i = 0; i < num_skeys; i++) {
                if (arg->scankey[i].flags & SSK_REF_ARG)
                    scratchpad->free(arg->scankey[i].arg,
                                     arg->scankey[i].arglen);
            }
        }

        free(arg);

        spdlog::debug("Begin StorPU seqscan for table {:#x}, handle: {:#x}",
                      rel, handle);

        struct storpu_tablescan* scan =
            (struct storpu_tablescan*)malloc(sizeof(struct storpu_tablescan));

        scan->handle = handle;
        scan->buf = 0;
        scan->buf_size = 0;

        return scan;
    }

    size_t storpu_table_getnext(struct storpu_tablescan* scan, char* buf,
                                size_t buf_size)
    {
        struct storpu_table_getnext_arg arg;
        auto* scratchpad = g_nvme_driver->get_scratchpad();
        auto argbuf = scratchpad->allocate(sizeof(arg));

        if (scan->buf_size != buf_size) {
            if (scan->buf_size != 0)
                g_memory_space->free(scan->buf, scan->buf_size);

            scan->buf = g_memory_space->allocate_pages(buf_size);
            scan->buf_size = buf_size;
        }

        arg.scan_state = (void*)scan->handle;
        arg.buf = (unsigned long)scan->buf;
        arg.buf_size = buf_size;

        scratchpad->write(argbuf, &arg, sizeof(arg));

        size_t count = (size_t)g_nvme_driver->invoke_function(
            g_storpu_context, ENTRY_storpu_table_getnext, argbuf);

        if (count > 0 && count <= buf_size)
            g_memory_space->read(scan->buf, buf, count);

        scratchpad->free(argbuf, sizeof(arg));

        return count;
    }

    void storpu_table_endscan(struct storpu_tablescan* scan)
    {
        spdlog::debug("Close StorPU seqscan, handle: {:#x}", scan->handle);

        g_nvme_driver->invoke_function(g_storpu_context,
                                       ENTRY_storpu_table_endscan,
                                       (unsigned long)scan->handle);

        if (scan->buf_size != 0)
            g_memory_space->free(scan->buf, scan->buf_size);

        free(scan);
    }
}
