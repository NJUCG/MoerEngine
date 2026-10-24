#include "VulkanDescriptorBufferAllocator.h"
#include "vma_candidate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>

using Current = Moer::Render::VulkanDescriptorBufferAllocator;
using Vma = VmaDescriptorCandidate;
using Clock = std::chrono::steady_clock;
constexpr uint64_t KiB = 1024, MiB = 1024 * KiB, Capacity = 6 * MiB;

void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class F> void Reject(F operation) {
    bool rejected = false;
    try { operation(); } catch (const std::logic_error&) { rejected = true; }
    Check(rejected, "invalid operation was accepted");
}
template<class A> std::unique_ptr<A> Make(uint64_t atom = 32) {
    if constexpr (std::is_same_v<A, Current>) return std::make_unique<A>(Capacity, 64 * KiB, 32);
    else return std::make_unique<A>(Capacity, 32, atom);
}
template<class A> void FullRecovery(A& allocator) {
    auto full = allocator.CreateLease();
    Check(allocator.ReserveRange(*full, Capacity) == 0, "pool did not recover full capacity");
    Check(!allocator.ReserveRange(*full, 32), "pool overflow accepted");
    allocator.Release(*full);
}

template<class A> void Contracts() {
    auto allocator = Make<A>();
    auto lease = allocator->CreateLease();
    auto retained = lease;
    lease.reset(); // Shared ownership handoff must not reclaim the allocation.
    Check(allocator->ReserveRange(*retained, 0) == 0, "zero reserve");
    Check(allocator->GetUsage(*retained).reserved_bytes == 0, "zero consumes capacity");
    Check(allocator->ReserveRange(*retained, 33) == 0, "unaligned reserve");
    const auto checkpoint = allocator->CreateCheckpoint(*retained);
    const auto prefix = allocator->GetUsage(*retained);
    auto other_allocator = Make<A>();
    Reject([&] { other_allocator->ReserveRange(*retained, 32); });
    auto other = allocator->CreateLease();
    const auto foreign_checkpoint = allocator->CreateCheckpoint(*other);
    Reject([&] { allocator->Rewind(*retained, foreign_checkpoint); });
    Check(allocator->ReserveRange(*retained, 96 * KiB).has_value(), "checkpoint suffix reserve");
    Check(allocator->ReserveRange(*other, 32).has_value(), "interleaved reserve");
    const auto before_failure = allocator->GetUsage(*retained);
    Check(!allocator->ReserveRange(*retained, std::numeric_limits<uint64_t>::max()), "size overflow");
    Check(!allocator->ReserveRange(*retained, Capacity), "space exhaustion");
    auto after_failure = allocator->GetUsage(*retained);
    Check(before_failure.used_bytes == after_failure.used_bytes &&
          before_failure.reserved_bytes == after_failure.reserved_bytes, "failure changed occupancy");
    allocator->Rewind(*retained, checkpoint); // Caller has joined/discarded suffix.
    const auto restored = allocator->GetUsage(*retained);
    Check(restored.used_bytes == prefix.used_bytes && restored.reserved_bytes == prefix.reserved_bytes,
          "rewind changed prefix");
    const auto ranges = allocator->GetWrittenRanges(*retained);
    Check(ranges.size() == 1 && ranges[0].byte_offset == 0 && ranges[0].byte_size == 64, "written prefix");
    Check(allocator->GetUsage(*other).used_bytes == 32, "rewind reclaimed another lease");
    allocator->Release(*other); // Later submission finishes first.
    allocator->Release(*retained);
    Reject([&] { allocator->ReserveRange(*retained, 0); });
    Reject([&] { allocator->Release(*retained); });
    FullRecovery(*allocator);
}

