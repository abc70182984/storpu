#include "libmcmq/config_reader.h"
#include "libmcmq/io_thread_synthetic.h"
#include "libmcmq/result_exporter.h"
#include "libunvme/memory_space.h"
#include "libunvme/nvme_driver.h"
#include "libunvme/pcie_link_mcmq.h"
#include "libunvme/pcie_link_vfio.h"

#include "pgtest/catalog.h"
#include "pgtest/types.h"

#include <storpu_interface.h>

#include "cxxopts.hpp"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"

#include <chrono>
#include <thread>

#include <libtest_symbols.h>

#define roundup(x, align) \
    (((x) % align == 0) ? (x) : (((x) + align) - ((x) % align)))

using cxxopts::OptionException;

cxxopts::ParseResult parse_arguments(int argc, char* argv[])
{
    try {
        cxxopts::Options options(argv[0], " - Host frontend for MCMQ");

        // clang-format off
        options.add_options()
            ("b,backend", "Backend type", cxxopts::value<std::string>()->default_value("mcmq"))
            ("m,memory", "Path to the shared memory file",
            cxxopts::value<std::string>()->default_value("/dev/shm/ivshmem"))
            ("c,config", "Path to the SSD config file",
            cxxopts::value<std::string>()->default_value("ssdconfig.yaml"))
            ("w,workload", "Path to the workload file",
            cxxopts::value<std::string>()->default_value("workload.yaml"))
            ("r,result", "Path to the result file",
            cxxopts::value<std::string>()->default_value("result.json"))
            ("g,group", "VFIO group",
            cxxopts::value<std::string>())
            ("d,device", "PCI device ID",
            cxxopts::value<std::string>())
            ("L,lib", "Context library", cxxopts::value<std::string>())
            ("h,help", "Print help");
        // clang-format on

        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            std::cerr << options.help({""}) << std::endl;
            exit(EXIT_SUCCESS);
        }

        return result;
    } catch (const OptionException& e) {
        exit(EXIT_FAILURE);
    }
}

typedef unsigned long DeviceHandle;

DeviceHandle storpu_open_relation(NVMeDriver& driver, unsigned int ctx, Oid oid)
{
    return (DeviceHandle)driver.invoke_function(ctx, ENTRY_storpu_open_relation,
                                                oid);
}

void storpu_close_relation(NVMeDriver& driver, unsigned int ctx,
                           DeviceHandle rel)
{
    driver.invoke_function(ctx, ENTRY_storpu_close_relation,
                           (unsigned long)rel);
}

DeviceHandle storpu_table_beginscan(NVMeDriver& driver, unsigned int ctx,
                                    DeviceHandle rel,
                                    struct storpu_scankey* skey, int num_skeys)
{
    struct storpu_table_beginscan_arg* arg;
    auto* scratchpad = driver.get_scratchpad();
    size_t argsize = sizeof(*arg) + num_skeys * sizeof(struct storpu_scankey);
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
    spdlog::info("Argbuf {:#x} Argsize {}", argbuf, argsize);

    DeviceHandle scan =
        driver.invoke_function(ctx, ENTRY_storpu_table_beginscan, argbuf);

    scratchpad->free(argbuf, argsize);

    if (num_skeys > 0) {
        for (int i = 0; i < num_skeys; i++) {
            if (arg->scankey[i].flags & SSK_REF_ARG)
                scratchpad->free(arg->scankey[i].arg, arg->scankey[i].arglen);
        }
    }

    free(arg);

    return scan;
}

size_t storpu_table_getnext(NVMeDriver& driver, unsigned int ctx,
                            DeviceHandle scan, MemorySpace::Address buf,
                            size_t buf_size)
{
    struct storpu_table_getnext_arg arg;
    auto* scratchpad = driver.get_scratchpad();
    auto argbuf = scratchpad->allocate(sizeof(arg));

    arg.scan_state = (void*)scan;
    arg.buf = (unsigned long)buf;
    arg.buf_size = buf_size;

    scratchpad->write(argbuf, &arg, sizeof(arg));

    size_t count =
        (size_t)driver.invoke_function(ctx, ENTRY_storpu_table_getnext, argbuf);

    scratchpad->free(argbuf, sizeof(arg));

    return count;
}

