#include "VulkanDescriptorBufferAllocator.h"
#include "legacy.h"
#include "misc/Types.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

using Allocator = Moer::Render::VulkanDescriptorBufferAllocator;
constexpr Moer::uint64 KiB = 1024, MiB = 1024 * KiB;
using Clock = std::chrono::steady_clock;

void Check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

void CapacityCases() {
    Allocator allocator(6 * MiB, 64 * KiB, 32);
    auto lease = allocator.CreateLease();
    Check(allocator.ReserveRange(*lease, 0).has_value(), "zero failed");
    std::cout << "capacity,empty_reserved,1048576," << allocator.GetUsage(*lease).reserved_bytes << '\n';
    Check(allocator.ReserveRange(*lease, 32).has_value(), "small failed");
    std::cout << "capacity,small_reserved,1048576," << allocator.GetUsage(*lease).reserved_bytes << '\n';
    allocator.Release(*lease);
    lease = allocator.CreateLease();
    LegacyLease old;
    std::cout << "capacity,single_2MiB_success," << LegacyReserve(old, 2 * MiB).has_value()
              << ',' << allocator.ReserveRange(*lease, 2 * MiB).has_value() << '\n';
    allocator.Release(*lease);

    std::vector<std::shared_ptr<Allocator::LeaseState>> small;
    for (int i = 0; i < 96; ++i) {
        auto item = allocator.CreateLease();
        Check(allocator.ReserveRange(*item, 32).has_value(), "small lease capacity incorrect");
        small.push_back(item);
    }
    // Old 3 graphics + 3 compute slots. More slots would wait, not fail.
    std::cout << "capacity,nonwaiting_small_leases,6," << small.size() << '\n';
    for (auto& item : small) allocator.Release(*item);

    // Each of the six old 1MiB slots has sufficient space for this trace.
    // Deliberately larger bind layouts expose the block-tail tradeoff.
    std::array<LegacyLease, 6> old_slots;
    std::array<std::shared_ptr<Allocator::LeaseState>, 6> leases;
    Moer::uint64                                          used = 0, reserved = 0;
    for (size_t i = 0; i < leases.size(); ++i) {
        leases[i] = allocator.CreateLease();
        for (int j = 0; j < 16; ++j) {
            Check(LegacyReserve(old_slots[i], 40 * KiB).has_value(), "legacy prefix failed");
            Check(allocator.ReserveRange(*leases[i], 40 * KiB).has_value(), "new prefix failed");
        }
        used += allocator.GetUsage(*leases[i]).used_bytes;
        reserved += allocator.GetUsage(*leases[i]).reserved_bytes;
    }
    std::cout << "capacity,fragmented_prefix_used," << used << ',' << used << '\n';
    std::cout << "capacity,fragmented_prefix_reserved,6291456," << reserved << '\n';
    const auto old_append = LegacyReserve(old_slots[0], 32 * KiB);
    const auto new_append = allocator.ReserveRange(*leases[0], 32 * KiB);
    std::cout << "capacity,fragmented_append_success," << old_append.has_value()
              << ',' << new_append.has_value() << '\n';
    Check(old_append.has_value(), "legacy counterexample is invalid");
    // Observation, not an assertion requiring the new implementation to fail.
    for (auto& item : leases) allocator.Release(*item);
    // A small 288-byte layout exhibits the same issue near full utilization:
    // 227 binds fit each block, stranding 160 bytes in every block.
    for (size_t i = 0; i < leases.size(); ++i) {
        old_slots[i].next_byte_offset.store(0);
        leases[i] = allocator.CreateLease();
        for (int j = 0; j < 16 * 227; ++j) {
            Check(LegacyReserve(old_slots[i], 288).has_value(), "legacy small-layout prefix failed");
            Check(allocator.ReserveRange(*leases[i], 288).has_value(), "new small-layout prefix failed");
        }
    }
    std::cout << "capacity,small_layout_prefix_used,6276096,6276096\n";
    std::cout << "capacity,small_layout_append_success,"
              << LegacyReserve(old_slots[0], 288).has_value() << ','
              << allocator.ReserveRange(*leases[0], 288).has_value() << '\n';
    for (auto& item : leases) allocator.Release(*item);
    lease = allocator.CreateLease();
    Check(allocator.ReserveRange(*lease, 6 * MiB) == 0, "capacity trace leaked blocks");
    allocator.Release(*lease);
}

double Measure(bool current, unsigned threads, Moer::uint64 bytes, unsigned count, Moer::uint64& checksum) {
    Allocator allocator(6 * MiB, 64 * KiB, 32);
    auto lease = allocator.CreateLease();
    LegacyLease old;
    std::barrier ready(static_cast<std::ptrdiff_t>(threads + 1));
    std::barrier start(static_cast<std::ptrdiff_t>(threads + 1));
    std::barrier done(static_cast<std::ptrdiff_t>(threads + 1));
    std::vector<Moer::uint64> sums(threads);
    std::vector<unsigned> failures(threads);
    std::vector<std::jthread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            ready.arrive_and_wait();
            start.arrive_and_wait();
            Moer::uint64 sum    = 0;
            unsigned failed = 0;
            for (unsigned i = 0; i < count / threads; ++i) {
                const auto offset = current ? allocator.ReserveRange(*lease, bytes) : LegacyReserve(old, bytes);
                if (offset) sum += *offset;
                else ++failed;
            }
            sums[t] = sum;
            failures[t] = failed;
            done.arrive_and_wait();
        });
    }
    ready.arrive_and_wait(); // Thread creation excluded from timing.
    const auto begin = Clock::now();
    start.arrive_and_wait();
    done.arrive_and_wait();
    const auto end = Clock::now();
    workers.clear();
    for (unsigned t = 0; t < threads; ++t) {
        Check(failures[t] == 0, "timed allocation failed: incomparable workload");
        checksum += sums[t];
    }
    const Moer::uint64 actual = current ? allocator.GetUsage(*lease).used_bytes : old.next_byte_offset.load();
    Check(actual == bytes * count, "accounting mismatch in timed workload");
    allocator.Release(*lease);
    return std::chrono::duration<double, std::nano>(end - begin).count() / count;
}

int main() {
    try {
        CapacityCases();
        std::cout << "kind,implementation,threads,bytes,operations,sample,ns_per_op\n";
        std::mt19937 random(42);
        Moer::uint64 checksum = 0;
        for (Moer::uint64 bytes : {0ull, 32ull, 256ull}) {
            const unsigned count = bytes == 256 ? 4096 : 16384;
            for (unsigned threads : {1u, 2u, 4u, 8u}) {
                for (int sample = -3; sample < 40; ++sample) {
                    const bool first = (random() & 1) != 0;
                    for (bool current : {first, !first}) {
                        const double ns = Measure(current, threads, bytes, count, checksum);
                        if (sample >= 0) std::cout << "timing," << (current ? "current" : "legacy")
                            << ',' << threads << ',' << bytes << ',' << count << ',' << sample << ',' << ns << '\n';
                    }
                }
            }
        }
        std::cerr << "checksum=" << checksum << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
