#include "rhi/ExternalCpuJoinPool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Moer::Render;
using namespace std::chrono_literals;

namespace {

struct ThrowingCopyJob {
    std::atomic<bool>* throw_on_copy{nullptr};

    ThrowingCopyJob() = default;
    explicit ThrowingCopyJob(std::atomic<bool>& _throw_on_copy) : throw_on_copy(&_throw_on_copy) {}
    ThrowingCopyJob(const ThrowingCopyJob& _other) : throw_on_copy(_other.throw_on_copy) {
        if (throw_on_copy && throw_on_copy->load(std::memory_order_relaxed)) {
            throw std::runtime_error("injected job copy failure");
        }
    }
    void operator()() const {}
};

void Expect(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

void ExternalCallerSingleWorker() {
    ExternalCpuJoinPool pool(1);
    std::atomic<uint32_t> executed{0};
    std::atomic<uint32_t> active{0};
    std::atomic<uint32_t> max_active{0};
    std::mutex            ids_mutex;
    std::vector<std::thread::id> worker_ids;
    std::thread::id               coordinator_id;
    ExternalJoinResult            result = ExternalJoinResult::Failed;

    std::vector<ExternalCpuJoinPool::Job> jobs;
    for (uint32_t index = 0; index < 32; ++index) {
        jobs.emplace_back([&] {
            const uint32_t now_active = active.fetch_add(1, std::memory_order_acq_rel) + 1;
            uint32_t       observed   = max_active.load(std::memory_order_relaxed);
            while (observed < now_active &&
                   !max_active.compare_exchange_weak(observed, now_active, std::memory_order_relaxed)) {
            }
            {
                std::lock_guard lock(ids_mutex);
                worker_ids.push_back(std::this_thread::get_id());
            }
            executed.fetch_add(1, std::memory_order_relaxed);
            active.fetch_sub(1, std::memory_order_acq_rel);
        });
    }

    std::jthread coordinator([&] {
        coordinator_id = std::this_thread::get_id();
        result         = pool.RunAndWait(jobs);
    });
    coordinator.join();

    Expect(result == ExternalJoinResult::Completed, "external single-worker batch did not complete");
    Expect(executed.load() == jobs.size(), "external single-worker batch lost or duplicated work");
    Expect(max_active.load() == 1, "single-worker pool executed more than one job concurrently");
    for (const std::thread::id worker_id : worker_ids) {
        Expect(worker_id != coordinator_id, "external coordinator pumped a worker job");
    }
}

void NestedJoinRejected() {
    ExternalCpuJoinPool pool(1);
    std::atomic<uint32_t> nested_executed{0};
    ExternalJoinResult   nested_result = ExternalJoinResult::Completed;

    std::vector<ExternalCpuJoinPool::Job> outer_jobs;
    outer_jobs.emplace_back([&] {
        std::vector<ExternalCpuJoinPool::Job> nested_jobs;
        nested_jobs.emplace_back([&] { nested_executed.fetch_add(1); });
        nested_result = pool.RunAndWait(nested_jobs);
    });

    const ExternalJoinResult outer_result = pool.RunAndWait(outer_jobs);
    Expect(outer_result == ExternalJoinResult::Completed, "outer batch failed after nested rejection");
    Expect(
        nested_result == ExternalJoinResult::ReentrantRejected,
        "nested worker join was not rejected"
    );
    Expect(nested_executed.load() == 0, "rejected nested batch executed work");
}

void CrossPoolNestedJoinRejected() {
    ExternalCpuJoinPool first_pool(1);
    ExternalCpuJoinPool second_pool(1);
    ExternalJoinResult  nested_result = ExternalJoinResult::Completed;
    std::atomic<uint32_t> nested_executed{0};

    std::vector<ExternalCpuJoinPool::Job> outer_jobs;
    outer_jobs.emplace_back([&] {
        std::vector<ExternalCpuJoinPool::Job> nested_jobs;
        nested_jobs.emplace_back([&] { nested_executed.fetch_add(1); });
        nested_result = second_pool.RunAndWait(nested_jobs);
    });

    Expect(
        first_pool.RunAndWait(outer_jobs) == ExternalJoinResult::Completed,
        "outer cross-pool batch failed"
    );
    Expect(
        nested_result == ExternalJoinResult::ReentrantRejected,
        "cross-pool nested join was not rejected"
    );
    Expect(nested_executed.load() == 0, "rejected cross-pool batch executed work");
}

void FailedJobDrainsAndPoolRemainsUsable() {
    ExternalCpuJoinPool pool(2);
    std::atomic<uint32_t> executed{0};
    std::vector<ExternalCpuJoinPool::Job> failing_jobs;
    failing_jobs.emplace_back([] { throw std::runtime_error("injected job failure"); });
    failing_jobs.emplace_back([&] { executed.fetch_add(1, std::memory_order_relaxed); });

    Expect(
        pool.RunAndWait(failing_jobs) == ExternalJoinResult::Failed,
        "throwing job did not report a failed batch"
    );
    Expect(executed.load() == 1, "failed batch did not drain its remaining accepted work");

    std::vector<ExternalCpuJoinPool::Job> recovery_jobs;
    recovery_jobs.emplace_back([&] { executed.fetch_add(1, std::memory_order_relaxed); });
    Expect(
        pool.RunAndWait(recovery_jobs) == ExternalJoinResult::Completed,
        "pool was not reusable after a failed job"
    );
    Expect(executed.load() == 2, "recovery batch did not execute");
}

void JobCopyFailureDoesNotPublishPartialBatch() {
    ExternalCpuJoinPool pool(1);
    std::atomic<bool>    throw_on_copy{false};
    std::vector<ExternalCpuJoinPool::Job> jobs;
    jobs.emplace_back(ThrowingCopyJob(throw_on_copy));
    throw_on_copy.store(true, std::memory_order_relaxed);

    bool copy_failed = false;
    try {
        (void)pool.RunAndWait(jobs);
    } catch (const std::runtime_error&) {
        copy_failed = true;
    }
    Expect(copy_failed, "injected job copy failure did not propagate");

    throw_on_copy.store(false, std::memory_order_relaxed);
    std::vector<ExternalCpuJoinPool::Job> recovery_jobs;
    recovery_jobs.emplace_back([] {});
    Expect(
        pool.RunAndWait(recovery_jobs) == ExternalJoinResult::Completed,
        "copy failure left the pool in an active or poisoned state"
    );
}

void EmptyBatchAndRepeatedStopAreIdempotent() {
    ExternalCpuJoinPool pool(1);
    const std::vector<ExternalCpuJoinPool::Job> no_jobs;
    Expect(
        pool.RunAndWait(no_jobs) == ExternalJoinResult::Completed,
        "empty batch did not complete"
    );

    pool.StopAndDrain();
    pool.StopAndDrain();
    Expect(
        pool.RunAndWait(no_jobs) == ExternalJoinResult::Stopped,
        "stopped pool accepted an empty batch"
    );
}

void StoppedPoolRejectsBeforeCopyingJobs() {
    ExternalCpuJoinPool pool(1);
    pool.StopAndDrain();

    std::atomic<bool> throw_on_copy{false};
    std::vector<ExternalCpuJoinPool::Job> jobs;
    jobs.emplace_back(ThrowingCopyJob(throw_on_copy));
    throw_on_copy.store(true, std::memory_order_relaxed);

    bool threw = false;
    ExternalJoinResult result = ExternalJoinResult::Failed;
    try {
        result = pool.RunAndWait(jobs);
    } catch (...) {
        threw = true;
    }
    Expect(!threw, "stopped pool copied a throwing job before rejection");
    Expect(result == ExternalJoinResult::Stopped, "stopped pool did not return Stopped");
}

void ConcurrentBatchesUseSingleActiveBatch() {
    ExternalCpuJoinPool pool(2);
    std::mutex              gate_mutex;
    std::condition_variable gate_cv;
    bool                    first_started{false};
    bool                    release_first{false};
    std::atomic<bool>       second_entered{false};
    std::atomic<bool>       second_executed{false};
    ExternalJoinResult      first_result  = ExternalJoinResult::Failed;
    ExternalJoinResult      second_result = ExternalJoinResult::Failed;

    std::vector<ExternalCpuJoinPool::Job> first_jobs;
    first_jobs.emplace_back([&] {
        std::unique_lock lock(gate_mutex);
        first_started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_first; });
    });
    std::vector<ExternalCpuJoinPool::Job> second_jobs;
    second_jobs.emplace_back([&] { second_executed.store(true, std::memory_order_release); });

