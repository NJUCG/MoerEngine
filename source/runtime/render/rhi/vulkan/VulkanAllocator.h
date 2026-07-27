#pragma once
#include "VulkanCommon.h"
#include "RHICmdReorderer.h"
#include "VulkanCommand.h"
#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"
#include "VulkanResourceTracker.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIQuery.h"

#include <limits>

namespace Moer::Render {

struct QueryCmd;

class VulkanCmdAllocator : public VulkanDeviceObject {
private:
    VkQueueFlags  queue_type;
    VkCommandPool command_pool;

public:
    VulkanCmdAllocator(VulkanDevice* _device, EQueueType _queue_type);
    ~VulkanCmdAllocator();
    VkCommandPool GetHandle() const {
        return command_pool;
    }
    VkQueueFlags GetQueueType() const {
        return queue_type;
    }
    std::string_view GetQueueName() const {
        return VK_TYPE_TO_STRING(VkQueueFlagBits, (VkQueueFlagBits)queue_type);
    }
};
enum class EVkInternalBufferUsage {
    Upload,
    Readback,
    Scratch,
    ShaderBuffer,
    ShaderBuffer_Constant
};
struct VkTmpBufferAllocator : VulkanDeviceObject {
    VkTmpBufferAllocator(VulkanDevice* _device);
    uint64 Allocate(uint64 _size, std::string_view _name);
    uint64 Allocate(uint64 _size, EVkInternalBufferUsage _usage);
    void   DeAllocate(uint64 _handle);
};

class VulkanAllocatorBase : public VulkanDeviceObject {
public:
    VulkanAllocatorBase(VulkanDevice* _device, EQueueType _queue_type);
    virtual ~VulkanAllocatorBase();

    VulkanCmdList& GetCmdList() {
        return cmd_list.value();
    }
    VkTracker& GetTracker() {
        return tracker;
    }
    bool ResetCmdList();
    void AddOnComplete(std::function<void()>&& _func) {
        on_complete.push_back(std::move(_func));
    }

    // Completion callbacks are user-extensible and must never escape the
    // Completion owner. A false result quarantines the allocator instead of
    // resetting/reusing partially finalized state.
    virtual bool CompleteSuccess() noexcept;
    virtual bool Reset();
    // A recorder that was never submitted must not run success callbacks.
    // It can still reset and return to the pool after all worker jobs joined.
    bool ResetAbandoned();

protected:
    std::optional<VulkanCmdAllocator> cmd_allocator;
    std::optional<VulkanCmdList>      cmd_list;

    Array<std::function<void()>> on_complete;

    EQueueType queue_type{EQueueType::Ignore};
    VkTracker tracker;
};

class VkNativeQueryPool {
public:
    VkNativeQueryPool(
        VulkanDevice& _device,
        VkQueryType    _type,
        uint32         _count,
        EQueueType     _queue_type
    );
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
    // The owning allocator may grow an idle pool before command-buffer
    // recording starts. Existing pools are never replaced while they are
    // referenced by submitted work.
    void EnsureCapacity(uint32 _required_count);

private:
    [[nodiscard]] VkQueryPool CreatePool(uint32 _query_count);

    VulkanDevice& device;
    VkQueryPool   query_pool{VK_NULL_HANDLE};
    uint32        count{0};
    VkQueryType   type;
    EQueueType    queue_type{EQueueType::Ignore};
};

class VulkanPresentor : public VulkanAllocatorBase {
public:
    VulkanPresentor(VulkanDevice* _device, EQueueType _queue_type);
    virtual ~VulkanPresentor();

    [[nodiscard]] VkSemaphore GetImageReadySemaphore() const noexcept {
        return image_ready_semaphore;
    }

    bool CompleteSuccess() noexcept override;

private:
    VkSemaphore image_ready_semaphore{VK_NULL_HANDLE};
};

class VulkanAllocator : public VulkanAllocatorBase {
    constexpr static uint64_t small_block_size = 64 * 1024;

public:
    VulkanAllocator(VulkanDevice* _device, EQueueType _queue_type);
    virtual ~VulkanAllocator();
    BufferView AllocateUploadBuffer(uint64 _size, uint _align);
    BufferView AllocateReadbackBuffer(uint64 _size, uint _align);

    BufferView AllocateScratch(uint64 _size);
    BufferView AllocateShaderBuffer(uint64 _size);

    void ResetBufferAlloc();

    // Query slots and their tokens are allocator-local. The allocator is the
    // native command-buffer owner and already travels in the atomic Submission
    // -> Completion packet, so query state never needs a global active-
    // recording map or lock. Timestamp boundaries consume one slot each;
    // every occlusion begin/end pair shares one slot.
    void EnsureQueryCapacity(
        size_t _timestamp_slot_count,
        size_t _occlusion_pair_count
    );
    void RecordQuery(VulkanCmdList& _cmd_list, const QueryCmd& _cmd);
    // GPU completion is a two-stage transaction. Prepare performs native
    // readback and fault classification without publishing user callbacks.
    // The queue Completion owner later terminalizes submit signals, invokes
    // allocator retirement callbacks, publishes every query in the batch, and
    // only then releases Query callbacks.
    [[nodiscard]] VkResult PrepareQueriesAfterGpuCompletion(
        const VulkanOperationContext& _context
    ) noexcept;
    void PublishQueriesAfterGpuCompletion(
        QueryPublishBatch _batch,
        bool              _gpu_success,
        std::string_view  _failure_reason
    ) noexcept;
    void NotifyQueriesAfterGpuCompletion(
        QueryPublishBatch _batch
    ) noexcept;
    bool CompleteSuccessCallbacks() noexcept;
    [[nodiscard]] bool HasQueries() const noexcept {
        return !timestamp_query_records.empty() ||
               !occlusion_query_records.empty();
    }

