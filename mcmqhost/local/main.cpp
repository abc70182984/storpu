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

#include <thread>

#include <iomanip>

#include <libtest_symbols.h>

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

    // auto buffer = memory_space->allocate_pages(0x8000);

    driver.set_thread_id(1);
    unsigned long a = driver.invoke_function(ctx, ENTRY_pgtest_scan, 0);
    driver.delete_context(ctx);

    // unsigned long val;
    // memory_space->read(buffer + 0x1000, &val, 8);
    // spdlog::info("Invoke {:#x} {:#x}", a, val);
    // memory_space->read(buffer + 0x2000, &val, 8);
    // spdlog::info("Invoke {:#x} {:#x}", a, val);

    // memory_space->free_pages(buffer, 0x8000);

    // driver.delete_context(ctx);

    // auto nsid = driver.create_namespace(4UL << 32);
    // spdlog::info("Create NS {}", nsid);
    // driver.attach_namespace(nsid);
    // driver.detach_namespace(nsid);
    // driver.delete_namespace(nsid);

    // driver.set_thread_id(1);

    // auto write_buf = memory_space->allocate_pages(0x4000);
    // auto read_buf = memory_space->allocate_pages(0x4000);
    // auto* cpu_buffer = new unsigned char[0x4000];

    // for (int i = 0; i < 0x4000; i++) {
    //     cpu_buffer[i] = i & 0xff;
    // }

    // memory_space->write(write_buf, cpu_buffer, 0x4000);

    // driver.write(1, 0, write_buf, 0x4000);

    // memset(cpu_buffer, 0, 0x4000);

    // auto read_buf = memory_space->allocate_pages(0x4000);
    // auto* cpu_buffer = new unsigned char[0x4000];
    // driver.read(1, 0, read_buf, 0x4000);
    // memory_space->read(read_buf, cpu_buffer, 0x4000);

    // for (int i = 0; i < 0x4000; i += 32) {
    //     for (int j = 0; j < 32; j++) {
    //         std::cout << std::setw(2) << std::hex
    //                   << (unsigned int)cpu_buffer[i + j] << " ";
    //     }
    //     std::cout << std::endl;
    // }

    driver.shutdown();
    link->stop();

    return 0;
}
