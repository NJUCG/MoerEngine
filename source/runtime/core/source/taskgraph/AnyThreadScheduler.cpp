#include "taskgraph/GraphTask.h"
#include "misc/LockFree.h"
#include "platform/Platform.h"

#include "AnyThreadScheduler.h"

#include <cassert>

struct AnyThreadScheduler::PriorityPool::GlobalQueue {
    LockFreeQueueBase<BaseGraphTask, false> queue{};
};

AnyThreadScheduler::PriorityPool::PriorityPool(int32_t in_worker_count) :
    worker_count(in_worker_count),
    workers(std::make_unique<WorkerState[]>(static_cast<size_t>(in_worker_count))),
    global_queue(std::make_unique<GlobalQueue>()) {
    assert(worker_count > 0);
}

AnyThreadScheduler::PriorityPool::~PriorityPool() = default;

AnyThreadScheduler::AnyThreadScheduler(
    Moer::TaskGraphDetail::WorkerPoolTopology topology,
    WakeWorkerFn                              wake_worker,
    void*                                     wake_context
) :
    m_topology(topology),
    m_wake_worker(wake_worker),
    m_wake_context(wake_context) {
    assert(m_wake_worker != nullptr);
    for (int32_t priority = 0; priority < EThread::PriorityCount; ++priority) {
        assert(m_topology.counts[priority] > 0);
        m_pools[priority] = std::make_unique<PriorityPool>(m_topology.counts[priority]);
    }
}

AnyThreadScheduler::~AnyThreadScheduler() = default;

bool AnyThreadScheduler::TryGetLocalWorker(
    EThread::Type  actual_current_thread,
    ThreadPriority target_priority,
    int32_t&       local_worker_index
) const noexcept {
    if (EThread::IsUnKnownThread(actual_current_thread)) {
        return false;
    }

    const int32_t thread_index = EThread::GetThreadIndex(actual_current_thread);
    const int32_t actual_priority = m_topology.PriorityForThread(thread_index);
    if (actual_priority != target_priority ||
        EThread::GetThreadPriority(actual_current_thread) != target_priority) {
        return false;
    }

    local_worker_index = m_topology.LocalIndex(thread_index, target_priority);
    return local_worker_index >= 0;
}

bool AnyThreadScheduler::IsTaskGraphWorker(EThread::Type actual_current_thread) const noexcept {
    if (EThread::IsUnKnownThread(actual_current_thread)) {
        return false;
    }

    const int32_t thread_index = EThread::GetThreadIndex(actual_current_thread);
    const int32_t priority     = m_topology.PriorityForThread(thread_index);
    return priority >= 0 && EThread::GetThreadPriority(actual_current_thread) == priority;
}

bool AnyThreadScheduler::WakeOneIdleWorker(ThreadPriority priority) noexcept {
    PriorityPool& pool = *m_pools[priority];
    const uint32_t start = pool.wake_cursor.fetch_add(1, std::memory_order_relaxed) %
                           static_cast<uint32_t>(pool.worker_count);

    for (int32_t offset = 0; offset < pool.worker_count; ++offset) {
        const int32_t local_index = static_cast<int32_t>(
            (start + static_cast<uint32_t>(offset)) % static_cast<uint32_t>(pool.worker_count)
        );
        bool expected = true;
        if (!pool.workers[local_index].idle.compare_exchange_strong(
                expected,
                false,
                std::memory_order_seq_cst,
                std::memory_order_seq_cst
            )) {
            continue;
        }

        const int32_t thread_index = m_topology.ThreadIndex(priority, local_index);
        assert(thread_index >= 0);
        m_wake_worker(m_wake_context, thread_index);
        return true;
    }
    return false;
}

void AnyThreadScheduler::Enqueue(
    BaseGraphTask* task,
    EThread::Type  actual_current_thread,
    bool           wake_peer
) noexcept {
    assert(task != nullptr);
    (void)wake_peer;
    const ThreadPriority priority = task->GetPriority();
    assert(priority >= 0 && priority < EThread::PriorityCount);
    PriorityPool& pool = *m_pools[priority];

    const auto publish = [&]() noexcept {
        m_pending_tasks.fetch_add(1, std::memory_order_relaxed);

        int32_t local_worker_index = -1;
        if (TryGetLocalWorker(actual_current_thread, priority, local_worker_index)) {
            pool.workers[local_worker_index].deque.PushBottom(task);
            // Always notify an actual idle peer. A single continuation can block
            // its owner immediately after publication, so wake_peer=false is not
            // a sufficient progress guarantee for a local deque.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            WakeOneIdleWorker(priority);
            return;
        }

        // External, named-thread and cross-priority producers use the MPMC ingress
        // queue. They must always wake a real idle worker even when an upstream
        // continuation batch passes wake_peer=false.
        pool.global_queue->queue.Push(task);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        WakeOneIdleWorker(priority);
    };

    if (IsTaskGraphWorker(actual_current_thread)) {
        // The executing parent remains included in m_pending_tasks until after
        // task->Execute() returns, so nested worker publication cannot race the
        // pending-zero drain transition.
        publish();
        return;
    }

    // Serialize external/named publication with BeginDrain. Once shutdown owns
    // this gate, accepting an untracked producer would either prolong shutdown
    // indefinitely or publish into a scheduler that is about to be destroyed.
    std::lock_guard admission_lock(m_admission_mutex);
    if (m_draining.load(std::memory_order_acquire)) {
        Platform::FailFast("external AnyThread publication attempted during TaskGraph drain");
    }
    publish();
}

