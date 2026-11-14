#ifndef MOER_TASKGRAPH_WORK_STEALING_H
#define MOER_TASKGRAPH_WORK_STEALING_H

#include "taskgraph/ThreadManager.h"

#include "misc/Types.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <exception>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

namespace Moer::TaskGraphDetail {

/**
 * Exact worker ranges for the three legacy priority pools.
 *
 * The old ceil(worker_count / 3) arithmetic could leave the Low pool empty
 * for otherwise valid worker counts (for example four workers).  Keeping the
 * ranges explicit also removes the old 32-bit waiter-bitmap indexing limit.
 */
struct WorkerPoolTopology {
    int32                                     named_thread_count  = 0;
    int32                                     worker_thread_count = 0;
    std::array<int32, EThread::PriorityCount> starts{};
    std::array<int32, EThread::PriorityCount> counts{};

    [[nodiscard]] int32 PriorityForThread(int32 thread_index) const noexcept {
        for (int32 priority = 0; priority < EThread::PriorityCount; ++priority) {
            const int32 begin = starts[priority];
            const int32 end   = begin + counts[priority];
            if (thread_index >= begin && thread_index < end) {
                return priority;
            }
        }
        return -1;
    }

    [[nodiscard]] int32 LocalIndex(int32 thread_index, int32 priority) const noexcept {
        if (priority < 0 || priority >= EThread::PriorityCount) {
            return -1;
        }
        const int32 local_index = thread_index - starts[priority];
        return local_index >= 0 && local_index < counts[priority] ? local_index : -1;
    }

    [[nodiscard]] int32 ThreadIndex(int32 priority, int32 local_index) const noexcept {
        if (priority < 0 || priority >= EThread::PriorityCount || local_index < 0 ||
            local_index >= counts[priority]) {
            return -1;
        }
        return starts[priority] + local_index;
    }
};

inline WorkerPoolTopology BuildWorkerPoolTopology(
    int32 named_thread_count,
    int32 worker_thread_count
) noexcept {
    if (named_thread_count < 0 || worker_thread_count < EThread::PriorityCount ||
        named_thread_count + worker_thread_count > EThread::UNKNOWN_THREAD) {
        std::terminate();
    }

    WorkerPoolTopology topology{};
    topology.named_thread_count  = named_thread_count;
    topology.worker_thread_count = worker_thread_count;

    const int32 workers_per_pool = worker_thread_count / EThread::PriorityCount;
    const int32 remainder        = worker_thread_count % EThread::PriorityCount;
    int32       next_start       = named_thread_count;
    for (int32 priority = 0; priority < EThread::PriorityCount; ++priority) {
        topology.starts[priority] = next_start;
        topology.counts[priority] = workers_per_pool + (priority < remainder ? 1 : 0);
        next_start += topology.counts[priority];
    }

    assert(next_start == named_thread_count + worker_thread_count);
    return topology;
}

/**
 * Single-owner, multi-thief Chase-Lev deque.
 *
 * Only the owning worker may call PushBottom/PopBottom. Any worker may call
 * StealTop. Buffers grow by powers of two and every old generation remains
 * alive until scheduler destruction, after all workers have joined. This is
 * deliberately simple reclamation: a thief can safely finish through a stale
 * buffer pointer without a hazard-pointer/epoch dependency in the hot path.
 */
template<typename T>
class ChaseLevDeque {
    static_assert(std::is_object_v<T>);

    struct Buffer {
        explicit Buffer(int64 initial_capacity) : capacity(initial_capacity) {
            assert(initial_capacity >= 2);
            assert((initial_capacity & (initial_capacity - 1)) == 0);
            slots = std::make_unique<std::atomic<T*>[]>(static_cast<size_t>(capacity));
            for (int64 index = 0; index < capacity; ++index) {
                slots[index].store(nullptr, std::memory_order_relaxed);
            }
        }

        [[nodiscard]] T* Load(int64 index) const noexcept {
            return slots[static_cast<size_t>(index & (capacity - 1))].load(
                std::memory_order_relaxed
            );
        }

        void Store(int64 index, T* value) noexcept {
            slots[static_cast<size_t>(index & (capacity - 1))].store(
                value,
                std::memory_order_relaxed
            );
        }