// An independent byte-interval oracle, unrelated to either allocator's free list.
template<class A> void RandomizedLifetimes() {
    auto allocator = Make<A>();
    using Lease = std::shared_ptr<typename A::LeaseState>;
    struct Live { Lease lease; std::vector<std::pair<uint64_t, uint64_t>> spans; };
    std::array<Live, 16> live;
    std::vector<unsigned> occupied(Capacity / 32);
    std::mt19937 rng(20261023);
    auto retire = [&](unsigned index) {
        auto& item = live[index];
        if (!item.lease) return;
        for (auto [offset, size] : item.spans) for (uint64_t cell = offset / 32; cell < (offset + size) / 32; ++cell) {
            Check(occupied[cell] == index + 1, "random lifetime owner mismatch");
            occupied[cell] = 0;
        }
        allocator->Release(*item.lease);
        item = {};
    };
    for (unsigned step = 0; step < 20000; ++step) {
        const unsigned index = rng() % live.size();
        auto& item = live[index];
        if (rng() % 7 == 0) { retire(index); continue; }
        if (!item.lease) item.lease = allocator->CreateLease();
        const uint64_t size = 1 + rng() % 3072, aligned = (size + 31) / 32 * 32;
        const auto offset = allocator->ReserveRange(*item.lease, size);
        Check(offset.has_value(), "random workload unexpectedly exhausted capacity");
        Check(*offset % 32 == 0 && *offset + aligned <= Capacity, "random range bounds/alignment");
        for (uint64_t cell = *offset / 32; cell < (*offset + aligned) / 32; ++cell) {
            Check(occupied[cell] == 0, "random lifetime overlap");
            occupied[cell] = index + 1;
        }
        item.spans.emplace_back(*offset, aligned);
    }
    for (unsigned i = 0; i < live.size(); ++i) retire(i);
    FullRecovery(*allocator);
}

template<class A> void ConcurrentLifetimes(bool shared) {
    auto allocator = Make<A>();
    auto common = allocator->CreateLease();
    std::vector<std::atomic<unsigned>> occupied(Capacity / 32);
    for (auto& cell : occupied) cell.store(0);
    std::atomic<unsigned> errors{0};
    std::barrier start(8);
    std::vector<std::jthread> workers;
    for (unsigned t = 0; t < 8; ++t) workers.emplace_back([&, t] {
        start.arrive_and_wait();
        const unsigned rounds = shared ? 1 : 300, count = shared ? 2048 : 32;
        for (unsigned round = 0; round < rounds; ++round) {
            auto lease = shared ? common : allocator->CreateLease();
            std::vector<uint64_t> offsets;
            for (unsigned i = 0; i < count; ++i) {
                const auto offset = allocator->ReserveRange(*lease, 32);
                if (!offset || *offset % 32 || *offset + 32 > Capacity) { ++errors; continue; }
                unsigned expected = 0;
                if (!occupied[*offset / 32].compare_exchange_strong(expected, t + 1)) ++errors;
                offsets.push_back(*offset);
            }
            if (!shared) {
                // Clear oracle ownership before publishing the free space.
                for (auto offset : offsets) {
                    if (occupied[offset / 32].exchange(0) != t + 1) ++errors;
                }
                allocator->Release(*lease);
            }
        }
    });
    workers.clear();
    Check(errors.load() == 0, "concurrent overlap/bounds/retirement");
    if (shared) {
        uint64_t cells = 0;
        for (auto& cell : occupied) cells += cell.load() != 0;
        Check(cells == 8 * 2048, "shared recordings lost allocations");
        Check(allocator->GetUsage(*common).used_bytes == cells * 32, "concurrent accounting");
    }
    allocator->Release(*common);
    FullRecovery(*allocator);
}

void AtomIsolation() {
    auto allocator = Make<Vma>(256);
    auto a = allocator->CreateLease(), b = allocator->CreateLease();
    std::vector<unsigned> atoms(Capacity / 256);
    for (unsigned i = 0; i < 2000; ++i) {
        const bool choose_a = i % 2 == 0;
        const uint64_t bytes = i % 3 ? 32 : 288;
        auto& lease = choose_a ? a : b;
        const auto offset = allocator->ReserveRange(*lease, bytes);
        Check(offset && *offset % 256 == 0, "atom alignment");
        const uint64_t reserved = (bytes + 255) / 256 * 256;
        for (uint64_t cell = *offset / 256; cell < (*offset + reserved) / 256; ++cell) {
            Check(atoms[cell] == 0, "independent leases share a memory atom");
            atoms[cell] = choose_a ? 1 : 2;
        }
    }
    for (auto [lease, owner] : {std::pair{a, 1u}, std::pair{b, 2u}}) {
        for (const auto& range : allocator->GetWrittenRanges(*lease))
            for (uint64_t cell = range.byte_offset / 256; cell < (range.byte_offset + range.byte_size + 255) / 256; ++cell)
                Check(atoms[cell] == owner, "flush touches another lease's atom");
    }
    Check(allocator->GetAllocatedBytes() == allocator->GetUsage(*a).reserved_bytes +
          allocator->GetUsage(*b).reserved_bytes, "VMA allocation statistics disagree");
    allocator->Release(*b);
    allocator->Release(*a);
    Check(allocator->GetAllocatedBytes() == 0, "VMA leaked after retirement");
    FullRecovery(*allocator);
}

