#include "rhi/vulkan/VulkanDescriptorBufferAllocator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>

namespace {
using Allocator        = Moer::Render::VulkanDescriptorBufferAllocator;
constexpr uint64_t KiB = 1024;
constexpr uint64_t MiB = 1024 * KiB;

void Check(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

template<typename F>
void CheckRejected(F&& _operation) {
    bool rejected = false;
    try {
        _operation();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    Check(rejected, "invalid lease operation was accepted");
}

void CheckLazyAllocationAndRetirement() {
    Allocator                                           allocator(6 * MiB, 64 * KiB, 32);
    std::vector<std::shared_ptr<Allocator::LeaseState>> leases;
    for (size_t i = 0; i < 20; ++i) {
        auto lease = allocator.CreateLease();
        Check(allocator.ReserveRange(*lease, 0) == 0, "empty reservation failed");
        Check(allocator.GetUsage(*lease).reserved_bytes == 0, "empty lease occupied storage");
        Check(allocator.ReserveRange(*lease, 48) == i * 64 * KiB, "small leases overlap or hit a slot limit");
        Check(allocator.GetUsage(*lease).used_bytes == 64, "allocation alignment was not applied");
        leases.push_back(std::move(lease));
    }
    allocator.Release(*leases[7]);
    CheckRejected([&] {
        allocator.ReserveRange(*leases[7], 0);
    });
    CheckRejected([&] {
        allocator.Release(*leases[7]);
    });
    auto replacement = allocator.CreateLease();
    Check(allocator.ReserveRange(*replacement, 16) == 7 * 64 * KiB, "completed region was not reused");
    allocator.Release(*replacement);
    for (size_t i = 0; i < leases.size(); ++i) {
        if (i != 7) {
            allocator.Release(*leases[i]);
        }
    }
    auto large = allocator.CreateLease();
    Check(allocator.ReserveRange(*large, 6 * MiB) == 0, "out-of-order retirement did not coalesce storage");
    Check(!allocator.ReserveRange(*large, 1), "exhausted buffer accepted another allocation");
    allocator.Release(*large);
}

void CheckFragmentationAndFailurePreserveState() {
    Allocator                                             allocator(4 * 64 * KiB, 64 * KiB, 16);
    std::array<std::shared_ptr<Allocator::LeaseState>, 4> leases;
    for (auto& lease : leases) {
        lease = allocator.CreateLease();
        Check(allocator.ReserveRange(*lease, 64 * KiB).has_value(), "initial block allocation failed");
    }
    allocator.Release(*leases[0]);
    allocator.Release(*leases[2]);
    auto large = allocator.CreateLease();
    Check(!allocator.ReserveRange(*large, 128 * KiB), "fragmented storage was treated as contiguous");
    Check(!allocator.ReserveRange(*large, std::numeric_limits<uint64_t>::max()), "size overflow accepted");
    Check(allocator.GetUsage(*large).reserved_bytes == 0, "failed allocation changed lease state");
    allocator.Release(*leases[1]);
    Check(allocator.ReserveRange(*large, 128 * KiB) == 0, "adjacent free blocks were not coalesced");
    allocator.Release(*leases[3]);
    allocator.Release(*large);
}

void CheckSharedLeaseHandoff() {
    Allocator allocator(64 * KiB, 64 * KiB, 32);
    Moer::Render::VulkanDescriptorBufferLease recording;
    Check(!recording, "default lease is not empty");
    recording = allocator.CreateLease();
    auto recorder = recording;
    Check(allocator.ReserveRange(*recorder, 32) == 0, "recorder allocation failed");

    auto completion = std::move(recording);
    Check(!recording, "handoff retained the local retirement handle");
    auto waiting = allocator.CreateLease();
    Check(!allocator.ReserveRange(*waiting, 32), "handoff released storage before completion");

    allocator.Release(*completion);
    completion.reset();
    CheckRejected([&] { allocator.ReserveRange(*recorder, 32); });
    Check(allocator.ReserveRange(*waiting, 32) == 0, "completed storage was not reused");
    allocator.Release(*waiting);
}

void CheckCheckpointAndWrittenRanges() {
    Allocator  allocator(6 * MiB, 64 * KiB, 32);
    auto       lease            = allocator.CreateLease();
    const auto empty_checkpoint = allocator.CreateCheckpoint(*lease);
    Check(allocator.ReserveRange(*lease, 32) == 0, "prefix allocation failed");
    auto other = allocator.CreateLease();
    Check(allocator.ReserveRange(*other, 32) == 64 * KiB, "interleaved lease allocation failed");
    const auto checkpoint = allocator.CreateCheckpoint(*lease);
    Check(allocator.ReserveRange(*lease, 32) == 32, "existing block was not reused");
    Check(allocator.ReserveRange(*lease, 2 * MiB) == 128 * KiB, "large reservation failed");
    auto ranges = allocator.GetWrittenRanges(*lease);
    Check(
        ranges.size() == 2 && ranges[0].byte_size == 64 && ranges[1].byte_offset == 128 * KiB,
        "written ranges included another lease or lost a block"
    );
    Check(
        allocator.GetUsage(*lease).used_bytes == 2 * MiB + 64,
        "usage was computed from physical offsets instead of allocated bytes"
    );
    CheckRejected([&] {
        allocator.Rewind(*other, checkpoint);
    });
    allocator.Rewind(*lease, checkpoint);
    Check(
        allocator.GetUsage(*lease).used_bytes == 32 && allocator.GetUsage(*lease).reserved_bytes == 64 * KiB,
        "rollback did not restore the prefix"
    );
    Check(allocator.ReserveRange(*lease, 32) == 32, "fallback could not reuse the last block tail");
    allocator.Rewind(*lease, empty_checkpoint);
    Check(allocator.GetWrittenRanges(*lease).empty(), "empty rollback retained blocks");
    allocator.Release(*lease);
    allocator.Release(*other);
    auto full = allocator.CreateLease();
    Check(allocator.ReserveRange(*full, 6 * MiB) == 0, "rollback leaked storage");
    allocator.Release(*full);
}

void CheckConcurrentRecording() {
    Allocator                                       allocator(6 * MiB, 64 * KiB, 32);
    auto                                            lease        = allocator.CreateLease();
    const auto                                      checkpoint   = allocator.CreateCheckpoint(*lease);
    constexpr size_t                                worker_count = 8;
    constexpr size_t                                allocations_per_worker = 2048;
    std::array<std::vector<uint64_t>, worker_count> offsets;
    std::array<std::exception_ptr, worker_count>    errors{};
    std::vector<std::jthread>                       workers;
    std::vector<uint8_t>                            mapped_bytes(6 * MiB);
    for (size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                for (size_t i = 0; i < allocations_per_worker; ++i) {
                    const auto offset = allocator.ReserveRange(*lease, 32);
                    Check(offset.has_value(), "parallel recording ran out of space unexpectedly");
                    offsets[worker].push_back(*offset);
                    std::fill_n(mapped_bytes.data() + *offset, 32, static_cast<uint8_t>(worker + 1));
                }
            } catch (...) {
                errors[worker] = std::current_exception();
            }
        });
    }
    workers.clear(); // Join before flushing, inspecting payloads, or rewinding.
    std::vector<uint64_t> all_offsets;
    for (size_t worker = 0; worker < worker_count; ++worker) {
        if (errors[worker]) {
            std::rethrow_exception(errors[worker]);
        }
        for (uint64_t offset : offsets[worker]) {
            Check(
                std::all_of(
                    mapped_bytes.begin() + offset,
                    mapped_bytes.begin() + offset + 32,
                    [=](uint8_t value) {
                        return value == worker + 1;
                    }
                ),
                "parallel recorders overwrote descriptor bytes"
            );
            all_offsets.push_back(offset);
        }
    }
    std::sort(all_offsets.begin(), all_offsets.end());
    Check(
        std::adjacent_find(all_offsets.begin(), all_offsets.end()) == all_offsets.end(),
        "parallel allocations overlapped"
    );
    Check(
        allocator.GetUsage(*lease).used_bytes == worker_count * allocations_per_worker * 32,
        "parallel allocation accounting mismatch"
    );
    allocator.Rewind(*lease, checkpoint);
    Check(allocator.ReserveRange(*lease, 6 * MiB) == 0, "parallel rollback leaked blocks");
    allocator.Release(*lease);
}

