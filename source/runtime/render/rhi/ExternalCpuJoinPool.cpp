#include "ExternalCpuJoinPool.h"

#include <algorithm>

namespace Moer::Render {
namespace {
thread_local ExternalCpuJoinPool* g_active_external_join_pool = nullptr;
}

ExternalCpuJoinPool::ExternalCpuJoinPool(uint32_t _worker_count) {
    const uint32_t worker_count = std::max(1u, _worker_count);
    workers.reserve(worker_count);
    try {
        for (uint32_t worker_index = 0; worker_index < worker_count; ++worker_index) {
            workers.emplace_back([this] { WorkerMain(); });
        }
    } catch (...) {
        {
            std::lock_guard lock(mutex);
            accepting = false;
        }
        work_cv.notify_all();
        for (std::jthread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

ExternalCpuJoinPool::~ExternalCpuJoinPool() {
    StopAndDrain();
}

ExternalJoinResult ExternalCpuJoinPool::RunAndWait(std::span<const Job> _jobs) {
    if (g_active_external_join_pool != nullptr) {
        return ExternalJoinResult::ReentrantRejected;
    }

    // A pool that was already stopped must reject without copying user jobs.
    // Besides being cheaper, this keeps Stopped deterministic even when a
    // std::function target has a throwing copy constructor.
    {
        std::lock_guard lock(mutex);
        if (!accepting) {
            return ExternalJoinResult::Stopped;
        }
    }

    // Copy every potentially-throwing job before publishing a batch. Once the
    // pool state changes below, pending_jobs.swap is noexcept and the accepted
    // batch is guaranteed to reach remaining_jobs == 0.
    std::deque<Job> prepared_jobs(_jobs.begin(), _jobs.end());

    std::unique_lock lock(mutex);
    batch_cv.wait(lock, [this] { return !batch_active || !accepting; });
    if (!accepting) {
        return ExternalJoinResult::Stopped;
    }

    batch_active   = true;
    first_failure  = nullptr;
    remaining_jobs = prepared_jobs.size();
    pending_jobs.swap(prepared_jobs);

    if (remaining_jobs == 0) {
        batch_active = false;
        batch_cv.notify_all();
        return ExternalJoinResult::Completed;
    }

    work_cv.notify_all();
    batch_cv.wait(lock, [this] { return remaining_jobs == 0; });

    const ExternalJoinResult result = first_failure ? ExternalJoinResult::Failed
                                                     : ExternalJoinResult::Completed;
    batch_active = false;
    batch_cv.notify_all();
    return result;
}

void ExternalCpuJoinPool::StopAndDrain() noexcept {
    if (g_active_external_join_pool == this) {
        // A worker must never join its own pool. RunAndWait has an explicit
        // typed rejection for this case; shutdown remains coordinator-owned.
        return;
    }

    {
        std::unique_lock lock(mutex);
        if (stopped) {
            return;
        }
        if (stop_join_in_progress) {
            stop_cv.wait(lock, [this] { return stopped; });
            return;
        }

        stop_join_in_progress = true;
        accepting             = false;
        work_cv.notify_all();
        batch_cv.notify_all();
        batch_cv.wait(lock, [this] {
            return !batch_active && remaining_jobs == 0 && pending_jobs.empty() && active_jobs == 0;
        });
    }

    for (std::jthread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    {
        std::lock_guard lock(mutex);
        stopped = true;
    }
    stop_cv.notify_all();
}

void ExternalCpuJoinPool::WorkerMain() {
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex);
            work_cv.wait(lock, [this] { return !pending_jobs.empty() || !accepting; });
            if (pending_jobs.empty()) {
                if (!accepting) {
                    return;
                }
                continue;
            }

            job = std::move(pending_jobs.front());
            pending_jobs.pop_front();
            ++active_jobs;
        }

        g_active_external_join_pool = this;
        try {
            job();
        } catch (...) {
            std::lock_guard lock(mutex);
            if (!first_failure) {
                first_failure = std::current_exception();
            }
        }
        g_active_external_join_pool = nullptr;

        {
            std::lock_guard lock(mutex);
            --active_jobs;
            --remaining_jobs;
            if (remaining_jobs == 0) {
                batch_cv.notify_all();
            }
            if (!accepting && pending_jobs.empty() && active_jobs == 0) {
                work_cv.notify_all();
                batch_cv.notify_all();
            }
        }
    }
}

} // namespace Moer::Render
