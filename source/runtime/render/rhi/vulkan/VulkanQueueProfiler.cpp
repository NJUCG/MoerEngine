#include "VulkanQueueTiming.h"
#include "VulkanQueue.h"
#include "VulkanDevice.h"
#include "log/LogSystem.h"
#include "rhi/RHIThreadHeartbeat.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace Moer::Render {

ProfilerStorage::ProfilerStorage(VkNativeQueryPool& _pool) : timestamp_pool(_pool) {

    std::memset(queries_used, 0, sizeof(queries_used));
    std::memset(query_pool_results, 0, sizeof(query_pool_results));
    timestamp_period = timestamp_pool.GetDevice().GetCoreProperties().core_1_0.limits.timestampPeriod;
    active           = true;
    name2sample.reserve(100);
}
void ProfilerStorage::CollectProfiling(VkCommandBuffer _cb) {
    if (!active) {
        return;
    }
    bool b_any_query_used = false;
    //maybe should use last frame
    for (uint idx = 0; idx < s_max_num_profiler_queries_per_frame / 8; ++idx) {
        if (queries_used[idx + cur_frame * s_max_num_profiler_queries_per_frame / 8]) {
            b_any_query_used = true;
            break;
        }
    }
    if (b_any_query_used) {
        VkResult result = vkGetQueryPoolResults(
            timestamp_pool.GetDevice().GetDevice(),
            timestamp_pool.GetHandle(),
            s_max_num_profiler_queries_per_frame * cur_frame,
            s_max_num_profiler_queries_per_frame,
            sizeof(query_pool_results),
            query_pool_results,
            sizeof(query_pool_results[0]) * 2, // each result contain two int
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
        );

        if (result != VK_SUCCESS && result != VK_NOT_READY) {
            LOG_ERROR("Failed call to vkGetQueryPoolResults, error code = {}\n", uint64(result));
            b_any_query_used = false;
        }
    }

    if (b_any_query_used) {
        for (auto& [name, sample] : name2sample) {
            // clang-format off
                if (IsQueryUsed (sample.index * 2 + 0 + cur_frame * s_max_num_profiler_queries_per_frame)
                    && IsQueryUsed(sample.index * 2 + 1 + cur_frame * s_max_num_profiler_queries_per_frame)
                    && query_pool_results[sample.index * 2 * 2 + 1] != 0 // first *2 for begin/end, second *2 for query result
                    && query_pool_results[(sample.index * 2 + 1) * 2 + 1] != 0) {
                // clang-format on

                uint64_t begin = query_pool_results[sample.index * 2 * 2 + 0];
                uint64_t end   = query_pool_results[(sample.index * 2 + 1) * 2 + 0];
                sample.Record(end - begin);
            } else {
                // not valid
                sample.Reset();
            }
        }
    } else {
        memset(query_pool_results, 0, sizeof(query_pool_results));
    }

    vkCmdResetQueryPool(
        _cb,
        timestamp_pool.GetHandle(),
        s_max_num_profiler_queries_per_frame * cur_frame,
        s_max_num_profiler_queries_per_frame
    );

    memset(
        queries_used + s_max_num_profiler_queries_per_frame * cur_frame / 8,
        0,
        s_max_num_profiler_queries_per_frame / 8
    );
}

int ProfilerStorage::GetQueryStorageIndex(std::string_view _name) {
    const std::string name{_name};
    if (const auto found = name2sample.find(name); found != name2sample.end()) {
        return found->second.index;
    }
    if (name2sample.size() >= s_query_max_storage) {
        LOG_ERROR(
            "GPU profiler query-name capacity ({}) exhausted; skipping timestamp scope '{}'.",
            s_query_max_storage,
            name
        );
        return -1;
    }
    const int index = static_cast<int>(name2sample.size());
    name2sample.emplace(name, Sample(index));
    return index;
}

void ProfilerStorage::BeginProfilerSession(
    VulkanCommandList&      _cmd_list,
    std::string_view        _name,
    VkPipelineStageFlagBits _stage
) {
    if (!active) {
        return;
    }
    const int storage_index = GetQueryStorageIndex(_name);
    if (storage_index < 0) {
        return;
    }
    uint idx = storage_index * 2 + 0 + cur_frame * s_max_num_profiler_queries_per_frame;
    vkCmdWriteTimestamp(_cmd_list.GetHandle(), _stage, timestamp_pool.GetHandle(), idx);
    SetQueryUsed(idx);
    assert(IsQueryUsed(idx + 1) == false && "Query already used");
}

void ProfilerStorage::EndProfilerSession(
    VulkanCommandList&      _cmd_list,
    std::string_view        _name,
    VkPipelineStageFlagBits _stage
) {
    if (!active) {
        return;
    }
    const int storage_index = GetQueryStorageIndex(_name);
    if (storage_index < 0) {
        return;
    }
    uint idx = storage_index * 2 + 1 + cur_frame * s_max_num_profiler_queries_per_frame;
    vkCmdWriteTimestamp(_cmd_list.GetHandle(), _stage, timestamp_pool.GetHandle(), idx);
    SetQueryUsed(idx);
    assert(IsQueryUsed(idx - 1) == true && "Query not used");
}

