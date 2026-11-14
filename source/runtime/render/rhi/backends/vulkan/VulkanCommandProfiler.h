#pragma once

#include "VulkanCommandList.h"
#include "VulkanCommandQueries.h"
#include "misc/Types.h"
#include "rhi/RHICommand.h"

#include <array>
#include <chrono>
#include <bitset>
#include <mutex>

namespace Moer::Render {

inline constexpr uint32 s_queue_max_frame_in_flight = 3;
inline constexpr uint32 s_query_max_storage         = 8 * 64;

// Timestamp slots, rolling GPU samples and per-frame CPU timings for one queue.
struct VulkanTimestampProfiler {
    // Fixed query capacity and serial timestamp stage contract.
    static constexpr int s_max_num_profiler_queries_per_frame = s_query_max_storage * 2; // *2 is for begin&end
    // These stages are part of the serial query golden contract.  Keep the
    // actual timestamp writes and the diagnostic event sourced from the same
    // constants so they cannot silently drift apart.
    static constexpr VkPipelineStageFlagBits kBeginTimestampStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    static constexpr VkPipelineStageFlagBits kEndTimestampStage   = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    // Query-slot recording and completed frame collection.
    VulkanTimestampProfiler(VkNativeQueryPool& _timestamp_pool);
    void CollectProfiling(VkCommandBuffer _cmd);
    int  GetQueryStorageIndex(std::string_view _name);
    bool IsQueryUsed(int _idx) {
        return queries_used[_idx / s_max_num_profiler_queries_per_frame].test(_idx % s_max_num_profiler_queries_per_frame);
    }
    void SetQueryUsed(int _idx) {
        queries_used[_idx / s_max_num_profiler_queries_per_frame].set(_idx % s_max_num_profiler_queries_per_frame);
    }

    // CPU timings and presentation of the previous profiling frame.
    void RegisterCpuTimestamp(std::string_view _name, double _timestamp) {
        cpu_timestamps[cur_frame][_name] = _timestamp;
    }
    void AdvanceFrame() {
        cur_frame = (cur_frame + 1) % s_queue_max_frame_in_flight;
    }
    ProfileData GetProfilerEntry() {
        ProfileData               data{};
        Array<ProfileResultEntry> entries;
        entries.reserve(name2sample.size());
        uint32 last_frame = (cur_frame + s_queue_max_frame_in_flight - 1) % s_queue_max_frame_in_flight;
        for (auto& [name, sample] : name2sample) {
            if (sample.accumulated > 0)
                entries.push_back({name, double(sample.accumulated) / (Sample::s_range * 1e6) * timestamp_period});
        }
        data.gpu_entries = std::move(entries);
        //cpu timestamp
        data.cpu_entries.reserve(cpu_timestamps[last_frame].size());
        for (auto& [name, timestamp] : cpu_timestamps[last_frame]) {
            data.cpu_entries.emplace_back(name.data(), timestamp);
        }
        return data;
    }

    void BeginProfilerSession(
        VulkanCommandList&      _cmd,
        std::string_view        _name,
        VkPipelineStageFlagBits _stage = kBeginTimestampStage
    );
    void EndProfilerSession(
        VulkanCommandList&      _cmd,
        std::string_view        _name,
        VkPipelineStageFlagBits _stage = kEndTimestampStage
    );

    // Borrowed pool and native result storage.
    VkNativeQueryPool& timestamp_pool;
    float              timestamp_period = 0.0f;
    std::array<uint64, s_max_num_profiler_queries_per_frame * 2> query_pool_results{};
    StaticArray<UnorderedMap<std::string_view, double>, s_queue_max_frame_in_flight> cpu_timestamps;
    // each result contain two int(VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
    // first int contains the queried timestamp
    // next(last) int contains availability of the timestamp, available if nonzero

    // Sliding window of completed GPU durations for one named scope.
    struct Sample {
        int                           index;
        uint64                        accumulated{0};
        size_t                        next_idx_to_store{0};
        static constexpr int          s_range = 13;
        std::array<uint64, s_range>   deltas{};

        Sample(int _idx) : index(_idx) {}
        Sample() : Sample(0) {}

        void Reset() {
            next_idx_to_store = 0;
            accumulated       = 0;
            deltas.fill(0);
        }
        void Record(uint64 _value) {
            next_idx_to_store = (next_idx_to_store + 1) % s_range;
            accumulated -= deltas[next_idx_to_store];
            deltas[next_idx_to_store] = _value;
            accumulated += _value;
        }
    };

    // Stable scope identities and per-frame query usage.
    UnorderedMap<std::string, Sample> name2sample;
    std::array<std::bitset<s_max_num_profiler_queries_per_frame>, s_queue_max_frame_in_flight> queries_used{};

    uint64 cur_frame = 0;
};

// Recording statistics persist across submissions of one logical queue.
class VulkanCommandProfiler {
public:
    VulkanCommandProfiler(VulkanDevice& _device, EQueueType _queue_type);

    void        BeginSplitFrame();
    void        EndSplitFrame();
    void        ResetSplitFrame() noexcept;
    void        FinishRecording(const CmdSubmit& _submit, double _elapsed_ms) noexcept;
    ProfileData GetProfilerEntry();

private:
    friend class VulkanCommandRecorder;

    // Native query storage precedes its borrower in construction order.
    VkNativeQueryPool timestamp_pool;
    VulkanTimestampProfiler   storage;
    ProfileData       cached_entry;
    std::mutex        mutex;

    struct SplitFrame {
        bool                                  active{false};
        std::chrono::steady_clock::time_point started{};
    } split_frame;
};

} // namespace Moer::Render