template<class A> void CapacityCases(std::string_view name, uint64_t atom = 32) {
    auto allocator = Make<A>(atom);
    auto small_lease = allocator->CreateLease();
    allocator->ReserveRange(*small_lease, 32);
    auto usage = allocator->GetUsage(*small_lease);
    std::cout << "capacity," << name << ",small_reserved,1," << usage.used_bytes << ',' << usage.reserved_bytes << '\n';
    allocator->Release(*small_lease);
    for (bool large : {true, false}) {
        std::array<std::shared_ptr<typename A::LeaseState>, 6> leases;
        uint64_t used = 0, reserved = 0;
        bool prefix = true;
        for (auto& lease : leases) lease = allocator->CreateLease();
        // Interleave submissions to exercise sharing rather than contiguous ownership.
        for (unsigned i = 0; i < (large ? 16u : 3632u) && prefix; ++i)
            for (auto& lease : leases)
                if (!allocator->ReserveRange(*lease, large ? 40 * KiB : 288)) { prefix = false; break; }
        for (auto& lease : leases) {
            usage = allocator->GetUsage(*lease);
            used += usage.used_bytes;
            reserved += usage.reserved_bytes;
        }
        const bool success = prefix && allocator->ReserveRange(*leases[0], large ? 32 * KiB : 288).has_value();
        std::cout << "capacity," << name << ',' << (large ? "40KiB_append" : "288B_append") << ','
                  << success << ',' << used << ',' << reserved << '\n';
        if constexpr (std::is_same_v<A, Vma>) {
            if (atom == 32 || large) Check(success, "VMA failed known capacity counterexample");
            else Check(!prefix, "atom padding capacity expectation changed");
        }
        for (auto& lease : leases) allocator->Release(*lease);
        FullRecovery(*allocator);
    }
    // Total free bytes do not imply a large contiguous allocation is possible.
    std::array<std::shared_ptr<typename A::LeaseState>, 6> chunks;
    for (auto& chunk : chunks) {
        chunk = allocator->CreateLease();
        Check(allocator->ReserveRange(*chunk, MiB).has_value(), "fragmentation setup");
    }
    for (unsigned i : {0u, 2u, 4u}) allocator->Release(*chunks[i]);
    auto request = allocator->CreateLease();
    Check(!allocator->ReserveRange(*request, 2 * MiB), "external fragmentation expectation changed");
    std::cout << "capacity," << name << ",external_fragmentation_2MiB,0,3145728,3145728\n";
    for (unsigned i : {1u, 3u, 5u}) allocator->Release(*chunks[i]);
    allocator->Release(*request);
    FullRecovery(*allocator);
}