BaseGraphTask* AnyThreadScheduler::TryFindWork(
    PriorityPool& pool,
    int32_t       local_worker_index
) noexcept {
    WorkerState& self = pool.workers[local_worker_index];

    // A permanently replenished local deque must not starve external ingress.
    if (self.local_pop_streak >= GlobalProbeInterval) {
        self.local_pop_streak = 0;
        if (BaseGraphTask* task = pool.global_queue->queue.Pop()) {
            return task;
        }
    }

    if (BaseGraphTask* task = self.deque.PopBottom()) {
        ++self.local_pop_streak;
        return task;
    }

    self.local_pop_streak = 0;
    if (BaseGraphTask* task = pool.global_queue->queue.Pop()) {
        return task;
    }

    if (pool.worker_count <= 1) {
        return nullptr;
    }

    const uint32_t first_victim = ++self.steal_cursor % static_cast<uint32_t>(pool.worker_count);
    for (int32_t offset = 0; offset < pool.worker_count; ++offset) {
        const int32_t victim = static_cast<int32_t>(
            (first_victim + static_cast<uint32_t>(offset)) %
            static_cast<uint32_t>(pool.worker_count)
        );
        if (victim == local_worker_index) {
            continue;
        }
        if (BaseGraphTask* task = pool.workers[victim].deque.StealTop()) {
            return task;
        }
    }
    return nullptr;
}

BaseGraphTask* AnyThreadScheduler::DequeueOrPrepareToPark(int32_t thread_index) noexcept {
    const int32_t priority = m_topology.PriorityForThread(thread_index);
    assert(priority >= 0 && priority < EThread::PriorityCount);
    PriorityPool& pool = *m_pools[priority];
    const int32_t local_worker_index = m_topology.LocalIndex(thread_index, priority);
    assert(local_worker_index >= 0 && local_worker_index < pool.worker_count);

    WorkerState& self = pool.workers[local_worker_index];
    self.idle.store(false, std::memory_order_release);
    if (BaseGraphTask* task = TryFindWork(pool, local_worker_index)) {
        return task;
    }

    // Two-phase park handshake. A producer either observes this registration
    // and sends a sticky Event signal, or its earlier publication is observed
    // by the complete second search below.
    self.idle.store(true, std::memory_order_relaxed);
    // Paired with the producer-side seq_cst fence before idle claiming. This
    // prevents the store-buffering outcome where the worker misses the newly
    // published task while the producer still misses the idle registration.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (BaseGraphTask* task = TryFindWork(pool, local_worker_index)) {
        self.idle.store(false, std::memory_order_release);
        return task;
    }
    return nullptr;
}

void AnyThreadScheduler::NotifyTaskCompleted() noexcept {
    const uint64_t previous = m_pending_tasks.fetch_sub(1, std::memory_order_acq_rel);
    assert(previous > 0);
    if (previous != 1) {
        return;
    }

    std::lock_guard lock(m_idle_mutex);
    m_idle_condition.notify_all();
}

void AnyThreadScheduler::BeginDrain() noexcept {
    std::lock_guard admission_lock(m_admission_mutex);
    m_draining.store(true, std::memory_order_release);
}

AnyThreadScheduler::NamedPublicationGuard
AnyThreadScheduler::AcquireNamedPublication() noexcept {
    NamedPublicationGuard guard(m_admission_mutex);
    if (m_draining.load(std::memory_order_acquire)) {
        Platform::FailFast("TaskGraph named-target publication attempted during drain");
    }
    return guard;
}

bool AnyThreadScheduler::IsDraining() const noexcept {
    return m_draining.load(std::memory_order_acquire);
}

bool AnyThreadScheduler::IsWaitingForIdle() const noexcept {
    return m_waiting_for_idle.load(std::memory_order_acquire);
}

void AnyThreadScheduler::WaitUntilIdle() noexcept {
    std::unique_lock lock(m_idle_mutex);
    m_waiting_for_idle.store(true, std::memory_order_release);
    m_idle_condition.wait(lock, [this] {
        return m_pending_tasks.load(std::memory_order_acquire) == 0;
    });
    m_waiting_for_idle.store(false, std::memory_order_release);
}
