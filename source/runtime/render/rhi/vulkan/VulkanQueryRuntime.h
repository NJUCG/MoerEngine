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

    void FinalizeSubmit(uint64 _timeline, VkCommandBuffer _owner_cmd);
    void ResolveCompleted(uint64 _timeline);

    void ResolveAsError(std::span<const QueryToken> _tokens, StringView _reason);

    float GetTimestampPeriod() const {
        return timestamp_period;
    }

private:
    struct PoolSlot {
        QueryKind kind{QueryKind::Timestamp};
        uint32    pool_index{0};
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

    struct PoolInstance {
        UniquePtr<IQueryPoolBackend> backend{};
        uint32                       next_query{0};
        uint32                       pending_slot_uses{0};
        std::thread::id              owner_thread{};
        VkCommandBuffer              owner_cmd{VK_NULL_HANDLE};
        bool                         in_use{false};
    };

    struct RecordContext {
        std::thread::id owner_thread{};
        VkCommandBuffer owner_cmd{VK_NULL_HANDLE};

        int active_timestamp_pool{-1};
        int active_occlusion_pool{-1};

        UnorderedMap<uint64, QueryRecord> records{};
        UnorderedMap<uint64, QueryToken>  issued_tokens{};
        Array<PoolSlot>                   used_slots{};
    };

    struct PendingSubmission {
        uint64            timeline{0};
        Array<QueryRecord> records{};
        Array<PoolSlot>    used_slots{};
    };

private:
    QueryRecord& GetOrCreateRecord(RecordContext& _ctx, const QueryToken& _token);
    PoolSlot     AcquireSlot(QueryKind _kind, RecordContext& _ctx);
    RecordContext* FindActiveContext(VkCommandBuffer _owner_cmd);

    uint64 ResolveTimestampTick(const PoolSlot& _slot);
    uint64 ResolveOcclusionSamples(const PoolSlot& _slot);

    void MarkSlotPendingUse(const PoolSlot& _slot, int _delta);
    void RecyclePoolIfPossible(const PoolSlot& _slot);

    Array<PoolInstance>& GetPools(QueryKind _kind);
    int&                 GetActivePoolIndex(RecordContext& _ctx, QueryKind _kind);
    uint32               GetInitialPoolSize(QueryKind _kind) const;
    UniquePtr<IQueryPoolBackend> CreateBackend(QueryKind _kind, uint32 _capacity);

private:
    static constexpr uint32 k_initial_timestamp_pool_size = 2048;
    static constexpr uint32 k_initial_occlusion_pool_size = 1024;

    VulkanDevice& device;
    float         timestamp_period{0.0f};

    Array<PoolInstance> timestamp_pools{};
    Array<PoolInstance> occlusion_pools{};
    UnorderedMap<QueryKind, PoolBackendFactory> backend_factories{};

    UnorderedMap<VkCommandBuffer, RecordContext> active_contexts{};
    Array<PendingSubmission>                     pending_submissions{};

    mutable std::mutex runtime_mtx{};
};

} // namespace Moer::Render