        int64                              capacity = 0;
        std::unique_ptr<std::atomic<T*>[]> slots{};
    };

public:
    explicit ChaseLevDeque(int64 initial_capacity = 32) : m_top(0), m_bottom(0) {
        assert(initial_capacity >= 2);
        assert((initial_capacity & (initial_capacity - 1)) == 0);
        Buffer* initial = AllocateBuffer(initial_capacity);
        m_buffer.store(initial, std::memory_order_release);
    }

    ChaseLevDeque(const ChaseLevDeque&)            = delete;
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;
    ChaseLevDeque(ChaseLevDeque&&)                 = delete;
    ChaseLevDeque& operator=(ChaseLevDeque&&)      = delete;

    void PushBottom(T* value) noexcept {
        assert(value != nullptr);

        const int64   bottom = m_bottom.load(std::memory_order_relaxed);
        const int64   top    = m_top.load(std::memory_order_acquire);
        Buffer*       buffer = m_buffer.load(std::memory_order_relaxed);
        if (bottom - top >= buffer->capacity - 1) {
            Grow(top, bottom, buffer);
            buffer = m_buffer.load(std::memory_order_relaxed);
        }

        buffer->Store(bottom, value);
        std::atomic_thread_fence(std::memory_order_release);
        m_bottom.store(bottom + 1, std::memory_order_relaxed);
    }

    [[nodiscard]] T* PopBottom() noexcept {
        int64   bottom = m_bottom.load(std::memory_order_relaxed) - 1;
        Buffer* buffer = m_buffer.load(std::memory_order_relaxed);
        m_bottom.store(bottom, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        int64 top = m_top.load(std::memory_order_relaxed);
        if (top <= bottom) {
            T* value = buffer->Load(bottom);
            if (top == bottom) {
                if (!m_top.compare_exchange_strong(
                        top,
                        top + 1,
                        std::memory_order_seq_cst,
                        std::memory_order_relaxed
                    )) {
                    value = nullptr;
                }
                m_bottom.store(bottom + 1, std::memory_order_relaxed);
            }
            return value;
        }

        m_bottom.store(bottom + 1, std::memory_order_relaxed);
        return nullptr;
    }

    [[nodiscard]] T* StealTop() noexcept {
        int64 top = m_top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const int64 bottom = m_bottom.load(std::memory_order_acquire);
        if (top >= bottom) {
            return nullptr;
        }

        Buffer* buffer = m_buffer.load(std::memory_order_acquire);
        T*      value  = buffer->Load(top);
        if (m_top.compare_exchange_strong(
                top,
                top + 1,
                std::memory_order_seq_cst,
                std::memory_order_relaxed
            )) {
            return value;
        }
        return nullptr;
    }

    [[nodiscard]] int64 ApproximateSize() const noexcept {
        const int64 bottom = m_bottom.load(std::memory_order_acquire);
        const int64 top    = m_top.load(std::memory_order_acquire);
        return std::max<int64>(0, bottom - top);
    }

private:
    Buffer* AllocateBuffer(int64 capacity) {
        auto buffer = std::make_unique<Buffer>(capacity);
        Buffer* raw = buffer.get();
        m_buffers.emplace_back(std::move(buffer));
        return raw;
    }

    void Grow(int64 top, int64 bottom, Buffer* current) noexcept {
        assert(current == m_buffer.load(std::memory_order_relaxed));
        if (current->capacity > std::numeric_limits<int64>::max() / 2) {
            std::terminate();
        }

        Buffer* expanded = AllocateBuffer(current->capacity * 2);
        for (int64 index = top; index < bottom; ++index) {
            expanded->Store(index, current->Load(index));
        }
        m_buffer.store(expanded, std::memory_order_release);
    }

    alignas(64) std::atomic<int64> m_top;
    alignas(64) std::atomic<int64> m_bottom;
    alignas(64) std::atomic<Buffer*> m_buffer{nullptr};

    // Owner-only metadata. Thieves only dereference published Buffer objects.
    std::vector<std::unique_ptr<Buffer>> m_buffers{};
};

} // namespace Moer::TaskGraphDetail

#endif // MOER_TASKGRAPH_WORK_STEALING_H
