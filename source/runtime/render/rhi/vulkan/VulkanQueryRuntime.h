#pragma once

#include "VulkanAllocator.h"
#include "VulkanCommand.h"
#include "rhi/RHICommand.h"

#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace Moer::Render {

struct QueryCmd;

class VulkanQueryRuntime {
public:
    struct SubmissionState;

    class IQueryPoolBackend {
    public:
        virtual ~IQueryPoolBackend() = default;
        virtual QueryKind      Kind() const                 = 0;
        virtual uint32         Capacity() const             = 0;
        virtual VkNativeQueryPool& NativePool()             = 0;
        virtual const VkNativeQueryPool& NativePool() const = 0;
    };

    class VulkanTimestampPoolBackend final : public IQueryPoolBackend {
    public:
        VulkanTimestampPoolBackend(VulkanDevice& _device, uint32 _capacity);
        QueryKind Kind() const override {
            return QueryKind::Timestamp;
        }
        uint32 Capacity() const override {
            return capacity;
        }
        VkNativeQueryPool& NativePool() override {
            return *pool;
        }
        const VkNativeQueryPool& NativePool() const override {
            return *pool;
        }

    private:
        UniquePtr<VkNativeQueryPool> pool{};
        uint32                       capacity{0};
    };

    class VulkanOcclusionPoolBackend final : public IQueryPoolBackend {
    public:
        VulkanOcclusionPoolBackend(VulkanDevice& _device, uint32 _capacity);
        QueryKind Kind() const override {
            return QueryKind::Occlusion;
        }
        uint32 Capacity() const override {
            return capacity;
        }
        VkNativeQueryPool& NativePool() override {
            return *pool;
        }
        const VkNativeQueryPool& NativePool() const override {
            return *pool;
        }

    private:
        UniquePtr<VkNativeQueryPool> pool{};
        uint32                       capacity{0};
    };

    using PoolBackendFactory = std::function<UniquePtr<IQueryPoolBackend>(VulkanDevice&, uint32)>;

    explicit VulkanQueryRuntime(VulkanDevice& _device);
    ~VulkanQueryRuntime() = default;

    void RegisterPoolBackendFactory(QueryKind _kind, PoolBackendFactory _factory);

    void BeginRecord(VulkanCmdList& _cmd_list, std::span<const QueryToken> _issued_tokens);
    void EndRecord();

    void HandleQueryCommand(VulkanCmdList& _cmd_list, const QueryCmd& _cmd);

    void BeginTimestamp(VulkanCmdList& _cmd_list, const QueryToken& _token);
    void EndTimestamp(VulkanCmdList& _cmd_list, const QueryToken& _token);

    void BeginOcclusion(VulkanCmdList& _cmd_list, const QueryToken& _token);
    void EndOcclusion(VulkanCmdList& _cmd_list, const QueryToken& _token);

    std::optional<SubmissionState> FinalizeSubmit(uint64 _timeline, VkCommandBuffer _owner_cmd);
    void                          ResolveCompleted(SubmissionState&& _submission);

    void ResolveAsError(std::span<const QueryToken> _tokens, StringView _reason);

    float GetTimestampPeriod() const {
        return timestamp_period;
    }

private:
    struct PoolInstance {
        UniquePtr<IQueryPoolBackend> backend{};
        uint32                       next_query{0};
    };

    struct PoolSlot {
        QueryKind kind{QueryKind::Timestamp};
        PoolInstance* pool{nullptr};
        uint32    query_index{0};
    };

    struct QueryRecord {
        QueryToken token{};
        QueryKind  kind{QueryKind::Timestamp};
        String name{};

        std::optional<PoolSlot> begin_slot{};
        std::optional<PoolSlot> end_slot{};
        std::optional<PoolSlot> occlusion_slot{};
    };

public:
    struct SubmissionState {
        uint64                      timeline{0};
        Array<QueryRecord>          records{};
        Array<UniquePtr<PoolInstance>> timestamp_pools{};
        Array<UniquePtr<PoolInstance>> occlusion_pools{};

        bool HasAllocatedPools() const {
            return !timestamp_pools.empty() || !occlusion_pools.empty();
        }

        bool HasWork() const {
            return !records.empty() || HasAllocatedPools();
        }
    };

private:

    struct RecordContext {
        VkCommandBuffer owner_cmd{VK_NULL_HANDLE};

        PoolInstance* active_timestamp_pool{nullptr};
        PoolInstance* active_occlusion_pool{nullptr};
        SubmissionState submission{};

        UnorderedMap<uint64, QueryRecord> records{};
        UnorderedMap<uint64, QueryToken>  issued_tokens{};
    };

private:
    QueryRecord& GetOrCreateRecord(RecordContext& _ctx, const QueryToken& _token);
    PoolSlot     AcquireSlot(QueryKind _kind, RecordContext& _ctx);
    RecordContext* FindActiveContext(VkCommandBuffer _owner_cmd);

    uint64 ResolveTimestampTick(const PoolSlot& _slot);
    uint64 ResolveOcclusionSamples(const PoolSlot& _slot);

    Array<UniquePtr<PoolInstance>>& GetAvailablePools(QueryKind _kind);
    Array<UniquePtr<PoolInstance>>& GetSubmissionPools(SubmissionState& _submission, QueryKind _kind);
    PoolInstance*&                  GetActivePool(RecordContext& _ctx, QueryKind _kind);
    UniquePtr<IQueryPoolBackend>    CreateBackend(QueryKind _kind, uint32 _capacity);
    UniquePtr<PoolInstance>         AcquirePoolChunk(QueryKind _kind);
    void                            RecycleSubmissionPools(SubmissionState& _submission);

private:
    static constexpr uint32 k_query_pool_chunk_size = 256;

    VulkanDevice& device;
    float         timestamp_period{0.0f};

    Array<UniquePtr<PoolInstance>> available_timestamp_pools{};
    Array<UniquePtr<PoolInstance>> available_occlusion_pools{};
    UnorderedMap<QueryKind, PoolBackendFactory> backend_factories{};

    UnorderedMap<VkCommandBuffer, RecordContext> active_contexts{};

    mutable std::mutex runtime_mtx{};
};

} // namespace Moer::Render
