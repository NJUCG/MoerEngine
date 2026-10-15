#pragma once

#include "VulkanCommon.h"
#include "VulkanFault.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIQuery.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace Moer::Render {

class VulkanCmdList;
class VulkanDevice;
struct QueryCmd;

class VkNativeQueryPool {
public:
    VkNativeQueryPool(VulkanDevice& _device, VkQueryType _type, uint32 _count, EQueueType _queue_type);
    ~VkNativeQueryPool();
    VkNativeQueryPool(const VkNativeQueryPool&)            = delete;
    VkNativeQueryPool& operator=(const VkNativeQueryPool&) = delete;

    VkQueryPool GetHandle() const {
        return query_pool;
    }
    uint32 GetCount() const {
        return count;
    }
    [[nodiscard]] VkResult GetResults(
        std::span<uint64>  _results,
        uint32             _first_query,
        uint32             _query_count,
        VkQueryResultFlags _flags
    );
    VulkanDevice& GetDevice() const {
        return device;
    }

private:
    friend class VulkanCommandQueries;

    [[nodiscard]] VkQueryPool CreatePool(uint32 _query_count);
    // The query owner may grow or host-reset an idle pool only before
    // command-buffer recording starts. Keeping both operations private makes
    // its single-owner synchronization contract explicit.
    void EnsureCapacity(uint32 _required_count);
    void ResetHost(uint32 _first_query, uint32 _query_count);

    VulkanDevice& device;
    VkQueryPool   query_pool{VK_NULL_HANDLE};
    uint32        count{0};
    VkQueryType   type;
    EQueueType    queue_type{EQueueType::Ignore};
};

// Query pools, slots, tokens, and prepared results for one command recording.
// This object stays with the command context through Submission and Completion.
class VulkanCommandQueries {
public:
    VulkanCommandQueries(VulkanDevice& _device, EQueueType _queue_type);
    VulkanCommandQueries(const VulkanCommandQueries&)            = delete;
    VulkanCommandQueries& operator=(const VulkanCommandQueries&) = delete;

    // Reserve before recording: timestamps use one slot per boundary;
    // occlusion begin/end pairs share one slot.
    void EnsureQueryCapacity(size_t _timestamp_slot_count, size_t _occlusion_pair_count);
    // A recycled command context can switch native/degraded timestamp capability
    // only before recording its first query boundary.
    void
    ConfigureTimestampCapabilityForRecording(uint32 _valid_bits, EVulkanTimestampQueryResetMode _reset_mode);
    void RecordQuery(VulkanCmdList& _cmd_list, const QueryCmd& _cmd);

    [[nodiscard]] bool HasQueries() const noexcept {
        return !timestamp_query_records.empty() || !occlusion_query_records.empty();
    }

    // Prepare reads native results without publishing. The queue retires
    // submission signals and command context callbacks before publishing the batch,
    // then releases query callbacks only after every result is published.
    [[nodiscard]] VkResult PrepareQueriesAfterGpuCompletion(const VulkanOperationContext& _context) noexcept;

    void PublishQueriesAfterGpuCompletion(
        QueryPublishBatch _batch,
        bool              _gpu_success,
        std::string_view  _failure_reason
    ) noexcept;
    void NotifyQueriesAfterGpuCompletion(QueryPublishBatch _batch) noexcept;

    // Clear CPU records after command-pool reset; retain native pools for reuse.
    void ClearRecords() noexcept;

private:
    struct TimestampQueryRecord {
        enum class EPreparedState : uint8 {
            Unprepared,
            Ready,
            InvalidValidBits,
            Incomplete,
            Unavailable,
            NativeFault,
        };

        QueryToken           token{};
        uint32               begin_slot{std::numeric_limits<uint32>::max()};
        uint32               end_slot{std::numeric_limits<uint32>::max()};
        EPreparedState       prepared_state{EPreparedState::Unprepared};
        TimestampQueryResult prepared_result{};
    };

    struct OcclusionQueryRecord {
        enum class EPreparedState : uint8 {
            Unprepared,
            Ready,
            Incomplete,
            Unavailable,
            NativeFault,
        };

        QueryToken           token{};
        uint32               slot{std::numeric_limits<uint32>::max()};
        bool                 ended{false};
        bool                 precise{false};
        EPreparedState       prepared_state{EPreparedState::Unprepared};
        OcclusionQueryResult prepared_result{};
    };

    // Timestamp recording and completion.
    TimestampQueryRecord* FindTimestampQueryRecord(uint64 _token_id) noexcept;
    void                  EnsureTimestampQueryCapacity(size_t _required_count);
    uint32                AllocateTimestampQuerySlot();
    void                  RecordTimestampQuery(VulkanCmdList& _cmd_list, const QueryCmd& _cmd);

    [[nodiscard]] VkResult
    PrepareTimestampQueriesAfterGpuCompletion(const VulkanOperationContext& _context) noexcept;

    void PublishTimestampQueriesAfterGpuCompletion(
        QueryPublishBatch _batch,
        bool              _gpu_success,
        std::string_view  _failure_reason
    ) noexcept;
    void NotifyTimestampQueriesAfterGpuCompletion(QueryPublishBatch _batch) noexcept;

    // Occlusion recording and completion.
    OcclusionQueryRecord* FindOcclusionQueryRecord(uint64 _token_id) noexcept;
    void                  EnsureOcclusionQueryCapacity(size_t _required_count);
    uint32                AllocateOcclusionQuerySlot();
    void                  RecordOcclusionQuery(VulkanCmdList& _cmd_list, const QueryCmd& _cmd);

    [[nodiscard]] VkResult
    PrepareOcclusionQueriesAfterGpuCompletion(const VulkanOperationContext& _context) noexcept;

    void PublishOcclusionQueriesAfterGpuCompletion(
        QueryPublishBatch _batch,
        bool              _gpu_success,
        std::string_view  _failure_reason
    ) noexcept;
    void NotifyOcclusionQueriesAfterGpuCompletion(QueryPublishBatch _batch) noexcept;

    VulkanDevice& device;
    EQueueType    queue_type;

    Array<TimestampQueryRecord>    timestamp_query_records;
    Array<OcclusionQueryRecord>    occlusion_query_records;
    uint32                         next_timestamp_query{0};
    uint32                         next_occlusion_query{0};
    uint32                         timestamp_valid_bits{0};
    EVulkanTimestampQueryResetMode timestamp_reset_mode{EVulkanTimestampQueryResetMode::Unsupported};
    double                         timestamp_period_ns{0.0};
    // Most recordings never contain an explicit Query command. Lazily creating
    // this pool avoids adding a native driver object to every ordinary,
    // parallel-worker, fallback, and Copy command context.
    std::optional<VkNativeQueryPool> timestamp_pool;
    std::optional<VkNativeQueryPool> occlusion_pool;
};

} // namespace Moer::Render
