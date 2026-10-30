#pragma once

#include "VulkanCommandList.h"
#include "VulkanCommandQueries.h"
#include "misc/Types.h"
#include "rhi/RHICommand.h"

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>

namespace Moer::Render {

inline constexpr uint32 s_queue_max_frame_in_flight = 3;
inline constexpr uint32 s_query_max_storage         = 8 * 64;

struct ProfilerStorage {
    static constexpr int s_max_num_profiler_queries_per_frame = s_query_max_storage * 2; // *2 is for begin&end
    static constexpr int s_total_query_count = s_max_num_profiler_queries_per_frame * s_queue_max_frame_in_flight;
    // These stages are part of the serial query golden contract.  Keep the
    // actual timestamp writes and the diagnostic event sourced from the same
    // constants so they cannot silently drift apart.
    static constexpr VkPipelineStageFlagBits kBeginTimestampStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    static constexpr VkPipelineStageFlagBits kEndTimestampStage   = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    static constexpr VkPipelineStageFlags2   kResetQueryStage     = VK_PIPELINE_STAGE_2_NONE;

    ProfilerStorage(VkNativeQueryPool& _timestamp_pool);
    void CollectProfiling(VkCommandBuffer _cmd);
    int  GetQueryStorageIndex(std::string_view _name);
    bool IsQueryUsed(int _idx) {
        return queries_used[_idx / 8] & (1 << (_idx % 8));
    }
    void SetQueryUsed(int _idx) {
        queries_used[_idx / 8] |= (1 << (_idx % 8));
    }
    bool IsActive() const {
        return active;
    }
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

    bool               active = false;
    VkNativeQueryPool& timestamp_pool;
    float              timestamp_period = 0.0f;
    uint64             query_pool_results[s_max_num_profiler_queries_per_frame * 2];
    StaticArray<UnorderedMap<std::string_view, double>, s_queue_max_frame_in_flight> cpu_timestamps;
    // each result contain two int(VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
    // first int contains the queried timestamp
    // next(last) int contains availability of the timestamp, available if nonzero

    struct Sample {
        int                           index;
        uint64                        accumulated;
        size_t                        next_idx_to_store;
        static constexpr int          s_range = 13;
        std::array<uint64, s_range>   deltas;

        Sample(int _idx) : index(_idx), accumulated(0), next_idx_to_store(0) {
            std::memset(deltas.data(), 0, sizeof(deltas));
        }
        Sample() : Sample(0) {}

        void Reset() {
            next_idx_to_store = 0;
            accumulated       = 0;
            std::memset(deltas.data(), 0, sizeof(deltas));
        }
        void Record(uint64 _value) {
            next_idx_to_store = (next_idx_to_store + 1) % s_range;
            accumulated -= deltas[next_idx_to_store];
            deltas[next_idx_to_store] = _value;
            accumulated += _value;
        }
    };

    UnorderedMap<std::string, Sample> name2sample;
    ubyte                             queries_used[s_total_query_count / 8];

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

    VkNativeQueryPool timestamp_pool;
    ProfilerStorage   storage;
    ProfileData       cached_entry;
    std::mutex        mutex;

    struct SplitFrame {
        bool                                  active{false};
        std::chrono::steady_clock::time_point started{};
    } split_frame;
};

} // namespace Moer::Render