void VkCommandQueue::BeginSplitProfilingCpuFrame() {
    ResetSplitProfilingCpuFrame();
    split_profiling_cpu_frame.active  = true;
    split_profiling_cpu_frame.started = std::chrono::steady_clock::now();
}

void VkCommandQueue::EndSplitProfilingCpuFrame() {
    if (!split_profiling_cpu_frame.active) {
        split_profiling_cpu_frame.active  = true;
        split_profiling_cpu_frame.started = std::chrono::steady_clock::now();
    }

    const double execute_ms = RhiThreadProfileMilliseconds(
        split_profiling_cpu_frame.started, std::chrono::steady_clock::now()
    );
    profiler_storage.RegisterCpuTimestamp("Queue Execution", execute_ms);
    profiler_storage.AdvanceFrame();
    ResetSplitProfilingCpuFrame();
}

void VkCommandQueue::ResetSplitProfilingCpuFrame() {
    split_profiling_cpu_frame = {};
}

ProfileData VkCommandQueue::GetProfilerEntry() {
    std::unique_lock<std::mutex> lock(profiler_mutex);
    return cached_profiler_entry;
}

void VkCommandQueue::RecordThreadingProfile(
    ERhiWorkKind _kind,
    double       _caller_ms,
    double       _queue_wait_ms,
    double       _work_ms,
    uint32       _enqueue_depth
) {
    if (!thread_profile) {
        return;
    }

    RhiThreadProfileState& profile = *thread_profile;
    ++profile.samples;
    RhiWorkProfileTotals& kind_profile = _kind == ERhiWorkKind::Execute
                                            ? profile.execute
                                            : profile.present;
    ++kind_profile.samples;
    kind_profile.caller_total_ms += _caller_ms;
    kind_profile.queue_wait_total_ms += _queue_wait_ms;
    kind_profile.work_total_ms += _work_ms;
    profile.caller_total_ms += _caller_ms;
    profile.caller_max_ms = std::max(profile.caller_max_ms, _caller_ms);
    profile.queue_wait_total_ms += _queue_wait_ms;
    profile.queue_wait_max_ms = std::max(profile.queue_wait_max_ms, _queue_wait_ms);
    profile.work_total_ms += _work_ms;
    profile.work_max_ms = std::max(profile.work_max_ms, _work_ms);
    profile.max_enqueue_depth = std::max(profile.max_enqueue_depth, _enqueue_depth);

    const auto   now       = std::chrono::steady_clock::now();
    const double window_ms = RhiThreadProfileMilliseconds(profile.window_start, now);
    if (window_ms < 1000.0) {
        return;
    }

    const uint64 submitted_timeline = last_frame.load(std::memory_order_acquire);
    const uint64 completed_timeline = cpu_settled_frame.load(std::memory_order_acquire);
    const uint64 gpu_pending = submitted_timeline > completed_timeline
                                   ? submitted_timeline - completed_timeline
                                   : 0;
    auto average = [](double _total, uint64 _samples) {
        return _samples == 0 ? 0.0 : _total / double(_samples);
    };
    LOG_INFO(
        "[ThreadingProfile][RHI] queue={} mode={} window_ms={:.3f} samples={} execute={} "
        "present={} caller_avg_ms={:.3f} caller_max_ms={:.3f} queue_wait_avg_ms={:.3f} "
        "queue_wait_max_ms={:.3f} work_avg_ms={:.3f} work_max_ms={:.3f} "
        "execute_caller_avg_ms={:.3f} execute_wait_avg_ms={:.3f} execute_work_avg_ms={:.3f} "
        "present_caller_avg_ms={:.3f} present_wait_avg_ms={:.3f} present_work_avg_ms={:.3f} "
        "max_enqueue_depth={} gpu_pending={}",
        queue.GetType() == EQueueType::Graphics ? "Graphics" : "Compute",
        rhi_thread_enabled ? "threaded" : "synchronous",
        window_ms,
        profile.samples,
        profile.execute.samples,
        profile.present.samples,
        profile.caller_total_ms / double(profile.samples),
        profile.caller_max_ms,
        profile.queue_wait_total_ms / double(profile.samples),
        profile.queue_wait_max_ms,
        profile.work_total_ms / double(profile.samples),
        profile.work_max_ms,
        average(profile.execute.caller_total_ms, profile.execute.samples),
        average(profile.execute.queue_wait_total_ms, profile.execute.samples),
        average(profile.execute.work_total_ms, profile.execute.samples),
        average(profile.present.caller_total_ms, profile.present.samples),
        average(profile.present.queue_wait_total_ms, profile.present.samples),
        average(profile.present.work_total_ms, profile.present.samples),
        profile.max_enqueue_depth,
        gpu_pending
    );

    profile.window_start        = now;
    profile.samples             = 0;
    profile.execute             = {};
    profile.present             = {};
    profile.caller_total_ms     = 0.0;
    profile.caller_max_ms       = 0.0;
    profile.queue_wait_total_ms = 0.0;
    profile.queue_wait_max_ms   = 0.0;
    profile.work_total_ms       = 0.0;
    profile.work_max_ms         = 0.0;
    profile.max_enqueue_depth   = 0;
}

} // namespace Moer::Render
