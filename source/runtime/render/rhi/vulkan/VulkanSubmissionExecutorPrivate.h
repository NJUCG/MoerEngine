#pragma once

#include "VulkanSubmissionExecutor.h"

#include "VulkanAllocator.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanInterruptRuntime.h"
#include "VulkanQueue.h"
#include "VulkanRHIResource.h"
#include "VulkanRHITrace.h"
#include "VulkanSubmissionRuntime.h"
#include "VulkanTranslateTask.h"
#include "RHICmdReorderer.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHI.h"
#include "rhi/RHIImpl.h"
#include "taskgraph/TaskPipe.h"
#include "trace/Trace.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vulkan/vulkan_core.h>

namespace Moer::Render {

enum class ETrackedResourceType : uint8 {
    Buffer,
    Texture,
    Bindless,
    Accel
};

struct ResourceKey {
    ETrackedResourceType type{ETrackedResourceType::Buffer};
    uint64               handle{0};
    uint8                mip_level{0};
    uint8                mip_count{kRemainingSubresource};
    uint8                array_layer{0};
    uint8                array_count{kRemainingSubresource};

    bool operator==(const ResourceKey& other) const {
        if (type != other.type || handle != other.handle) {
            return false;
        }
        if (type != ETrackedResourceType::Texture) {
            return true;
        }
        return mip_level == other.mip_level && mip_count == other.mip_count &&
               array_layer == other.array_layer && array_count == other.array_count;
    }
};

struct ResourceKeyHash {
    size_t operator()(const ResourceKey& key) const {
        size_t hash = std::hash<uint64>{}(key.handle);
        hash ^= static_cast<size_t>(key.type) + 0x9e3779b9 + (hash << 6u) + (hash >> 2u);
        if (key.type == ETrackedResourceType::Texture) {
            const uint32 range_bits =
                (uint32(key.mip_level) << 24) | (uint32(key.mip_count) << 16) |
                (uint32(key.array_layer) << 8) | uint32(key.array_count);
            hash ^= std::hash<uint32>{}(range_bits) + 0x9e3779b9 + (hash << 6u) + (hash >> 2u);
        }
        return hash;
    }
};

struct ResourceAccessDigestEntry {
    bool read{false};
    bool write{false};
    bool last_access_write{false};
    std::optional<EBufferState>  buffer_state{};
    std::optional<ETextureState> texture_state{};
};

using ResourceAccessDigest = UnorderedMap<ResourceKey, ResourceAccessDigestEntry, ResourceKeyHash>;
using ResourceStateSnapshot = UnorderedMap<ResourceKey, ResourceStateValue, ResourceKeyHash>;
using DirtyWrittenResources = UnorderedSet<ResourceKey, ResourceKeyHash>;

struct ExecutorSubmitOp {
    Array<CmdSubmit> submits;
    EQueueType       queue{EQueueType::Graphics};

    ExecutorSubmitOp() = default;
    ExecutorSubmitOp(ExecutorSubmitOp&&) noexcept            = default;
    ExecutorSubmitOp& operator=(ExecutorSubmitOp&&) noexcept = default;
    ExecutorSubmitOp(const ExecutorSubmitOp&)                = delete;
    ExecutorSubmitOp& operator=(const ExecutorSubmitOp&)     = delete;
};

using ExecutorOp = std::variant<ExecutorSubmitOp, ExecutorPresentOp>;

struct TranslateInfo {
    SubmissionKey    key{};
    SourceSubmitKey  source_key{};
    EQueueType       queue{EQueueType::Ignore};
    RHISubmitSegment segment{};
    bool             include{false};