void CheckRandomizedLifetimes() {
    constexpr uint64_t           block_size  = 256;
    constexpr size_t             block_count = 64;
    Allocator                    allocator(block_count * block_size, block_size, 16);
    std::array<int, block_count> owners;
    owners.fill(-1);
    std::array<std::shared_ptr<Allocator::LeaseState>, 12> leases;
    std::mt19937                                           random(42);
    for (size_t iteration = 0; iteration < 4000; ++iteration) {
        const size_t index = random() % leases.size();
        if (leases[index] && random() % 3 == 0) {
            allocator.Release(*leases[index]);
            leases[index].reset();
            std::replace(owners.begin(), owners.end(), static_cast<int>(index), -1);
        } else {
            if (!leases[index]) {
                leases[index] = allocator.CreateLease();
            }
            const size_t count = 1 + random() % 4;
            if (auto offset = allocator.ReserveRange(*leases[index], count * block_size)) {
                Check(*offset % block_size == 0, "block was not aligned");
                for (size_t b = *offset / block_size; b < *offset / block_size + count; ++b) {
                    Check(b < owners.size() && owners[b] == -1, "live storage was reused");
                    owners[b] = static_cast<int>(index);
                }
            }
        }
    }
    for (auto& lease : leases) {
        if (lease) {
            allocator.Release(*lease);
        }
    }
    auto full = allocator.CreateLease();
    Check(
        allocator.ReserveRange(*full, block_count * block_size) == 0,
        "random lifetime sequence leaked or fragmented free storage"
    );
    allocator.Release(*full);
}

