#ifndef MOER_ANY_THREAD_SCHEDULER_H
#define MOER_ANY_THREAD_SCHEDULER_H

#include "taskgraph/WorkStealing.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

class BaseGraphTask;

/** Core-private AnyThread backend. GraphEvent and dependency semantics stay in TaskGraph. */
class AnyThreadScheduler {
public:
    using WakeWorkerFn = void (*)(void* context, int32_t thread_index) noexcept;

    AnyThreadScheduler(
        Moer::TaskGraphDetail::WorkerPoolTopology topology,
        WakeWorkerFn                              wake_worker,
        void*                                     wake_context
    );
    ~AnyThreadScheduler();

    AnyThreadScheduler(const AnyThreadScheduler&)            = delete;
    AnyThreadScheduler& operator=(const AnyThreadScheduler&) = delete;

    void Enqueue(
        BaseGraphTask* task,
        EThread::Type  actual_current_thread,
        bool           wake_peer
    ) noexcept;

    /** Returns work or atomically registers the caller as safe to park. */
    [[nodiscard]] BaseGraphTask* DequeueOrPrepareToPark(int32_t thread_index) noexcept;
    void                         NotifyTaskCompleted() noexcept;
    /** Stop external/named producers while allowing in-flight workers to publish continuations. */
    void                         BeginDrain() noexcept;
    [[nodiscard]] bool           IsDraining() const noexcept;
    [[nodiscard]] bool           IsWaitingForIdle() const noexcept;
    void                         WaitUntilIdle() noexcept;

private:
    struct WorkerState {
        Moer::TaskGraphDetail::ChaseLevDeque<BaseGraphTask> deque{};
        std::atomic_bool idle{false};
        uint32_t         local_pop_streak = 0;
        uint32_t         steal_cursor     = 0;
    };

    struct PriorityPool {
        struct GlobalQueue;

        explicit PriorityPool(int32_t worker_count);
        ~PriorityPool();

        int32_t                         worker_count = 0;
        std::unique_ptr<WorkerState[]>  workers{};
        std::unique_ptr<GlobalQueue>    global_queue{};
        std::atomic<uint32_t>           wake_cursor{0};
    };

    [[nodiscard]] BaseGraphTask*
    TryFindWork(PriorityPool& pool, int32_t local_worker_index) noexcept;
    [[nodiscard]] bool TryGetLocalWorker(
        EThread::Type  actual_current_thread,
        ThreadPriority target_priority,
        int32_t&       local_worker_index
    ) const noexcept;
    [[nodiscard]] bool IsTaskGraphWorker(EThread::Type actual_current_thread) const noexcept;
    bool WakeOneIdleWorker(ThreadPriority priority) noexcept;

    static constexpr uint32_t GlobalProbeInterval = 32;

    Moer::TaskGraphDetail::WorkerPoolTopology                 m_topology{};
    std::array<std::unique_ptr<PriorityPool>, EThread::PriorityCount> m_pools{};
    WakeWorkerFn                                              m_wake_worker = nullptr;
    void*                                                     m_wake_context = nullptr;
    std::atomic<uint64_t>                                     m_pending_tasks{0};
    std::atomic_bool                                          m_draining{false};
    std::atomic_bool                                          m_waiting_for_idle{false};
    std::mutex                                                m_admission_mutex{};
    std::mutex                                                m_idle_mutex{};
    std::condition_variable                                   m_idle_condition{};
};

#endif // MOER_ANY_THREAD_SCHEDULER_H
