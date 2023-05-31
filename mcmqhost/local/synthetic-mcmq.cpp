#include "libmcmq/config_reader.h"
#include "libunvme/memory_space.h"
#include "libunvme/nvme_driver.h"
#include "libunvme/pcie_link_mcmq.h"
#include "libunvme/pcie_link_vfio.h"

#include "cxxopts.hpp"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"

#include <chrono>
#include <thread>

using cxxopts::OptionException;

#define NSID_STATS 14
#define NSID_KNN 15
#define NSID_GREP 16

#define BLOCK_SIZE (64 << 10UL)

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

static void do_stats(int bits, const uint8_t* buf, size_t count,
                     __uint128_t& sum, uint64_t& max, uint64_t& min)
{
    int stride = bits >> 3;
    const uint8_t* end = buf + count;

    while (buf < end) {
        uint64_t val;

        if (bits == 32) {
            val = *(const uint32_t*)buf;
        } else {
            val = *(const uint64_t*)buf;
        }

        sum += val;
        max = std::max(max, val);
        min = std::min(min, val);

        buf += stride;
    }
}

static double do_knn(int bits, const uint8_t* buf, size_t count)
{
    int stride = bits >> 3;
    const uint8_t* end = buf + count;
    double sum = 0.0;

    while (buf < end) {
        if (bits == 32) {
            float val = *(const float*)buf;
            sum += val * val;
        } else {
            double val = *(const double*)buf;
            sum += val * val;
        }

        buf += stride;
    }

    return sum;
}

void computeLPSArray(const char* pat, size_t M, int* lps)
{
    size_t len = 0;

    lps[0] = 0;

    size_t i = 1;
    while (i < M) {
        if (pat[i] == pat[len]) {
            len++;
            lps[i] = len;
            i++;
        } else {
            if (len != 0) {
                len = lps[len - 1];
            } else {
                lps[i] = 0;
                i++;
            }
        }
    }
}

bool KMPSearch(const char* txt, size_t N, const char* pat, size_t M, int* lps)
{
    size_t i = 0;
    size_t j = 0;

    while ((N - i) >= (M - j)) {
        if (pat[j] == txt[i]) {
            j++;
            i++;
        }

        if (j == M) {
            return true;
        }

        else if (i < N && pat[j] != txt[i]) {
            if (j != 0)
                j = lps[j - 1];
            else
                i = i + 1;
        }
    }

    return false;
}

int main(int argc, char* argv[])
{
    spdlog::cfg::load_env_levels();

    auto args = parse_arguments(argc, argv);

    std::string backend, workload;
    std::string config_file, result_file;
    size_t data_size;
    int N;
    try {
        backend = args["backend"].as<std::string>();
        config_file = args["config"].as<std::string>();
        workload = args["workload"].as<std::string>();
        result_file = args["result"].as<std::string>();
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

    bool stats_workload = false;
    int stats_bits = 64;

    bool knn_workload = false;
    bool grep_workload = false;

    if (workload == "stats") {
        stats_workload = true;

        try {
            stats_bits = args["bits"].as<int>();
        } catch (const OptionException& e) {
            spdlog::error("Failed to parse options: {}", e.what());
            exit(EXIT_FAILURE);
        }
    }

    if (workload == "knn") {
        knn_workload = true;

        try {
            stats_bits = args["bits"].as<int>();
        } catch (const OptionException& e) {
            spdlog::error("Failed to parse options: {}", e.what());
            exit(EXIT_FAILURE);
        }
    }

    if (workload == "grep") {
        grep_workload = true;
    }

    {
        using std::chrono::duration;
        using std::chrono::high_resolution_clock;

        auto t1 = high_resolution_clock::now();
        auto t2 = t1;

        std::vector<std::thread> threads;

        for (int i = 0; i < N; i++) {
            threads.emplace_back([=, &memory_space, &driver]() {
                auto dev_buf = memory_space->allocate_pages(BLOCK_SIZE);
                auto cpu_buf = std::make_unique<uint8_t[]>(BLOCK_SIZE);

                driver.set_thread_id(i + 1);

                size_t start = i * (data_size / N);
                size_t end = (i + 1) * (data_size / N);
                end = std::min(end, data_size);

                if (stats_workload) {
                    __uint128_t sum = 0;
                    uint64_t min = UINT64_MAX;
                    uint64_t max = 0;

                    for (size_t pos = start; pos < end; pos += BLOCK_SIZE) {
                        size_t chunk =
                            std::min((size_t)BLOCK_SIZE, data_size - pos);

                        if (pos > start && pos % 0x40000000 == 0)
                            spdlog::info("Pos@{}: {:#x}", i, pos);

                        driver.read(NSID_STATS, pos, dev_buf, chunk);
                        memory_space->read(dev_buf, cpu_buf.get(), chunk);

                        do_stats(stats_bits, cpu_buf.get(), chunk, sum, max,
                                 min);
                    }

                    spdlog::info("{} {:#x} {:#x} {:#x}", i, sum, max, min);
                }

                if (knn_workload) {
                    double sum = 0.0;

                    for (size_t pos = start; pos < end; pos += BLOCK_SIZE) {
                        size_t chunk =
                            std::min((size_t)BLOCK_SIZE, data_size - pos);

                        if (pos > start && pos % 0x40000000 == 0)
                            spdlog::info("Pos@{}: {:#x}", i, pos);

                        driver.read(NSID_KNN, pos, dev_buf, chunk);
                        memory_space->read(dev_buf, cpu_buf.get(), chunk);

                        sum += do_knn(stats_bits, cpu_buf.get(), chunk);
                    }

                    spdlog::info("{} {}", i, sum);
                }

                if (grep_workload) {
                    const char pat[] = "TUKOZYPDOBRKBLBUTYAIBHSBWROFWGNR";
                    int M = 32;
                    int lps[32];
                    int matches = 0;

                    computeLPSArray(pat, M, lps);

                    for (size_t pos = start; pos < end; pos += BLOCK_SIZE) {
                        size_t chunk =
                            std::min((size_t)BLOCK_SIZE, data_size - pos);

                        if (pos > start && pos % 0x40000000 == 0)
                            spdlog::info("Pos@{}: {:#x}", i, pos);

                        driver.read(NSID_KNN, pos, dev_buf, chunk);
                        memory_space->read(dev_buf, cpu_buf.get(), chunk);

                        if (KMPSearch((char*)cpu_buf.get(), chunk, pat, M, lps))
                            matches++;
                    }

                    spdlog::info("{} {}", i, matches);
                }

                memory_space->free_pages(dev_buf, BLOCK_SIZE);
            });
        }

        for (auto& p : threads)
            p.join();

        t2 = high_resolution_clock::now();

        duration<double> delta = t2 - t1;
        spdlog::info("Execution time: {}s", delta.count());
    }

    driver.shutdown();
    link->stop();

    return 0;
}