void CheckConcurrentLeaseRetirement() {
    constexpr size_t                          block_count  = 64;
    constexpr uint64_t                        block_size   = 4096;
    constexpr size_t                          worker_count = 8;
    Allocator                                 allocator(block_count * block_size, block_size, 32);
    std::array<std::atomic<int>, block_count> owners;
    for (auto& owner : owners) {
        owner.store(-1);
    }
    std::array<std::exception_ptr, worker_count> errors{};
    std::vector<std::jthread>                    workers;
    for (size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                for (size_t batch = 0; batch < 500; ++batch) {
                    auto                lease      = allocator.CreateLease();
                    const auto          checkpoint = allocator.CreateCheckpoint(*lease);
                    std::vector<size_t> owned_blocks;
                    for (size_t i = 0; i < 1 + batch % 4; ++i) {
                        const auto offset = allocator.ReserveRange(*lease, block_size);
                        Check(offset.has_value(), "concurrent leases exhausted sufficient storage");
                        const size_t index    = *offset / block_size;
                        int          expected = -1;
                        Check(
                            owners[index].compare_exchange_strong(expected, static_cast<int>(worker)),
                            "another live lease's block was reused"
                        );
                        owned_blocks.push_back(index);
                        std::this_thread::yield();
                    }
                    const auto ranges = allocator.GetWrittenRanges(*lease);
                    Check(ranges.size() == owned_blocks.size(), "flush omitted an owned block");
                    for (size_t i = 0; i < ranges.size(); ++i) {
                        Check(
                            ranges[i].byte_offset == owned_blocks[i] * block_size &&
                                ranges[i].byte_size == block_size,
                            "flush included another lease's storage"
                        );
                    }
                    // Simulated completion/discard: clear the live-owner oracle
                    // before returning storage, so a new owner may claim it.
                    for (size_t index : owned_blocks) {
                        Check(
                            owners[index].exchange(-1) == static_cast<int>(worker),
                            "live ownership changed before retirement"
                        );
                    }
                    if (batch % 2 == 0) {
                        allocator.Rewind(*lease, checkpoint);
                    }
                    allocator.Release(*lease);
                }
            } catch (...) {
                errors[worker] = std::current_exception();
            }
        });
    }
    workers.clear();
    for (const auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    auto full = allocator.CreateLease();
    Check(
        allocator.ReserveRange(*full, block_count * block_size) == 0, "concurrent retirement lost free space"
    );
    allocator.Release(*full);
}

