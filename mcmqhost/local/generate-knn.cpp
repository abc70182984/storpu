#include "cxxopts.hpp"
#include "spdlog/cfg/env.h"
#include "spdlog/spdlog.h"

#include <cstdlib>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#define BLOCK_SIZE 65536

using cxxopts::OptionException;

cxxopts::ParseResult parse_arguments(int argc, char* argv[])
{
    try {
        cxxopts::Options options(argv[0], " - Host frontend for MCMQ");

        // clang-format off
        options.add_options()
            ("o,output", "Path to the output file", cxxopts::value<std::string>()->default_value("knn.bin"))
            ("s,datasize", "Data size", cxxopts::value<size_t>())
            ("N,worker", "Number of worker threads", cxxopts::value<int>()->default_value("8"))
            ("B,bits", "Data width", cxxopts::value<int>()->default_value("64"))
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

void generate_buf(int bits, uint8_t* buf, size_t count)
{
    int stride = bits >> 3;
    const uint8_t* end = buf + count;

    while (buf < end) {
        int r = rand();

        if (bits == 32) {
            *(float*)buf = (float)r / (float)RAND_MAX;
        } else {
            *(double*)buf = (double)r / (double)RAND_MAX;
        }

        buf += stride;
    }
}

int main(int argc, char* argv[])
{
    spdlog::cfg::load_env_levels();
    srand(time(NULL));

    auto args = parse_arguments(argc, argv);

    std::string output_file;
    size_t data_size;
    int bits;
    int N;
    try {
        output_file = args["output"].as<std::string>();
        data_size = args["datasize"].as<size_t>();
        N = args["worker"].as<int>();
        bits = args["bits"].as<int>();
    } catch (const OptionException& e) {
        spdlog::error("Failed to parse options: {}", e.what());
        exit(EXIT_FAILURE);
    }

    int fd = open(output_file.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    ftruncate(fd, data_size);

    std::vector<std::thread> threads;

    for (int i = 0; i < N; i++) {
        threads.emplace_back([=]() {
            auto buf = std::make_unique<uint8_t[]>(BLOCK_SIZE);

            size_t start = i * (data_size / N);
            size_t end = (i + 1) * (data_size / N);
            end = std::min(end, data_size);

            for (size_t pos = start; pos < end; pos += BLOCK_SIZE) {
                size_t chunk = std::min((size_t)BLOCK_SIZE, data_size - pos);

                generate_buf(bits, buf.get(), chunk);
                pwrite(fd, buf.get(), chunk, pos);
            }
        });
    }

    for (auto& p : threads)
        p.join();

    close(fd);

    return 0;
}
