#include "VulkanCommandProfiler.h"
#include "VulkanDevice.h"
#include "log/LogSystem.h"
#include "misc/Types.h"

#include <cassert>
#include <chrono>
#include <cstring>

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
    for (uint32 idx = 0; idx < s_max_num_profiler_queries_per_frame / 8; ++idx) {
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

                uint64 begin = query_pool_results[sample.index * 2 * 2 + 0];
                uint64 end   = query_pool_results[(sample.index * 2 + 1) * 2 + 0];
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
        queries_used + s_max_num_profiler_queries_per_frame * cur_frame / 8, 0, s_max_num_profiler_queries_per_frame / 8
    );
}

int ProfilerStorage::GetQueryStorageIndex(std::string_view _name) {
    const std::string name{_name};
    if (const auto found = name2sample.find(name); found != name2sample.end()) {
        return found->second.index;
    }
    if (name2sample.size() >= s_query_max_storage) {
        LOG_ERROR(
            "GPU profiler query-name capacity ({}) exhausted; skipping timestamp scope '{}'.", s_query_max_storage, name
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
    uint32 idx = storage_index * 2 + 0 + cur_frame * s_max_num_profiler_queries_per_frame;
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
    uint32 idx = storage_index * 2 + 1 + cur_frame * s_max_num_profiler_queries_per_frame;
    vkCmdWriteTimestamp(_cmd_list.GetHandle(), _stage, timestamp_pool.GetHandle(), idx);
    SetQueryUsed(idx);
    assert(IsQueryUsed(idx - 1) == true && "Query not used");
}

VulkanCommandProfiler::VulkanCommandProfiler(VulkanDevice& _device, EQueueType _queue_type) :
    timestamp_pool(
        _device,
        VK_QUERY_TYPE_TIMESTAMP,
        s_queue_max_frame_in_flight * s_query_max_storage * 4,
        _queue_type
    ),
    storage(timestamp_pool) {}

void VulkanCommandProfiler::BeginSplitFrame() {
    ResetSplitFrame();
    split_frame.active  = true;
    split_frame.started = std::chrono::steady_clock::now();
}

void VulkanCommandProfiler::EndSplitFrame() {
    if (!split_frame.active) {
        split_frame.active  = true;
        split_frame.started = std::chrono::steady_clock::now();
    }

    const double execute_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - split_frame.started).count();
    storage.RegisterCpuTimestamp("Queue Execution", execute_ms);
    storage.AdvanceFrame();
    ResetSplitFrame();
}

void VulkanCommandProfiler::ResetSplitFrame() noexcept {
    split_frame = {};
}

void VulkanCommandProfiler::FinishRecording(const CmdSubmit& _submit, double _elapsed_ms) noexcept {
    if (_submit.cmds.empty()) {
        return;
    }
    try {
        if (_submit.ProfilingPhase() == ERHIProfilingPhase::Complete) {
            storage.RegisterCpuTimestamp("Queue Execution", _elapsed_ms);
            storage.AdvanceFrame();
        } else if (_submit.EndsProfilingFrame()) {
            EndSplitFrame();
        }
    } catch (...) {
        // Optional CPU statistics cannot invalidate an already recorded packet.
        ResetSplitFrame();
        try {
            LOG_WARNING("[VulkanCommandProfiler] failed to record CPU profiling statistics");
        } catch (...) {
        }
    }
}

ProfileData VulkanCommandProfiler::GetProfilerEntry() {
    std::unique_lock<std::mutex> lock(mutex);
    return cached_entry;
}
} // namespace Moer::Render