void CheckAlignmentAndFailedAppend() {
    CheckRejected([] {
        Allocator invalid(1024, 256, 0);
    });
    CheckRejected([] {
        Allocator invalid(1025, 256, 32);
    });
    CheckRejected([] {
        Allocator invalid(1024, 256, 512);
    });

    // A memory atom may be larger than the usual block size. Separate leases
    // must remain isolated even when descriptor alignment itself is smaller.
    Allocator allocator(4 * 128 * KiB, 128 * KiB, 256);
    auto      first  = allocator.CreateLease();
    auto      second = allocator.CreateLease();
    Check(allocator.ReserveRange(*first, 1) == 0, "first aligned allocation failed");
    Check(allocator.ReserveRange(*second, 1) == 128 * KiB, "lease boundary split a memory atom");
    Check(!allocator.ReserveRange(*first, 3 * 128 * KiB), "oversized append succeeded");
    Check(
        allocator.GetUsage(*first).used_bytes == 256 &&
            allocator.GetUsage(*first).reserved_bytes == 128 * KiB,
        "failed append changed existing block state"
    );
    Check(allocator.ReserveRange(*first, 1) == 256, "failed append lost the reusable tail");
    Allocator other_allocator(4 * 128 * KiB, 128 * KiB, 256);
    CheckRejected([&] {
        other_allocator.ReserveRange(*first, 32);
    });
    allocator.Release(*first);
    allocator.Release(*second);
}
void CheckPreparedRecordings() {
    // Eight occupied pages, then release alternate pages. A 512-byte recording
    // fits across four holes although no contiguous 512-byte range exists.
    Allocator allocator(1024, 128, 16, 32);
    std::vector<Moer::Render::VulkanDescriptorBufferLease> blockers;
    for (int i = 0; i < 8; ++i) {
        auto lease = allocator.CreateLease();
        Check(allocator.ReserveRange(*lease, 128).has_value(), "blocker allocation failed");
        blockers.push_back(std::move(lease));
    }
    for (int i = 0; i < 8; i += 2) {
        allocator.Release(*blockers[i]);
    }
    auto batch = allocator.CreateLease();
    const std::vector<std::vector<uint64_t>> too_large{{128, 128, 128, 128, 16}};
    Check(!allocator.PrepareRecordings(*batch, too_large), "oversized batch succeeded");
    Check(allocator.GetUsage(*batch).reserved_bytes == 0, "failed prepare leaked reservations");
    const std::vector<std::vector<uint64_t>> requests{{64, 64, 64, 64}, {128, 128}};
    auto recordings = allocator.PrepareRecordings(*batch, requests);
    Check(recordings.has_value(), "fragmented recording reservation failed");
    Check((*recordings)[0].ranges.size() == 2, "recording should span two holes");
    Check((*recordings)[1].ranges.size() == 2, "second recording should span two holes");
    Allocator::Cursor first((*recordings)[0]);
    Check(first.Allocate(64) == 0, "first cursor offset mismatch");
    Check(!first.Allocate(256), "cursor crossed an unowned gap");
    Check(first.Allocate(64) == 64, "failed allocation advanced cursor");
    Check(first.Allocate(64) == 256, "cursor did not switch segments");
    Check(first.Allocate(64) == 320, "cursor tail mismatch");
    Check(!first.Allocate(1), "cursor exceeded its budget");
    Check(first.Allocate(0) == 0, "zero bind required capacity");
    // After discarding failed recording, a fresh cursor replays the same ranges.
    Allocator::Cursor replay((*recordings)[0]);
    Check(replay.Allocate(128) == 0, "fallback did not reuse prepared range");
    allocator.Release(*batch);
    for (int i = 1; i < 8; i += 2) {
        allocator.Release(*blockers[i]);
    }
    auto full = allocator.CreateLease();
    Check(allocator.ReserveRange(*full, 1024) == 0, "prepared retirement lost space");
    allocator.Release(*full);
}

void CheckPreparedAtomIsolationAndConcurrency() {
    Allocator allocator(4096, 256, 16, 64);
    auto first = allocator.CreateLease();
    const std::vector<std::vector<uint64_t>> requests{{1, 1}, {1}};
    auto recordings = allocator.PrepareRecordings(*first, requests);
    Check(recordings.has_value(), "atom reservation failed");
    const auto usage = allocator.GetUsage(*first);
    Check(usage.used_bytes == 48 && usage.reserved_bytes == 64, "batch padding is not atom granular");
    auto second = allocator.CreateLease();
    const std::vector<std::vector<uint64_t>> next{{1}};
    auto next_recording = allocator.PrepareRecordings(*second, next);
    Allocator::Cursor next_cursor(next_recording->front());
    Check(next_cursor.Allocate(1) == 64, "leases share a flush atom");
    std::vector<std::jthread> workers;
    std::array<uint64_t, 2> offsets{};
    for (size_t i = 0; i < 2; ++i) {
        workers.emplace_back([&, i] {
            Allocator::Cursor cursor((*recordings)[i]);
            offsets[i] = cursor.Allocate(1).value();
        });
    }
    workers.clear();
    Check(offsets[0] == 0 && offsets[1] == 32, "parallel cursors overlap");
    const auto before = allocator.GetUsage(*first);
    const std::vector<std::vector<uint64_t>> overflow{{std::numeric_limits<uint64_t>::max()}};
    Check(!allocator.PrepareRecordings(*first, overflow), "overflow request succeeded");
    Check(allocator.GetUsage(*first).reserved_bytes == before.reserved_bytes, "failed append changed lease");
    allocator.Release(*second);
    allocator.Release(*first);
    auto full = allocator.CreateLease();
    Check(allocator.ReserveRange(*full, 4096) == 0, "mixed atom and block retirement lost space");
    allocator.Release(*full);
}

