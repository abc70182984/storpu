#include "libmcmq/config_reader.h"
#include "libunvme/memory_space.h"
#include "libunvme/nvme_driver.h"
#include "libunvme/pcie_link_mcmq.h"
#include "libunvme/pcie_link_vfio.h"

#include "pgtest/aggregate.h"
#include "pgtest/btree.h"
#include "pgtest/buffer.h"
#include "pgtest/bufpage.h"
#include "pgtest/catalog.h"
#include "pgtest/fmgr.h"
#include "pgtest/heap.h"
#include "pgtest/index.h"
#include "pgtest/relation.h"
#include "pgtest/tupdesc.h"

#include "pgtest/fmgrprotos.h"

#include "cxxopts.hpp"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"

#include <chrono>
#include <thread>

#include <libtest_symbols.h>

using cxxopts::OptionException;

void relation_set_nvme_driver(NVMeDriver* drv);

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

static Datum scan_func_int(PG_FUNCTION_ARGS)
{
    printf("Datum %d\n", (unsigned int)PG_GETARG_DATUM(0));
    return 1;
}

static Datum scan_func1(PG_FUNCTION_ARGS)
{
    char* str = text_to_cstring((text*)PG_GETARG_DATUM(0));
    printf("Datum %s\n", str);
    free(str);

    return 1;
}

static Datum scan_func2(PG_FUNCTION_ARGS)
{
    printf("Datum %d\n", VARSIZE_ANY_EXHDR(PG_GETARG_DATUM(0)));
    return 1;
}

Datum F_int4_gt(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);

    return (int32_t)lhs > (int32_t)rhs;
}

static Datum F_int2_ge(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);

    return (int16_t)lhs >= (int16_t)rhs;
}

static Datum F_int4_ge(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);

    return (int32_t)lhs >= (int32_t)rhs;
}

int main(int argc, char* argv[])
{
    spdlog::cfg::load_env_levels();

    auto args = parse_arguments(argc, argv);

    std::string backend;
    std::string config_file, workload_file, result_file;
    try {
        backend = args["backend"].as<std::string>();
        config_file = args["config"].as<std::string>();
        workload_file = args["workload"].as<std::string>();
        result_file = args["result"].as<std::string>();
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
    relation_set_nvme_driver(&driver);

    driver.set_thread_id(1);

    bool do_scan = false;
    bool do_agg = false;
    bool do_index_loop = true;

    {
        using std::chrono::duration;
        using std::chrono::high_resolution_clock;

        RelationData rfp;
        RelationData rind;

        auto t1 = high_resolution_clock::now();
        auto t2 = t1;

        if (do_scan) {
            rel_open_relation(&rfp, REL_OID_TABLE_TPCH_LINEITEM);

            TableScanDesc scan;

            Numeric arg = int64_to_numeric(71032);

            ScanKeyData skey[1];
            ScanKeyInit(&skey[0], 6, 0, numeric_le, (Datum)arg);

            SnapshotData snapshot;

            scan = heap_beginscan(&rfp, &snapshot, 1, skey);
            heap_rescan(scan, NULL);

            t1 = high_resolution_clock::now();

            size_t count = 0;
            while (true) {
                if (count && ((count % 100000) == 0))
                    spdlog::info("Processed {} tuples", count);

                auto* htup = heap_getnext(scan, ForwardScanDirection);

                if (!htup) break;

                count++;
            }

            t2 = high_resolution_clock::now();
            heap_endscan(scan);
        }

        if (do_agg) {
            rel_open_relation(&rfp, REL_OID_TABLE_TPCH_LINEITEM);

            TableScanDesc scan;
            SnapshotData snapshot;

            scan = heap_beginscan(&rfp, &snapshot, 0, NULL);
            heap_rescan(scan, NULL);

            AggState* agg;

            // AggregateDescData agg_desc = {.attnum = 6, .agg_id = 2803};
            AggregateDescData agg_desc[] = {
                {.attnum = 6, .agg_id = 2114},
                {.attnum = 6, .agg_id = 2130},
                {.attnum = 6, .agg_id = 2146},
                {.attnum = 6, .agg_id = 2803},
            };

            // AggregateDescData agg_desc[] = {
            //     {.attnum = 2, .agg_id = 2108},
            //     {.attnum = 2, .agg_id = 2116},
            //     {.attnum = 2, .agg_id = 2132},
            //     {.attnum = 2, .agg_id = 2803},
            // };

            size_t group_size = 2048;

            agg = agg_init(scan, agg_desc,
                           sizeof(agg_desc) / sizeof(agg_desc[0]), group_size);

            TupleTableSlot* slot;

            t1 = high_resolution_clock::now();

            while (true) {
                slot = agg_getnext(agg);
                if (!slot) break;
                // printf("%ld\n", (int64_t)slot->tts_values[0]);
            }

            t2 = high_resolution_clock::now();

            agg_end(agg);
            heap_endscan(scan);
        }

        if (do_index_loop) {
            rel_open_relation(&rfp, REL_OID_TABLE_TPCC_ORDERS);
            rel_open_relation(&rind, REL_OID_INDEX_TPCC_ORDERS);

            SnapshotData snapshot;
            IndexScanDesc scan_ind = index_beginscan(&rfp, &rind, &snapshot, 4);

            ScanKeyData skey[4];
            /* clang-format off */
            ScanKeyInit(&skey[0], 1, BTGreaterEqualStrategyNumber, F_int2_ge, (Datum)1);
            ScanKeyInit(&skey[1], 2, BTGreaterEqualStrategyNumber, F_int2_ge, (Datum)1);
            ScanKeyInit(&skey[2], 3, BTGreaterEqualStrategyNumber, F_int4_ge, (Datum)5);
            ScanKeyInit(&skey[3], 4, BTGreaterStrategyNumber, F_int4_gt, (Datum)0);
            /* clang-format on */

            index_rescan(scan_ind, skey, 4);

            for (int i = 0; i < 200; i++) {
                Datum values[8];
                bool isnull[8];

                HeapTuple htup =
                    index_getnext_slot(scan_ind, ForwardScanDirection);
                printf("%p %d\n", htup->t_data, htup->t_len);
                heap_deform_tuple(htup, &tpcc_orders1_schema, values, isnull);
                printf("%ld %ld %ld %ld\n", values[0], values[1], values[2],
                       values[3]);
            }

            index_endscan(scan_ind);
        }

        duration<double> delta = t2 - t1;

        spdlog::info("Execution time: {}s", delta.count());
    }

    relation_set_nvme_driver(nullptr);
    driver.shutdown();
    link->stop();

    return 0;
}
