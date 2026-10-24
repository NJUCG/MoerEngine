#include "VulkanDescriptorBufferAllocator.h"

#include <barrier>
#include <chrono>
#include <iostream>
#include <thread>

using Allocator = Moer::Render::VulkanDescriptorBufferAllocator;
using Clock = std::chrono::steady_clock;

void Run(bool prepared, size_t workers, size_t commands, size_t binds) {
    constexpr size_t iterations = 500;
    Allocator allocator(6 * 1024 * 1024, 64 * 1024, 32, 256);
    const std::vector<std::vector<uint64_t>> requests(commands, std::vector<uint64_t>(binds, 128));
    std::shared_ptr<Allocator::LeaseState> lease;
    std::optional<std::vector<Allocator::Recording>> recordings;
    std::barrier rendezvous(static_cast<std::ptrdiff_t>(workers + 1));
    std::vector<uint64_t> checksums(workers);
    std::vector<std::jthread> threads;
    for (size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            for (size_t iteration = 0; iteration < iterations; ++iteration) {
                rendezvous.arrive_and_wait();
                for (size_t command = worker; command < commands; command += workers) {
                    Allocator::Cursor cursor;
                    if (prepared) cursor = Allocator::Cursor((*recordings)[command]);
                    for (size_t bind = 0; bind < binds; ++bind) {
                        auto offset = prepared ? cursor.Allocate(128) : allocator.ReserveRange(*lease, 128);
                        if (!offset) std::terminate();
                        checksums[worker] += *offset;
                    }
                }
                rendezvous.arrive_and_wait();
            }
        });
    }
    double prepare_ms = 0, record_ms = 0, retire_ms = 0;
    uint64_t reserved_bytes = 0;
    auto milliseconds = [](auto start, auto end) {
        return std::chrono::duration<double, std::milli>(end - start).count();
    };
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto start = Clock::now();
        lease = allocator.CreateLease();
        if (prepared) {
            recordings = allocator.PrepareRecordings(*lease, requests);
            if (!recordings) std::terminate();
        }
        const auto ready = Clock::now();
        rendezvous.arrive_and_wait();
        rendezvous.arrive_and_wait();
        const auto recorded = Clock::now();
        reserved_bytes = allocator.GetUsage(*lease).reserved_bytes;
        allocator.Release(*lease);
        recordings.reset();
        const auto retired = Clock::now();
        prepare_ms += milliseconds(start, ready);
        record_ms += milliseconds(ready, recorded);
        retire_ms += milliseconds(recorded, retired);
    }
    threads.clear();
    uint64_t checksum = 0;
    for (auto value : checksums) checksum += value;
    std::cout << "{\"prepared\":" << (prepared ? "true" : "false")
              << ",\"workers\":" << workers << ",\"commands\":" << commands
              << ",\"binds_per_command\":" << binds << ",\"iterations\":" << iterations
              << ",\"prepare_ms\":" << prepare_ms << ",\"record_ms\":" << record_ms
              << ",\"retire_ms\":" << retire_ms
              << ",\"total_ms\":" << prepare_ms + record_ms + retire_ms
              << ",\"reserved_bytes\":" << reserved_bytes << ",\"checksum\":" << checksum << "}\n";
}

int main() {
    for (size_t workers : {1u, 4u}) {
        for (size_t binds : {4u, 64u}) {
            Run(false, workers, 256, binds);
            Run(true, workers, 256, binds);
        }
    }
}