void CheckPreparedRandomLifetimes() {
    constexpr uint64_t capacity = 16 * KiB;
    Allocator allocator(capacity, 256, 16, 64);
    struct Live {
        std::shared_ptr<Allocator::LeaseState> lease;
        std::vector<size_t> atoms;
    };
    std::vector<bool> owned(capacity / 64);
    std::vector<Live> live;
    std::mt19937 random(147);
    auto retire = [&](size_t index) {
        for (const auto atom : live[index].atoms) owned[atom] = false;
        allocator.Release(*live[index].lease);
        live.erase(live.begin() + index);
    };
    for (size_t step = 0; step < 4000; ++step) {
        if (!live.empty() && (random() % 3 == 0 || live.size() > 20)) {
            retire(random() % live.size());
            continue;
        }
        auto lease = allocator.CreateLease();
        std::vector<std::vector<uint64_t>> requests(1 + random() % 5);
        for (auto& command : requests) {
            for (size_t bind = 0, count = 1 + random() % 7; bind < count; ++bind) {
                command.push_back(random() % 193);
            }
        }
        auto prepared = allocator.PrepareRecordings(*lease, requests);
        if (!prepared) {
            Check(allocator.GetUsage(*lease).reserved_bytes == 0, "random failure leaked storage");
            allocator.Release(*lease);
            continue;
        }
        Live entry{lease, {}};
        std::vector<bool> written(capacity / 16);
        for (size_t i = 0; i < requests.size(); ++i) {
            Allocator::Cursor cursor((*prepared)[i]);
            for (auto bytes : requests[i]) {
                auto offset = cursor.Allocate(bytes);
                Check(offset.has_value(), "prepared bind could not be replayed");
                if (!bytes) continue;
                Check(*offset % 16 == 0 && *offset + bytes <= capacity, "invalid cursor address");
                for (size_t unit = *offset / 16; unit < (*offset + bytes + 15) / 16; ++unit) {
                    Check(!written[unit], "commands in a batch overlap");
                    written[unit] = true;
                }
            }
        }
        for (const auto range : allocator.GetWrittenRanges(*lease)) {
            Check(range.byte_offset % 64 == 0, "flush range does not start on an atom");
            for (size_t atom = range.byte_offset / 64;
                 atom < (range.byte_offset + range.byte_size + 63) / 64; ++atom) {
                Check(!owned[atom], "concurrent submissions share a flush atom");
                owned[atom] = true;
                entry.atoms.push_back(atom);
            }
        }
        live.push_back(std::move(entry));
    }
    while (!live.empty()) retire(random() % live.size());
    auto full = allocator.CreateLease();
    Check(allocator.ReserveRange(*full, capacity) == 0, "random retirement lost capacity");
    allocator.Release(*full);
}

void CheckPreparedCapacityImprovement() {
    Allocator legacy(64 * KiB, 64 * KiB, 32);
    Allocator prepared(64 * KiB, 64 * KiB, 32, 256);
    auto old_first = legacy.CreateLease();
    auto old_second = legacy.CreateLease();
    Check(legacy.ReserveRange(*old_first, 32).has_value(), "legacy allocation failed");
    Check(!legacy.ReserveRange(*old_second, 32), "legacy capacity comparison changed");
    std::vector<std::shared_ptr<Allocator::LeaseState>> leases;
    const std::vector<std::vector<uint64_t>> request{{32}};
    for (size_t i = 0; i < 256; ++i) {
        auto lease = prepared.CreateLease();
        Check(prepared.PrepareRecordings(*lease, request).has_value(), "small batch wasted a whole page");
        leases.push_back(std::move(lease));
    }
    auto extra = prepared.CreateLease();
    Check(!prepared.PrepareRecordings(*extra, request), "prepared capacity exceeded buffer");
    for (auto& lease : leases) prepared.Release(*lease);
    prepared.Release(*extra);
    legacy.Release(*old_second);
    legacy.Release(*old_first);
}
} // namespace

int main() {
    try {
        CheckLazyAllocationAndRetirement();
        CheckFragmentationAndFailurePreserveState();
        CheckSharedLeaseHandoff();
        CheckCheckpointAndWrittenRanges();
        CheckConcurrentRecording();
        CheckRandomizedLifetimes();
        CheckConcurrentLeaseRetirement();
        CheckAlignmentAndFailedAppend();
        CheckPreparedRecordings();
        CheckPreparedAtomIsolationAndConcurrency();
        CheckPreparedRandomLifetimes();
        CheckPreparedCapacityImprovement();
        std::cout << "Vulkan descriptor buffer allocator checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
