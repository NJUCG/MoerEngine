#ifndef MOER_RENDER_EXTERNAL_CPU_JOIN_POOL_H
#define MOER_RENDER_EXTERNAL_CPU_JOIN_POOL_H

#include "RenderAPI.h"

#include "misc/Types.h"
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace Moer::Render {

enum class ExternalJoinResult : uint8 {
    Completed,
    Failed,
    ReentrantRejected,
    Stopped,
};

// A CPU-only, single-batch join seam for work dispatched by an unregistered
// coordinator thread such as the Vulkan RHI std::jthread. The waiting caller
// never pumps jobs, and accepted work is drained before shutdown returns.
class RENDER_API ExternalCpuJoinPool {
public:
    using Job = std::function<void()>;

    explicit ExternalCpuJoinPool(uint32 _worker_count);
    ~ExternalCpuJoinPool();

    ExternalCpuJoinPool(const ExternalCpuJoinPool&)            = delete;
    ExternalCpuJoinPool& operator=(const ExternalCpuJoinPool&) = delete;

    ExternalJoinResult RunAndWait(std::span<const Job> _jobs);
    void               StopAndDrain() noexcept;

    uint32 WorkerCount() const noexcept {
        return static_cast<uint32>(workers.size());
    }

private:
    void WorkerMain();

    std::mutex              mutex;
    std::condition_variable work_cv;
    std::condition_variable batch_cv;
    std::condition_variable stop_cv;
    std::deque<Job>         pending_jobs;
    std::vector<std::jthread> workers;

    size_t             remaining_jobs{0};
    size_t             active_jobs{0};
    std::exception_ptr first_failure;
    bool               accepting{true};
    bool               batch_active{false};
    bool               stop_join_in_progress{false};
    bool               stopped{false};
};

} // namespace Moer::Render

#endif