    ResourceStateSnapshot initial_state_snapshot{};
    ResourceStateSnapshot last_state_snapshot{};
    ResourceAccessDigest  digest{};
    std::optional<EQueueType> prefix_transfer_queue{};
    std::optional<EQueueType> suffix_transfer_queue{};
    Array<ImportTexture> prefix_import_textures{};
    Array<ImportBuffer>  prefix_import_buffers{};
    Array<ExportTexture> suffix_export_textures{};
    Array<ExportBuffer>  suffix_export_buffers{};
    GraphEventArray      task_dependencies{};
    GraphEventRef        completion_event{nullptr};
};

struct PreprocessDependencyState {
    GraphEventRef last_fence_event{nullptr};
    GraphEventRef last_translate_event{nullptr};
};

struct SourceSubmitSegmentPlan {
    SubmissionKey key{};
    EQueueType    queue{EQueueType::Ignore};
    bool          inherit_source_wait_events{false};
    bool          inherit_source_signal_events_and_callbacks{false};
    bool          inherit_source_runtime_payload{false};
    bool          include{false};
};

struct SourceSubmitPlan {
    SourceSubmitKey source_key{};
    EQueueType      parent_queue{EQueueType::Ignore};
    Array<SourceSubmitSegmentPlan> segments{};
};

struct PresentCandidateMetadata {
    uint64             op_seq{0};
    bool               has_source_texture_state{false};
    ResourceStateValue source_texture_state{};
};

struct LogicalDependencyGraph {
    UnorderedMap<SubmissionKey, Array<SubmissionKey>, SubmissionKeyHash> producer_keys_by_consumer{};
    UnorderedMap<SubmissionKey, UnorderedSet<SubmissionKey, SubmissionKeyHash>, SubmissionKeyHash>
        dedup_keys_by_consumer{};

    void Reserve(uint32 count) {
        producer_keys_by_consumer.reserve(count);
        dedup_keys_by_consumer.reserve(count);
    }

    void AddEdge(const SubmissionKey& consumer_key, const SubmissionKey& producer_key) {
        auto& dedup_keys = dedup_keys_by_consumer[consumer_key];
        if (!dedup_keys.emplace(producer_key).second) {
            return;
        }
        producer_keys_by_consumer[consumer_key].emplace_back(producer_key);
    }

    void SortEdges();

    const Array<SubmissionKey>* FindProducers(const SubmissionKey& consumer_key) const {
        const auto iter = producer_keys_by_consumer.find(consumer_key);
        if (iter == producer_keys_by_consumer.end()) {
            return nullptr;
        }
        return &iter->second;
    }

    uint32 Count(const SubmissionKey& consumer_key) const {
        const auto iter = producer_keys_by_consumer.find(consumer_key);
        if (iter == producer_keys_by_consumer.end()) {
            return 0;
        }
        return static_cast<uint32>(iter->second.size());
    }
};

struct PreprocessTranslateStore {
    Array<TranslateInfo> translate_infos{};
    UnorderedMap<SubmissionKey, uint32, SubmissionKeyHash> lookup{};
    UnorderedMap<SourceSubmitKey, uint32, SourceSubmitKeyHash> source_lookup{};
    Array<SourceSubmitPlan> source_plans{};
    Array<PresentCandidateMetadata> present_candidates{};
    UnorderedMap<uint64, uint32> present_lookup{};
    LogicalDependencyGraph dependency_graph{};

    void Reserve(uint32 count) {
        translate_infos.reserve(count);
        lookup.reserve(count);
        source_plans.reserve(count);
        source_lookup.reserve(count);
        present_candidates.reserve(count);
        present_lookup.reserve(count);
        dependency_graph.Reserve(count);
    }

    void Add(TranslateInfo&& result) {
        const SubmissionKey key   = result.key;
        const uint32        index = static_cast<uint32>(translate_infos.size());
        translate_infos.emplace_back(std::move(result));
        lookup.emplace(key, index);
    }

    const TranslateInfo* Find(const SubmissionKey& key) const {
        const auto iter = lookup.find(key);
        if (iter == lookup.end()) {
            return nullptr;
        }
        return &translate_infos[iter->second];
    }

    TranslateInfo* FindMutable(const SubmissionKey& key) {
        const auto iter = lookup.find(key);
        if (iter == lookup.end()) {
            return nullptr;
        }
        return &translate_infos[iter->second];
    }

    void AddSourcePlan(SourceSubmitPlan&& plan) {
        const SourceSubmitKey key   = plan.source_key;
        const uint32         index = static_cast<uint32>(source_plans.size());
        source_plans.emplace_back(std::move(plan));
        source_lookup.emplace(key, index);
    }

    const SourceSubmitPlan* FindSourcePlan(const SourceSubmitKey& key) const {
        const auto iter = source_lookup.find(key);
        if (iter == source_lookup.end()) {
            return nullptr;
        }
        return &source_plans[iter->second];
    }

    void AddPresent(PresentCandidateMetadata&& result) {
        const uint32 index = static_cast<uint32>(present_candidates.size());
        present_lookup.emplace(result.op_seq, index);
        present_candidates.emplace_back(std::move(result));
    }

