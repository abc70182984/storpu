#include "libmcmq/config_reader.h"
#include "libmcmq/io_thread_synthetic.h"
#include "libmcmq/result_exporter.h"
#include "libunvme/memory_space.h"
#include "libunvme/nvme_driver.h"
#include "libunvme/pcie_link_mcmq.h"
#include "libunvme/pcie_link_vfio.h"

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
            ("m,memory", "Path to the shared memory file", cxxopts::value<std::string>()->default_value("/dev/shm/ivshmem"))
            ("c,config", "Path to the SSD config file", cxxopts::value<std::string>()->default_value("ssdconfig.yaml"))
            ("w,workload", "Workload type", cxxopts::value<std::string>()->default_value("stats"))
            ("r,result", "Path to the result file", cxxopts::value<std::string>()->default_value("result.json"))
            ("g,group", "VFIO group", cxxopts::value<std::string>())
            ("d,device", "PCI device ID", cxxopts::value<std::string>())
            ("L,lib", "Context library", cxxopts::value<std::string>())
            ("s,datasize", "Data size", cxxopts::value<size_t>())
            ("B,bits", "Bit width for stats workload", cxxopts::value<int>()->default_value("64"))
            ("N,worker", "Number of worker threads", cxxopts::value<int>()->default_value("1"))
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

int main(int argc, char* argv[])
{
    spdlog::cfg::load_env_levels();

    auto args = parse_arguments(argc, argv);

    std::string backend;
    std::string config_file, workload, result_file;
    std::string library;
    size_t data_size;
    int N;
    try {
        backend = args["backend"].as<std::string>();
        config_file = args["config"].as<std::string>();
        workload = args["workload"].as<std::string>();
        result_file = args["result"].as<std::string>();
        library = args["lib"].as<std::string>();
        data_size = args["datasize"].as<size_t>();
        N = args["worker"].as<int>();
    } catch (const OptionException& e) {
        spdlog::error("Failed to parse options: {}", e.what());
        exit(EXIT_FAILURE);
    }

    mcmq::SsdConfig ssd_config;
    if (!ConfigReader::load_ssd_config(config_file, ssd_config)) {
        spdlog::error("Failed to read SSD config");
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

    NVMeDriver driver(8, 1024, link.get(), memory_space.get(), false);
    link->send_config(ssd_config);
    driver.start();

    unsigned int ctx = driver.create_context(library);
    spdlog::info("Created context {}", ctx);

    driver.set_thread_id(1);

    bool do_stats = false;
    int stats_bits = 64;

    bool do_knn = false;
    bool do_grep = false;

    if (workload == "stats") {
        do_stats = true;

        try {
            stats_bits = args["bits"].as<int>();
        } catch (const OptionException& e) {
            spdlog::error("Failed to parse options: {}", e.what());
            exit(EXIT_FAILURE);
        }
    }

    if (workload == "knn") {
        do_knn = true;

        try {
            stats_bits = args["bits"].as<int>();
        } catch (const OptionException& e) {
            spdlog::error("Failed to parse options: {}", e.what());
            exit(EXIT_FAILURE);
        }
    }

    if (workload == "grep") {
        do_grep = true;
    }

    {
        using std::chrono::duration;
        using std::chrono::high_resolution_clock;

        auto* scratchpad = driver.get_scratchpad();

        auto t1 = high_resolution_clock::now();
        auto t2 = t1;

        int finished_threads = 0;
        std::mutex mutex;
        std::condition_variable cv;

        if (do_stats) {
            struct {
                unsigned long start_offset;
                unsigned long end_offset;
                int bits;
                int tid;
            } stats_arg;

            auto argbuf = scratchpad->allocate(sizeof(stats_arg) * N);

            for (int i = 0; i < N; i++) {
                size_t start = i * (data_size / N);
                size_t end = (i + 1) * (data_size / N);
                end = std::min(end, data_size);

                stats_arg.bits = stats_bits;
                stats_arg.tid = i;
                stats_arg.start_offset = start;
                stats_arg.end_offset = end;

                scratchpad->write(argbuf + sizeof(stats_arg) * i, &stats_arg,
                                  sizeof(stats_arg));

                driver.invoke_function_async(
                    ctx, ENTRY_stats_workload, argbuf + sizeof(stats_arg) * i,
                    [&](NVMeDriver::NVMeStatus status,
                        const NVMeDriver::NVMeResult& res) {
                        std::unique_lock<std::mutex> lock(mutex);
                        finished_threads++;
                        cv.notify_one();
                    });
            }

            t1 = high_resolution_clock::now();

            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [N, &finished_threads] {
                    return finished_threads >= N;
                });
            }

            t2 = high_resolution_clock::now();

            scratchpad->free(argbuf, sizeof(stats_arg) * N);
        }

        if (do_knn) {
            struct {
                unsigned long start_offset;
                unsigned long end_offset;
                int bits;
                int tid;
            } stats_arg;

            auto argbuf = scratchpad->allocate(sizeof(stats_arg) * N);

            for (int i = 0; i < N; i++) {
                size_t start = i * (data_size / N);
                size_t end = (i + 1) * (data_size / N);
                end = std::min(end, data_size);

                stats_arg.bits = stats_bits;
                stats_arg.tid = i;
                stats_arg.start_offset = start;
                stats_arg.end_offset = end;

                scratchpad->write(argbuf + sizeof(stats_arg) * i, &stats_arg,
                                  sizeof(stats_arg));

                driver.invoke_function_async(
                    ctx, ENTRY_knn_workload, argbuf + sizeof(stats_arg) * i,
                    [&](NVMeDriver::NVMeStatus status,
                        const NVMeDriver::NVMeResult& res) {
                        std::unique_lock<std::mutex> lock(mutex);
                        finished_threads++;
                        cv.notify_one();
                    });
            }

            t1 = high_resolution_clock::now();

            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [N, &finished_threads] {
                    return finished_threads >= N;
                });
            }

            t2 = high_resolution_clock::now();

            scratchpad->free(argbuf, sizeof(stats_arg) * N);
        }

        if (do_grep) {
            struct {
                unsigned long start_offset;
                unsigned long end_offset;
                int bits;
                int tid;
            } stats_arg;

            auto argbuf = scratchpad->allocate(sizeof(stats_arg) * N);

            for (int i = 0; i < N; i++) {
                size_t start = i * (data_size / N);
                size_t end = (i + 1) * (data_size / N);
                end = std::min(end, data_size);

                stats_arg.bits = 0;
                stats_arg.tid = i;
                stats_arg.start_offset = start;
                stats_arg.end_offset = end;

                scratchpad->write(argbuf + sizeof(stats_arg) * i, &stats_arg,
                                  sizeof(stats_arg));

                driver.invoke_function_async(
                    ctx, ENTRY_grep_workload, argbuf + sizeof(stats_arg) * i,
                    [&](NVMeDriver::NVMeStatus status,
                        const NVMeDriver::NVMeResult& res) {
                        std::unique_lock<std::mutex> lock(mutex);
                        finished_threads++;
                        cv.notify_one();
                    });
            }

            t1 = high_resolution_clock::now();

            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [N, &finished_threads] {
                    return finished_threads >= N;
                });
            }

            t2 = high_resolution_clock::now();

            scratchpad->free(argbuf, sizeof(stats_arg) * N);
        }

        duration<double> delta = t2 - t1;
        spdlog::info("Execution time: {}s", delta.count());
    }

    driver.delete_context(ctx);

    driver.shutdown();
    link->stop();

    return 0;
}