struct Times { double reserve, snapshot, retire; uint64_t ranges; };
template<class A> Times Measure(unsigned threads, uint64_t bytes, bool shared, unsigned count, uint64_t& checksum) {
    auto allocator = Make<A>();
    std::vector<std::shared_ptr<typename A::LeaseState>> leases;
    for (unsigned i = 0; i < (shared ? 1u : threads); ++i) leases.push_back(allocator->CreateLease());
    std::barrier ready(static_cast<std::ptrdiff_t>(threads + 1)), start(static_cast<std::ptrdiff_t>(threads + 1)),
                 done(static_cast<std::ptrdiff_t>(threads + 1));
    std::vector<uint64_t> sums(threads);
    std::vector<unsigned> failures(threads);
    std::vector<std::jthread> workers;
    for (unsigned t = 0; t < threads; ++t) workers.emplace_back([&, t] {
        auto& lease = *leases[shared ? 0 : t];
        ready.arrive_and_wait();
        start.arrive_and_wait();
        uint64_t sum = 0;
        unsigned failed = 0;
        for (unsigned i = 0; i < count / threads; ++i) {
            const auto offset = allocator->ReserveRange(lease, bytes);
            if (offset) sum += *offset; else ++failed;
        }
        sums[t] = sum;
        failures[t] = failed;
        done.arrive_and_wait();
    });
    ready.arrive_and_wait();
    const auto begin = Clock::now();
    start.arrive_and_wait();
    done.arrive_and_wait();
    const auto end = Clock::now();
    workers.clear();
    for (unsigned t = 0; t < threads; ++t) { Check(!failures[t], "timed allocation failed"); checksum += sums[t]; }
    uint64_t used = 0;
    for (auto& lease : leases) used += allocator->GetUsage(*lease).used_bytes;
    Check(used == bytes * count, "timed allocation accounting");
    // Snapshot/free costs are separate from the reservation hot path. This
    // also exposes the per-binding bookkeeping and flush-range consequences.
    uint64_t range_count = 0, written = 0;
    const auto snapshot_begin = Clock::now();
    for (auto& lease : leases) {
        const auto ranges = allocator->GetWrittenRanges(*lease);
        range_count += ranges.size();
        for (const auto& range : ranges) written += range.byte_size;
    }
    const auto snapshot_end = Clock::now();
    Check(written == used, "written-range accounting");
    const auto retire_begin = Clock::now();
    for (auto& lease : leases) allocator->Release(*lease);
    const auto retire_end = Clock::now();
    return {std::chrono::duration<double, std::nano>(end - begin).count() / count,
            std::chrono::duration<double, std::nano>(snapshot_end - snapshot_begin).count() / count,
            std::chrono::duration<double, std::nano>(retire_end - retire_begin).count() / count, range_count};
}

int main(int argc, char** argv) {
    try {
        const bool benchmark = argc == 2 && std::string_view(argv[1]) == "--bench";
        Check(argc == 1 || (argc == 2 && (benchmark || std::string_view(argv[1]) == "--test")), "use --test or --bench");
        if (!benchmark) {
            Contracts<Current>(); Contracts<Vma>();
            RandomizedLifetimes<Current>(); RandomizedLifetimes<Vma>();
            for (bool shared : {true, false}) { ConcurrentLifetimes<Current>(shared); ConcurrentLifetimes<Vma>(shared); }
            AtomIsolation();
            CapacityCases<Current>("current"); CapacityCases<Vma>("vma"); CapacityCases<Vma>("vma_atom256", 256);
            std::cout << "PASS: ownership, overflow/exhaustion, rollback, out-of-order retirement, random intervals, "
                         "concurrent recording/retirement, atom isolation, full capacity recovery\n";
            return 0;
        }
        std::cout << "kind,implementation,mode,threads,bytes,operations,sample,ns_per_op,snapshot_ns_per_op,retire_ns_per_op,ranges\n";
        std::mt19937 random(42);
        uint64_t checksum = 0;
        for (bool shared : {true, false}) for (uint64_t bytes : {0ull, 32ull, 256ull})
            for (unsigned threads : {1u, 2u, 4u, 8u}) {
                if (!shared && threads == 1) continue; // Identical to one shared lease.
                const unsigned count = bytes == 256 ? 4096 : 16384;
                for (int sample = -3; sample < 40; ++sample) {
                    const bool first = (random() & 1) != 0;
                    for (bool vma : {first, !first}) {
                        auto time = vma ? Measure<Vma>(threads, bytes, shared, count, checksum) :
                                          Measure<Current>(threads, bytes, shared, count, checksum);
                        if (sample >= 0) std::cout << "timing," << (vma ? "vma" : "current") << ','
                            << (shared ? "shared" : "independent") << ',' << threads << ',' << bytes << ','
                            << count << ',' << sample << ',' << time.reserve << ',' << time.snapshot << ','
                            << time.retire << ',' << time.ranges << '\n';
                    }
                }
            }
        std::cerr << "checksum=" << checksum << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