void storpu_table_endscan(NVMeDriver& driver, unsigned int ctx,
                          DeviceHandle scan)
{
    driver.invoke_function(ctx, ENTRY_storpu_table_endscan,
                           (unsigned long)scan);
}

DeviceHandle storpu_aggregate_init(NVMeDriver& driver, unsigned int ctx,
                                   DeviceHandle scan, size_t group_size,
                                   struct storpu_aggdesc* aggdesc, int num_aggs)
{
    struct storpu_agg_init_arg* arg;
    auto* scratchpad = driver.get_scratchpad();
    size_t argsize = sizeof(*arg) + num_aggs * sizeof(struct storpu_aggdesc);
    auto argbuf = scratchpad->allocate(argsize);

    arg = (struct storpu_agg_init_arg*)malloc(argsize);

    arg->scan_state = (void*)scan;
    arg->group_size = group_size;
    arg->num_aggs = num_aggs;
    memcpy(arg->aggdesc, aggdesc, num_aggs * sizeof(struct storpu_aggdesc));

    scratchpad->write(argbuf, arg, argsize);

    DeviceHandle agg =
        driver.invoke_function(ctx, ENTRY_storpu_aggregate_init, argbuf);

    scratchpad->free(argbuf, argsize);
    free(arg);

    return agg;
}

size_t storpu_aggregate_getnext(NVMeDriver& driver, unsigned int ctx,
                                DeviceHandle agg, MemorySpace::Address buf,
                                size_t buf_size)
{
    struct storpu_agg_getnext_arg arg;
    auto* scratchpad = driver.get_scratchpad();
    auto argbuf = scratchpad->allocate(sizeof(arg));

    arg.agg_state = (void*)agg;
    arg.buf = (unsigned long)buf;
    arg.buf_size = buf_size;

    scratchpad->write(argbuf, &arg, sizeof(arg));

    size_t count = (size_t)driver.invoke_function(
        ctx, ENTRY_storpu_aggregate_getnext, argbuf);

    scratchpad->free(argbuf, sizeof(arg));

    return count;
}

void storpu_aggregate_end(NVMeDriver& driver, unsigned int ctx,
                          DeviceHandle agg)
{
    driver.invoke_function(ctx, ENTRY_storpu_aggregate_end, (unsigned long)agg);
}

DeviceHandle storpu_index_beginscan(NVMeDriver& driver, unsigned int ctx,
                                    DeviceHandle heap_rel,
                                    DeviceHandle index_rel, int num_skeys)
{
    struct storpu_index_beginscan_arg arg;
    auto* scratchpad = driver.get_scratchpad();
    size_t argsize = sizeof(arg);
    auto argbuf = scratchpad->allocate(argsize);

    arg.heap_relation = (void*)heap_rel;
    arg.index_relation = (void*)index_rel;
    arg.num_scankeys = num_skeys;

    scratchpad->write(argbuf, &arg, argsize);

    DeviceHandle scan =
        driver.invoke_function(ctx, ENTRY_storpu_index_beginscan, argbuf);

    scratchpad->free(argbuf, argsize);

    return scan;
}

DeviceHandle storpu_index_rescan(NVMeDriver& driver, unsigned int ctx,
                                 DeviceHandle scan, struct storpu_scankey* skey,
                                 int num_skeys)
{
    struct storpu_index_rescan_arg* arg;
    auto* scratchpad = driver.get_scratchpad();
    size_t argsize = sizeof(*arg) + num_skeys * sizeof(struct storpu_scankey);
    auto argbuf = scratchpad->allocate(argsize);

    arg = (struct storpu_index_rescan_arg*)malloc(argsize);

    arg->scan_state = (void*)scan;
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

    driver.invoke_function(ctx, ENTRY_storpu_index_rescan, argbuf);

    scratchpad->free(argbuf, argsize);

    if (num_skeys > 0) {
        for (int i = 0; i < num_skeys; i++) {
            if (arg->scankey[i].flags & SSK_REF_ARG)
                scratchpad->free(arg->scankey[i].arg, arg->scankey[i].arglen);
        }
    }

    free(arg);

    return scan;
}