    bool CompleteSuccess() noexcept override;
    bool Reset() override;
    //staging buffer allocate with block strategy
private:
    struct ScratchAllocator : VulkanDeviceObject {
        ScratchAllocator(VulkanDevice* _device, VkTmpBufferAllocator* _allocator);
        uint64 Allocate(uint64 _size);
        void   Deallocate(uint64 _handle);
        void   Reset();

        Array<uint64>         allocated_buffers;
        uint64                alignment;
        VkTmpBufferAllocator* allocator = nullptr;
    };

    struct ShaderBufferAllocator : VulkanDeviceObject {
        ShaderBufferAllocator(VulkanDevice* _device, VkTmpBufferAllocator* _allocator);
        uint64 Allocate(uint64 _size);
        void   Deallocate(uint64 _handle);
        void   Reset();

        Array<uint64>         allocated_buffers;
        uint64                alignment;
        VkTmpBufferAllocator* allocator = nullptr;
    };
    struct StackAllocator {
        uint64 init_capacity;
        uint64 capacity;
        uint64 stack_memory_id;
        double growth_factor;
        struct Chunk {
            uint64 handle;
            uint64 offset;
        };
        struct Buffer {
            uint64 handle;
            uint64 size;
            uint64 offset;
        };
        StackAllocator(VkTmpBufferAllocator*, uint64 _init_capacity, double _growth_factor);

        VkTmpBufferAllocator* allocator;
        Array<Buffer>         allocated_buffers;
        Chunk                 Allocate(uint64 _size, uint _align);
        Chunk                 Allocate(uint64 _size);
        void                  Reset();
        void                  Dispose();
        std::string           GetStackBufferName();
    };
    Array<VulkanBuffer*> large_buffers;
    VkTmpBufferAllocator allocator;

    StackAllocator upload_allocator;
    StackAllocator readback_allocator;

    ScratchAllocator      scratch_allocator;
    ShaderBufferAllocator shader_buffer_allocator;

    struct TimestampQueryRecord {
        enum class EPreparedState : uint8 {
            Unprepared,
            Ready,
            InvalidValidBits,
            Incomplete,
            Unavailable,
            NativeFault,
        };

        QueryToken token{};
        uint32     begin_slot{std::numeric_limits<uint32>::max()};
        uint32     end_slot{std::numeric_limits<uint32>::max()};
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

        QueryToken token{};
        uint32     slot{std::numeric_limits<uint32>::max()};
        bool       ended{false};
        bool       precise{false};
        EPreparedState       prepared_state{EPreparedState::Unprepared};
        OcclusionQueryResult prepared_result{};
    };

    void EnsureTimestampQueryCapacity(size_t _required_count);
    void EnsureOcclusionQueryCapacity(size_t _required_count);
    void RecordTimestampQuery(VulkanCmdList& _cmd_list, const QueryCmd& _cmd);
    void RecordOcclusionQuery(VulkanCmdList& _cmd_list, const QueryCmd& _cmd);
    TimestampQueryRecord* FindTimestampQueryRecord(uint64 _token_id) noexcept;
    OcclusionQueryRecord* FindOcclusionQueryRecord(uint64 _token_id) noexcept;
    uint32                AllocateTimestampQuerySlot();
    uint32                AllocateOcclusionQuerySlot();
    [[nodiscard]] VkResult PrepareTimestampQueriesAfterGpuCompletion(
        const VulkanOperationContext& _context
    ) noexcept;
    [[nodiscard]] VkResult PrepareOcclusionQueriesAfterGpuCompletion(
        const VulkanOperationContext& _context
    ) noexcept;
    void PublishTimestampQueriesAfterGpuCompletion(
        QueryPublishBatch _batch,
        bool              _gpu_success,
        std::string_view  _failure_reason
    ) noexcept;
    void PublishOcclusionQueriesAfterGpuCompletion(
        QueryPublishBatch _batch,
        bool              _gpu_success,
        std::string_view  _failure_reason
    ) noexcept;
    void NotifyTimestampQueriesAfterGpuCompletion(
        QueryPublishBatch _batch
    ) noexcept;
    void NotifyOcclusionQueriesAfterGpuCompletion(
        QueryPublishBatch _batch
    ) noexcept;
    void ClearQueries() noexcept;

    Array<TimestampQueryRecord> timestamp_query_records;
    Array<OcclusionQueryRecord> occlusion_query_records;
    uint32                      next_timestamp_query{0};
    uint32                      next_occlusion_query{0};
    uint32                      timestamp_valid_bits{0};
    double                      timestamp_period_ns{0.0};
    // Most allocators never record an explicit Query command. Lazily creating
    // this pool avoids adding a native driver object to every ordinary,
    // parallel-worker, fallback, and Copy allocator.
    std::optional<VkNativeQueryPool> timestamp_pool;
    std::optional<VkNativeQueryPool> occlusion_pool;
};
} // namespace Moer::Render