    std::jthread first([&] { first_result = pool.RunAndWait(first_jobs); });
    {
        std::unique_lock lock(gate_mutex);
        Expect(
            gate_cv.wait_for(lock, 2s, [&] { return first_started; }),
            "first concurrent batch did not start"
        );
    }
    std::jthread second([&] {
        second_entered.store(true, std::memory_order_release);
        second_result = pool.RunAndWait(second_jobs);
    });
    while (!second_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(20ms);
    Expect(
        !second_executed.load(std::memory_order_acquire),
        "second batch overlapped the active first batch"
    );

    {
        std::lock_guard lock(gate_mutex);
        release_first = true;
    }
    gate_cv.notify_all();
    first.join();
    second.join();

    Expect(first_result == ExternalJoinResult::Completed, "first concurrent batch failed");
    Expect(second_result == ExternalJoinResult::Completed, "second concurrent batch failed");
    Expect(second_executed.load(std::memory_order_acquire), "second batch never executed");
}

void ShutdownWhileQueued() {
    ExternalCpuJoinPool pool(1);
    constexpr uint32_t  job_count = 24;

    std::mutex              gate_mutex;
    std::condition_variable gate_cv;
    bool                    first_started{false};
    bool                    release_first{false};
    std::atomic<uint32_t>    executed{0};
    std::atomic<bool>        stopper_entered{false};
    std::atomic<bool>        stop_finished{false};
    ExternalJoinResult       batch_result = ExternalJoinResult::Failed;

    std::vector<ExternalCpuJoinPool::Job> jobs;
    jobs.emplace_back([&] {
        std::unique_lock lock(gate_mutex);
        first_started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_first; });
        executed.fetch_add(1, std::memory_order_relaxed);
    });
    for (uint32_t index = 1; index < job_count; ++index) {
        jobs.emplace_back([&] { executed.fetch_add(1, std::memory_order_relaxed); });
    }

    std::jthread coordinator([&] { batch_result = pool.RunAndWait(jobs); });
    {
        std::unique_lock lock(gate_mutex);
        Expect(
            gate_cv.wait_for(lock, 2s, [&] { return first_started; }),
            "queued-shutdown test did not start its blocking job"
        );
    }

    std::jthread stopper([&] {
        stopper_entered.store(true, std::memory_order_release);
        pool.StopAndDrain();
        stop_finished.store(true, std::memory_order_release);
    });
    while (!stopper_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    Expect(!stop_finished.load(std::memory_order_acquire), "shutdown returned before accepted work drained");

    {
        std::lock_guard lock(gate_mutex);
        release_first = true;
    }
    gate_cv.notify_all();
    coordinator.join();
    stopper.join();

    Expect(batch_result == ExternalJoinResult::Completed, "accepted batch failed during shutdown");
    Expect(executed.load() == job_count, "shutdown did not drain every accepted job exactly once");
    Expect(stop_finished.load(), "shutdown did not finish after the accepted batch drained");

    std::vector<ExternalCpuJoinPool::Job> rejected_jobs;
    rejected_jobs.emplace_back([] {});
    Expect(
        pool.RunAndWait(rejected_jobs) == ExternalJoinResult::Stopped,
        "stopped pool accepted a new batch"
    );
}

} // namespace

int main() {
    try {
        ExternalCallerSingleWorker();
        NestedJoinRejected();
        CrossPoolNestedJoinRejected();
        FailedJobDrainsAndPoolRemainsUsable();
        JobCopyFailureDoesNotPublishPartialBatch();
        EmptyBatchAndRepeatedStopAreIdempotent();
        StoppedPoolRejectsBeforeCopyingJobs();
        ConcurrentBatchesUseSingleActiveBatch();
        ShutdownWhileQueued();
        std::cout << "ExternalCpuJoin tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ExternalCpuJoin test failed: " << error.what() << '\n';
        return 1;
    }
}