size_t storpu_index_getnext(NVMeDriver& driver, unsigned int ctx,
                            DeviceHandle scan, MemorySpace::Address buf,
                            size_t buf_size, size_t limit)
{
    struct storpu_index_getnext_arg arg;
    auto* scratchpad = driver.get_scratchpad();
    auto argbuf = scratchpad->allocate(sizeof(arg));

    arg.scan_state = (void*)scan;
    arg.buf = (unsigned long)buf;
    arg.buf_size = buf_size;
    arg.limit = limit;

    scratchpad->write(argbuf, &arg, sizeof(arg));

    size_t count =
        (size_t)driver.invoke_function(ctx, ENTRY_storpu_index_getnext, argbuf);

    scratchpad->free(argbuf, sizeof(arg));

    return count;
}

void storpu_index_endscan(NVMeDriver& driver, unsigned int ctx,
                          DeviceHandle scan)
{
    driver.invoke_function(ctx, ENTRY_storpu_index_endscan,
                           (unsigned long)scan);
}

int main(int argc, char* argv[])
{
    spdlog::cfg::load_env_levels();

    auto args = parse_arguments(argc, argv);

    std::string backend;
    std::string config_file, workload_file, result_file;
    std::string library;
    try {
        backend = args["backend"].as<std::string>();
        config_file = args["config"].as<std::string>();
        workload_file = args["workload"].as<std::string>();
        result_file = args["result"].as<std::string>();
        library = args["lib"].as<std::string>();
    } catch (const OptionException& e) {
        spdlog::error("Failed to parse options: {}", e.what());
        exit(EXIT_FAILURE);
    }

    HostConfig host_config;
    mcmq::SsdConfig ssd_config;
    if (!ConfigReader::load_ssd_config(config_file, ssd_config)) {
        spdlog::error("Failed to read SSD config");
        exit(EXIT_FAILURE);
    }

    if (!ConfigReader::load_host_config(workload_file, ssd_config,
                                        host_config)) {
        spdlog::error("Failed to read workload config");
        exit(EXIT_FAILURE);
    }

    std::unique_ptr<MemorySpace> memory_space;
    std::unique_ptr<PCIeLink> link;

    if (backend == "mcmq") {
        std::string shared_memory;

        try {
            shared_memory = args["memory"].as<std::string>();
        } catch (const OptionException& e) {
            spdlog::error("Failed to parse options: {}", e.what());
            exit(EXIT_FAILURE);
        }

        memory_space = std::make_unique<SharedMemorySpace>(shared_memory);
        link = std::make_unique<PCIeLinkMcmq>();
    } else if (backend == "vfio") {
        std::string group, device_id;

        try {
            group = args["group"].as<std::string>();
            device_id = args["device"].as<std::string>();
        } catch (const OptionException& e) {
            spdlog::error("Failed to parse options: {}", e.what());
            exit(EXIT_FAILURE);
        }

        memory_space =
            std::make_unique<VfioMemorySpace>(0x1000, 2 * 1024 * 1024);
        link = std::make_unique<PCIeLinkVfio>(group, device_id);
    } else {
        spdlog::error("Unknown backend type: {}", backend);
        return EXIT_FAILURE;
    }

    if (!link->init()) {
        spdlog::error("Failed to initialize PCIe link");
        return EXIT_FAILURE;
    }

    link->map_dma(*memory_space);
    link->start();

    NVMeDriver driver(host_config.flows.size(), host_config.io_queue_depth,
                      link.get(), memory_space.get(), false);
    link->send_config(ssd_config);
    driver.start();

    unsigned int ctx = driver.create_context(library);
    spdlog::info("Created context {}", ctx);

    driver.set_thread_id(1);

    bool do_scan = false;
    bool do_agg = false;
    bool do_index_lookup = true;

    {
        using std::chrono::duration;
        using std::chrono::high_resolution_clock;

        auto t1 = high_resolution_clock::now();
        auto t2 = t1;

        if (do_scan) {
            DeviceHandle rel;

            rel =
                storpu_open_relation(driver, ctx, REL_OID_TABLE_TPCH_LINEITEM);
            spdlog::info("Rel {:#x}", rel);

            DeviceHandle scan;

            struct storpu_scankey scankey;
            Datum scanarg = (Datum)int64_to_numeric(7903);
            scankey.attr_num = 6;
            // scankey.attr_num = 2;
            scankey.flags = SSK_VARLEN_ARG | SSK_REF_ARG;
            // scankey.flags = 0;
            scankey.strategy = 0;
            scankey.func = 1723;
            // scankey.func = 66;
            scankey.arg = scanarg;
            // scankey.arg = 20018;

            scan = storpu_table_beginscan(driver, ctx, rel, &scankey, 1);

            size_t buf_size = 16 * 0x1000;
            auto buf = memory_space->allocate_pages(16 * 0x1000);

            t1 = high_resolution_clock::now();

            while (true) {
                auto count =
                    storpu_table_getnext(driver, ctx, scan, buf, buf_size);

                if (count == 0) break;
            }

            t2 = high_resolution_clock::now();

            memory_space->free_pages(buf, buf_size);

            storpu_table_endscan(driver, ctx, scan);
            free((void*)scanarg);

            storpu_close_relation(driver, ctx, rel);
        }

        if (do_agg) {
            DeviceHandle rel;

            rel =
                storpu_open_relation(driver, ctx, REL_OID_TABLE_TPCH_LINEITEM);
            spdlog::info("Rel {:#x}", rel);

            DeviceHandle scan;

            scan = storpu_table_beginscan(driver, ctx, rel, nullptr, 0);

            // struct storpu_aggdesc agg_desc = {.attnum = 6, .aggid = 2803};
            // struct storpu_aggdesc agg_desc[] = {
            //     {.attnum = 2, .aggid = 2108},
            //     {.attnum = 2, .aggid = 2116},
            //     {.attnum = 2, .aggid = 2132},
            //     {.attnum = 2, .aggid = 2803},
            // };

            struct storpu_aggdesc agg_desc[] = {
                {.attnum = 6, .aggid = 2114},
                {.attnum = 6, .aggid = 2130},
                {.attnum = 6, .aggid = 2146},
                {.attnum = 6, .aggid = 2803},
            };

            size_t group_size = 2048;

            DeviceHandle agg =
                storpu_aggregate_init(driver, ctx, scan, group_size, agg_desc,
                                      sizeof(agg_desc) / sizeof(agg_desc[0]));

            size_t buf_size = 16 * 0x1000;
            auto buf = memory_space->allocate_pages(16 * 0x1000);

            t1 = high_resolution_clock::now();

            while (true) {
                auto count =
                    storpu_aggregate_getnext(driver, ctx, agg, buf, buf_size);

                if (count == 0) break;
            }

            t2 = high_resolution_clock::now();

            memory_space->free_pages(buf, buf_size);

            storpu_aggregate_end(driver, ctx, agg);

            storpu_table_endscan(driver, ctx, scan);

            storpu_close_relation(driver, ctx, rel);
        }

        if (do_index_lookup) {
            DeviceHandle heap_rel;
            DeviceHandle index_rel;

            heap_rel =
                storpu_open_relation(driver, ctx, REL_OID_TABLE_TPCC_ORDERS);
            index_rel =
                storpu_open_relation(driver, ctx, REL_OID_INDEX_TPCC_ORDERS);

            DeviceHandle scan;
            scan = storpu_index_beginscan(driver, ctx, heap_rel, index_rel, 4);

            // clang-format off
            struct storpu_scankey skey[] = {
                {.attr_num = 1, .strategy = 4, .flags = 0, .arglen = 0, .func = 151, .arg = 1},
                {.attr_num = 2, .strategy = 4, .flags = 0, .arglen = 0, .func = 151, .arg = 1},
                {.attr_num = 3, .strategy = 4, .flags = 0, .arglen = 0, .func = 150, .arg = 5},
                {.attr_num = 4, .strategy = 5, .flags = 0, .arglen = 0, .func = 147, .arg = 0},
            };
            // clang-format on

            storpu_index_rescan(driver, ctx, scan, skey, 4);

            size_t buf_size = 16 * 0x1000;
            auto buf = memory_space->allocate_pages(16 * 0x1000);

            t1 = high_resolution_clock::now();

            auto count =
                storpu_index_getnext(driver, ctx, scan, buf, buf_size, 1);

            t2 = high_resolution_clock::now();

            memory_space->free_pages(buf, buf_size);

            storpu_index_endscan(driver, ctx, scan);
        }

        duration<double> delta = t2 - t1;
        spdlog::info("Execution time: {}s", delta.count());
    }

    driver.delete_context(ctx);

    driver.shutdown();
    link->stop();

    return 0;
}