    const PresentCandidateMetadata* FindPresent(uint64 op_seq) const {
        const auto iter = present_lookup.find(op_seq);
        if (iter == present_lookup.end()) {
            return nullptr;
        }
        return &present_candidates[iter->second];
    }
};

struct PendingSubmitTask {
    SubmissionKey key{};
    EQueueType    queue{EQueueType::Ignore};
    uint32        translate_index{0};
    SyncPointId   signal_syncpoint{0};
    Array<SyncPointId> wait_syncpoints{};
    GraphEventArray    task_dependencies{};
    GraphEventRef      completion_event{nullptr};
};

struct TranslatePipelineBatch {
    Array<QueueTranslateInfo> translate_ops{};
    Array<PendingSubmitTask>  submit_ops{};
    Array<SubmitPresentStage> present_ops{};
};

class SubmissionPreprocessor {
public:
    PreprocessTranslateStore Process(const Array<ExecutorOp>& ops, uint64 op_seq_base);

private:
    PreprocessDependencyState dependency_state{};
};

class TranslatePipelineRuntime {
public:
    TranslatePipelineBatch Assemble(
        Array<ExecutorOp>&&             ops,
        const PreprocessTranslateStore& preprocess_store,
        uint64                          op_seq_base
    );

    void Dispatch(
        TranslatePipelineBatch&& pipeline_batch,
        uint64                   trace_frame,
        TaskPipe&                translate_dispatch_pipe,
        TaskPipe&                translate_pipe,
        VulkanSubmissionRuntime& submission_runtime
    );

private:
    std::atomic_uint64_t next_syncpoint_id{1};
};

inline bool SubmissionKeyLess(const SubmissionKey& lhs, const SubmissionKey& rhs) {
    if (lhs.op_seq != rhs.op_seq) {
        return lhs.op_seq < rhs.op_seq;
    }
    return lhs.submit_idx < rhs.submit_idx;
}

inline void AppendUniqueDependency(
    GraphEventArray&     dependencies,
    const GraphEventRef& dependency
) {
    if (!dependency) {
        return;
    }
    for (const GraphEventRef& existing : dependencies) {
        if (existing.Get() == dependency.Get()) {
            return;
        }
    }
    dependencies.emplace_back(dependency);
}

inline GraphEventRef ChainGraphEvents(
    const GraphEventRef& previous_event,
    const GraphEventRef& next_event
) {
    if (!previous_event) {
        return next_event;
    }
    if (!next_event) {
        return previous_event;
    }

    GraphEventRef chained_event = GraphEvent::CreateGraphEvent();
    chained_event->WaitUntil(previous_event);
    chained_event->WaitUntil(next_event);
    chained_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return chained_event;
}

inline size_t EstimateSubmitCount(const Array<ExecutorOp>& ops) {
    size_t submit_count = 0;
    for (const auto& op : ops) {
        if (const auto* submit_op = std::get_if<ExecutorSubmitOp>(&op)) {
            submit_count += submit_op->submits.size();
        }
    }
    return submit_count;
}

inline size_t EstimatePlatformOpCount(const Array<ExecutorOp>& ops) {
    size_t op_count = 0;
    for (const auto& op : ops) {
        if (const auto* submit_op = std::get_if<ExecutorSubmitOp>(&op)) {
            op_count += submit_op->submits.size();
        } else {
            op_count += 1;
        }
    }
    return op_count;
}

inline const char* QueueTypeName(EQueueType queue) {
    switch (queue) {
        case EQueueType::Graphics:
            return "Graphics";
        case EQueueType::Compute:
            return "Compute";
        case EQueueType::Copy:
            return "Copy";
        case EQueueType::Ignore:
            return "Ignore";
        case EQueueType::Num:
        default:
            return "Num";
    }
}

inline uint8 ResolveTextureMipCount(Texture* texture, const ResourceKey& key) {
    assert(texture != nullptr && "ResolveTextureMipCount requires a texture");
    return key.mip_count == kRemainingSubresource ? uint8(texture->GetNumMips() - key.mip_level) :
                                                    key.mip_count;
}

inline uint8 ResolveTextureArrayCount(Texture* texture, const ResourceKey& key) {
    assert(texture != nullptr && "ResolveTextureArrayCount requires a texture");
    return key.array_count == kRemainingSubresource ? uint8(texture->GetNumArray() - key.array_layer) :
                                                      key.array_count;
}

} // namespace Moer::Render
