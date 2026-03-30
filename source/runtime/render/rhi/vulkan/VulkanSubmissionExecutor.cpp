#include "VulkanSubmissionExecutor.h"

#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "VulkanRHITrace.h"
#include "VulkanRHIResource.h"
#include "VulkanAllocator.h"
#include "RHICmdReorderer.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHI.h"
#include "rhi/RHIImpl.h"
#include "trace/Trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <limits>
#include <vulkan/vulkan_core.h>

namespace Moer::Render {
namespace {

static const char* VkLayoutStr(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:                        return "UNDEFINED";
        case VK_IMAGE_LAYOUT_GENERAL:                          return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:         return "COLOR_ATTACHMENT";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_STENCIL_ATTACHMENT";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "DEPTH_STENCIL_READ_ONLY";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:        return "SHADER_READ_ONLY";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:             return "TRANSFER_SRC";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:             return "TRANSFER_DST";
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:                  return "PRESENT_SRC";
        case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:                return "READ_ONLY";
        case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:               return "ATTACHMENT";
        default:                                               return "UNKNOWN";
    }
}

enum class ETrackedResourceType : uint8 {
    Buffer,
    Texture,
    Bindless,
    Accel
};

struct ResourceKey {
    ETrackedResourceType type{ETrackedResourceType::Buffer};
    uint64               handle{0};
    // Subresource range — only meaningful for Texture type.
    // 0xFF (kRemainingSubresource) means "all remaining" (whole-resource default).
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

//NOTE: ResourceKey encodes subresource range in the map key for Texture type,
// allowing independent tracking of different mip/layer ranges of the same texture.
using ResourceAccessDigest = UnorderedMap<ResourceKey, ResourceAccessDigestEntry, ResourceKeyHash>;

struct SubmissionKey {
    uint64 op_seq{0};
    uint32 submit_idx{0};

    bool operator==(const SubmissionKey& other) const {
        return op_seq == other.op_seq && submit_idx == other.submit_idx;
    }
};

struct SubmissionKeyHash {
    size_t operator()(const SubmissionKey& key) const {
        size_t hash = std::hash<uint64>{}(key.op_seq);
        hash ^= std::hash<uint32>{}(key.submit_idx) + 0x9e3779b9 + (hash << 6u) + (hash >> 2u);
        return hash;
    }
};

static bool SubmissionKeyLess(const SubmissionKey& lhs, const SubmissionKey& rhs) {
    if (lhs.op_seq != rhs.op_seq) {
        return lhs.op_seq < rhs.op_seq;
    }
    return lhs.submit_idx < rhs.submit_idx;
}

struct ResourceStateValue {
    bool       known{false};
    bool       has_writer{false};
    EQueueType owner_queue{EQueueType::Ignore};
    std::optional<SubmissionKey> last_submission{};

    EBufferState  buffer_state{EBufferState::UNDEFINED};
    ETextureState texture_state{ETextureState::UNDEFINED};
};

using ResourceStateSnapshot = UnorderedMap<ResourceKey, ResourceStateValue, ResourceKeyHash>;
using DirtyWrittenResources = UnorderedSet<ResourceKey, ResourceKeyHash>;

struct SourceSubmitKey {
    uint64 op_seq{0};
    uint32 submit_idx{0};

    bool operator==(const SourceSubmitKey& other) const {
        return op_seq == other.op_seq && submit_idx == other.submit_idx;
    }
};

struct SourceSubmitKeyHash {
    size_t operator()(const SourceSubmitKey& key) const {
        size_t hash = std::hash<uint64>{}(key.op_seq);
        hash ^= std::hash<uint32>{}(key.submit_idx) + 0x9e3779b9 + (hash << 6u) + (hash >> 2u);
        return hash;
    }
};

enum class ESegmentType : uint8 {
    Graphics,
    Copy
};

struct CmdSubmitPreprocessResult {
    SubmissionKey key{};
    SourceSubmitKey source_key{};
    EQueueType    queue{EQueueType::Ignore};
    ESegmentType segment_type{ESegmentType::Graphics};
    bool include{false};

    ResourceStateSnapshot initial_state_snapshot{};
    ResourceStateSnapshot last_state_snapshot{};
    ResourceAccessDigest  digest{};
    std::optional<EQueueType> prefix_transfer_queue{};
    std::optional<EQueueType> suffix_transfer_queue{};
    Array<ImportTexture> prefix_import_textures{};
    Array<ImportBuffer>  prefix_import_buffers{};
    Array<ExportTexture> suffix_export_textures{};
    Array<ExportBuffer>  suffix_export_buffers{};
    Array<SubmissionKey> wait_submission_keys{};
};

struct SourceSubmitSegmentPlan {
    SubmissionKey key{};
    EQueueType    queue{EQueueType::Ignore};
    ESegmentType kind{ESegmentType::Graphics};
    bool          include{false};
};

struct SourceSubmitPlan {
    SourceSubmitKey source_key{};
    EQueueType      parent_queue{EQueueType::Ignore};
    Array<SourceSubmitSegmentPlan> segments{};
};

struct CommandSegmentInfo {
    ESegmentType type{ESegmentType::Graphics};
    size_t             begin{0};
    size_t             end{0};
    size_t             copy_scope_index{0};

    bool IsEmpty(const CmdSubmit& submit) const {
        if (type == ESegmentType::Copy) {
            const auto* copy_scope = static_cast<const CopyScopeCmd*>(submit.cmds[copy_scope_index].get());
            return copy_scope->Empty();
        }
        return begin == end;
    }
};

struct ExecutePreprocessStore {
    Array<CmdSubmitPreprocessResult>                        results{};
    UnorderedMap<SubmissionKey, uint32, SubmissionKeyHash> lookup{};
    UnorderedMap<SourceSubmitKey, uint32, SourceSubmitKeyHash> source_lookup{};
    Array<SourceSubmitPlan>                                    source_plans{};

    void Reserve(uint32 count) {
        results.reserve(count);
        lookup.reserve(count);
        source_plans.reserve(count);
        source_lookup.reserve(count);
    }

    void Add(CmdSubmitPreprocessResult&& result) {
        const SubmissionKey key   = result.key;
        const uint32        index = static_cast<uint32>(results.size());
        results.emplace_back(std::move(result));
        lookup.emplace(key, index);
    }

    const CmdSubmitPreprocessResult* Find(const SubmissionKey& key) const {
        const auto iter = lookup.find(key);
        if (iter == lookup.end()) {
            return nullptr;
        }
        return &results[iter->second];
    }

    CmdSubmitPreprocessResult* FindMutable(const SubmissionKey& key) {
        const auto iter = lookup.find(key);
        if (iter == lookup.end()) {
            return nullptr;
        }
        return &results[iter->second];
    }

    void AddSourcePlan(SourceSubmitPlan&& plan) {
        const SourceSubmitKey key   = plan.source_key;
        const uint32          index = static_cast<uint32>(source_plans.size());
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
};

struct QueueSubmissionInfo {
    SubmissionKey key{};
    uint64        op_seq{0};
    EQueueType    queue{EQueueType::Ignore};
    CmdSubmit     submit;
    std::optional<VulkanRecordedSubmit> recorded_submit{};
    ResourceAccessDigest digest{};
    ResourceStateSnapshot initial_state_snapshot{};  // §7.2: seed_tracker for translate
    Array<SubmissionKey>             wait_submission_keys{};
    bool                             valid{true};

    QueueSubmissionInfo(
        SubmissionKey in_key,
        uint64        in_op_seq,
        EQueueType    in_queue,
        CmdSubmit&&   in_submit
    ) :
        key(in_key),
        op_seq(in_op_seq),
        queue(in_queue),
        submit(std::move(in_submit)) {}

    QueueSubmissionInfo(QueueSubmissionInfo&&) noexcept            = default;
    QueueSubmissionInfo& operator=(QueueSubmissionInfo&&) noexcept = default;
    QueueSubmissionInfo(const QueueSubmissionInfo&)                = delete;
    QueueSubmissionInfo& operator=(const QueueSubmissionInfo&)     = delete;
};

struct PresentInfo {
    uint64       op_seq{0};
    RHIPresentOp present{};
    Array<SubmissionKey> wait_submission_keys{};
    bool         valid{true};
    std::string  error{};

    PresentInfo(uint64 in_op_seq, RHIPresentOp&& in_present) :
        op_seq(in_op_seq),
        present(std::move(in_present)) {}

    PresentInfo(PresentInfo&&) noexcept            = default;
    PresentInfo& operator=(PresentInfo&&) noexcept = default;
    PresentInfo(const PresentInfo&)                = delete;
    PresentInfo& operator=(const PresentInfo&)     = delete;
};

using PlatformExecOp = std::variant<QueueSubmissionInfo, PresentInfo>;

struct ResourceHazardState {
    std::optional<SubmissionKey> last_writer{};
    Array<SubmissionKey>         last_readers{};
};

static ResourceKey MakeBufferKey(uint64 handle) {
    return ResourceKey{ETrackedResourceType::Buffer, handle};
}

// Whole-resource texture key (default: all mips, all array layers).
static ResourceKey MakeTextureKey(uint64 handle) {
    return ResourceKey{
        ETrackedResourceType::Texture, handle,
        0, kRemainingSubresource, 0, kRemainingSubresource
    };
}

// Subresource-range texture key for per-range tracking.
static ResourceKey MakeTextureKeyWithRange(
    uint64 handle,
    uint8  mip_level,
    uint8  mip_count,
    uint8  array_layer,
    uint8  array_count
) {
    return ResourceKey{
        ETrackedResourceType::Texture, handle,
        mip_level, mip_count, array_layer, array_count
    };
}

static bool IsTextureKey(const ResourceKey& key) {
    return key.type == ETrackedResourceType::Texture;
}

static bool IsSingleTextureSubresourceKey(const ResourceKey& key) {
    return IsTextureKey(key) && key.mip_count == 1 && key.array_count == 1;
}

static uint8 ResolveTextureMipCount(Texture* texture, const ResourceKey& key) {
    assert(texture != nullptr && "ResolveTextureMipCount requires a texture");
    return key.mip_count == kRemainingSubresource ? uint8(texture->GetNumMips() - key.mip_level) :
                                                    key.mip_count;
}

static uint8 ResolveTextureArrayCount(Texture* texture, const ResourceKey& key) {
    assert(texture != nullptr && "ResolveTextureArrayCount requires a texture");
    return key.array_count == kRemainingSubresource ? uint8(texture->GetNumArray() - key.array_layer) :
                                                      key.array_count;
}

template<typename Fn>
static void ForEachTextureSubresourceKey(const ResourceKey& key, Fn&& fn) {
    if (!IsTextureKey(key) || key.handle == 0) {
        return;
    }

    auto* texture = reinterpret_cast<Texture*>(key.handle);
    if (texture == nullptr) {
        return;
    }

    ValidateSubresourceRange(
        texture,
        key.mip_level,
        key.mip_count,
        key.array_layer,
        key.array_count
    );

    const uint8 mip_count   = ResolveTextureMipCount(texture, key);
    const uint8 array_count = ResolveTextureArrayCount(texture, key);
    for (uint8 mip = 0; mip < mip_count; ++mip) {
        for (uint8 layer = 0; layer < array_count; ++layer) {
            fn(ResourceKey{
                ETrackedResourceType::Texture,
                key.handle,
                uint8(key.mip_level + mip),
                1,
                uint8(key.array_layer + layer),
                1
            });
        }
    }
}

static ResourceKey MakeBindlessKey(uint64 handle) {
    return ResourceKey{ETrackedResourceType::Bindless, handle};
}

static ResourceKey MakeAccelKey(uint64 handle) {
    return ResourceKey{ETrackedResourceType::Accel, handle};
}

static constexpr uint64 kGlobalAccelBuildSyncHandle = std::numeric_limits<uint64>::max();

static const char* QueueTypeName(EQueueType queue) {
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

static const char* SegmentKindName(ESegmentType kind) {
    switch (kind) {
        case ESegmentType::Graphics:
            return "Parent";
        case ESegmentType::Copy:
            return "Copy";
        default:
            return "Unknown";
    }
}

static const char* ResourceTypeName(ETrackedResourceType type) {
    switch (type) {
        case ETrackedResourceType::Buffer:
            return "Buffer";
        case ETrackedResourceType::Texture:
            return "Texture";
        case ETrackedResourceType::Bindless:
            return "Bindless";
        case ETrackedResourceType::Accel:
            return "Accel";
        default:
            return "Unknown";
    }
}

static std::string_view ResourceName(const ResourceKey& key) {
    switch (key.type) {
        case ETrackedResourceType::Buffer: {
            auto* buffer = reinterpret_cast<const Buffer*>(key.handle);
            return buffer != nullptr ? buffer->GetName() : "<null>";
        }
        case ETrackedResourceType::Texture: {
            auto* texture = reinterpret_cast<const Texture*>(key.handle);
            return texture != nullptr ? texture->GetName() : "<null>";
        }
        default:
            return "<non-resource>";
    }
}

static void TraceDigest(
    const SubmissionKey& key,
    EQueueType queue,
    ESegmentType segment_kind,
    const ResourceAccessDigest& digest
) {
    for (const auto& [resource_key, access] : digest) {
        RHITRACE_LOG(
            verbose,
            "[RHITrace][PreprocessDigest] submit=({}, {}) queue={} segment={} resource_type={} name={} handle=0x{:x} read={} write={} last_write={} buffer_state={} texture_state={}",
            key.op_seq,
            key.submit_idx,
            QueueTypeName(queue),
            SegmentKindName(segment_kind),
            ResourceTypeName(resource_key.type),
            ResourceName(resource_key),
            resource_key.handle,
            access.read,
            access.write,
            access.last_access_write,
            access.buffer_state.has_value() ? int(access.buffer_state.value()) : -1,
            access.texture_state.has_value() ? int(access.texture_state.value()) : -1
        );
    }
}

static ERHIResourceLastAccessKind ToPersistentAccessKind(bool has_writer) {
    return has_writer ? ERHIResourceLastAccessKind::Write : ERHIResourceLastAccessKind::Read;
}

static ResourceStateValue LoadPersistentState(const ResourceKey& resource_key) {
    ResourceStateValue state{};
    switch (resource_key.type) {
        case ETrackedResourceType::Buffer: {
            auto* buffer = reinterpret_cast<Buffer*>(resource_key.handle);
            if (buffer == nullptr) {
                return state;
            }
            const BufferPersistentState persistent_state = buffer->GetPersistentState();
            state.known        = persistent_state.known;
            state.has_writer   = persistent_state.last_access_kind == ERHIResourceLastAccessKind::Write;
            state.owner_queue  = persistent_state.owner_queue;
            state.buffer_state = persistent_state.state;
            RHITRACE_LOG(
                verbose,
                "[RHITrace][PersistentLoad][Buffer] name={} handle=0x{:x} known={} owner={} state={} last_access={}",
                buffer->GetName(),
                resource_key.handle,
                state.known,
                QueueTypeName(state.owner_queue),
                int(state.buffer_state),
                int(persistent_state.last_access_kind)
            );
            return state;
        }
        case ETrackedResourceType::Texture: {
            auto* texture = reinterpret_cast<Texture*>(resource_key.handle);
            if (texture == nullptr) {
                return state;
            }
            assert(
                IsSingleTextureSubresourceKey(resource_key) &&
                "Texture persistent load must use canonical single-subresource keys"
            );
            const TexturePersistentState persistent_state =
                texture->GetPersistentState(resource_key.mip_level, resource_key.array_layer);
            state.known         = persistent_state.known;
            state.has_writer    = persistent_state.last_access_kind == ERHIResourceLastAccessKind::Write;
            state.owner_queue   = persistent_state.owner_queue;
            state.texture_state = persistent_state.state;
            RHITRACE_LOG(
                verbose,
                "[RHITrace][PersistentLoad][Texture] name={} handle=0x{:x} mip={} layer={} known={} owner={} state={} last_access={}",
                texture->GetName(),
                resource_key.handle,
                resource_key.mip_level,
                resource_key.array_layer,
                state.known,
                QueueTypeName(state.owner_queue),
                int(state.texture_state),
                int(persistent_state.last_access_kind)
            );
            return state;
        }
        default:
            return state;
    }
}

static void EnsureDigestStateLoaded(
    ResourceStateSnapshot&      snapshot,
    const ResourceAccessDigest& digest
) {
    for (const auto& [resource_key, access] : digest) {
        if (!access.read && !access.write) {
            continue;
        }
        if (IsTextureKey(resource_key)) {
            ForEachTextureSubresourceKey(
                resource_key,
                [&](const ResourceKey& subresource_key) {
                    if (!snapshot.contains(subresource_key)) {
                        snapshot.emplace(subresource_key, LoadPersistentState(subresource_key));
                    }
                }
            );
            continue;
        }
        if (snapshot.contains(resource_key)) {
            continue;
        }
        snapshot.emplace(resource_key, LoadPersistentState(resource_key));
    }
}

static void CommitPersistentResourceStates(const ResourceStateSnapshot& snapshot) {
    for (const auto& [resource_key, state] : snapshot) {
        switch (resource_key.type) {
            case ETrackedResourceType::Buffer: {
                auto* buffer = reinterpret_cast<Buffer*>(resource_key.handle);
                if (buffer == nullptr) {
                    break;
                }
                buffer->SetPersistentState(BufferPersistentState{
                    .known = state.known,
                    .owner_queue = state.owner_queue,
                    .state = state.buffer_state,
                    .last_access_kind = state.known ? ToPersistentAccessKind(state.has_writer) :
                                                      ERHIResourceLastAccessKind::Unknown
                });
                break;
            }
            case ETrackedResourceType::Texture: {
                auto* texture = reinterpret_cast<Texture*>(resource_key.handle);
                if (texture == nullptr) {
                    break;
                }
                assert(
                    IsSingleTextureSubresourceKey(resource_key) &&
                    "Texture persistent commit must use canonical single-subresource keys"
                );
                texture->SetPersistentState(resource_key.mip_level, resource_key.array_layer, TexturePersistentState{
                    .known = state.known,
                    .owner_queue = state.owner_queue,
                    .state = state.texture_state,
                    .last_access_kind = state.known ? ToPersistentAccessKind(state.has_writer) :
                                                      ERHIResourceLastAccessKind::Unknown
                });
                break;
            }
            default:
                break;
        }
    }
}

static bool IsBufferTextureWrite(VulkanShaderResourceState state) {
    return state.resource_type == SRT_UAV;
}

static bool IsBufferTextureWrite(uint64 flags) {
    return IsBufferTextureWrite(VulkanShaderResourceState(flags));
}

static bool IsTextureSampled(uint64 flags) {
    VulkanShaderResourceState state(flags);
    return state.b_sampled;
}

static bool IsBufferTextureRead(uint64 flags) {
    VulkanShaderResourceState state(flags);
    return state.resource_type == SRT_SRV || state.resource_type == SRT_CBV;
}

static void LockBindlessArray(uint64 handle) {
    auto* bindless_array = reinterpret_cast<VulkanBindlessArray*>(handle);
    bindless_array->Lock();
}

static void UnlockBindlessArray(uint64 handle) {
    auto* bindless_array = reinterpret_cast<VulkanBindlessArray*>(handle);
    bindless_array->Unlock();
}

static uint64 GetHandle(const BufferView& view) {
    return uint64(view.GetBuffer());
}

static uint64 GetHandle(const TextureView& view) {
    return uint64(view.GetTexture());
}

static void MergeDigestEntry(
    ResourceAccessDigest&         digest,
    const ResourceKey&            key,
    bool                          read,
    bool                          write,
    std::optional<EBufferState>   buffer_state,
    std::optional<ETextureState>  texture_state
) {
    if (key.handle == 0) {
        return;
    }
    auto& entry  = digest[key];
    entry.read |= read;
    entry.write |= write;
    entry.last_access_write = write;
    if (buffer_state.has_value()) {
        entry.buffer_state = buffer_state;
    }
    if (texture_state.has_value()) {
        entry.texture_state = texture_state;
    }
}

static void MergeDigest(
    ResourceAccessDigest&       dst,
    const ResourceAccessDigest& src
) {
    for (const auto& [key, entry] : src) {
        if (key.handle == 0) {
            continue;
        }
        auto& dst_entry = dst[key];
        dst_entry.read |= entry.read;
        dst_entry.write |= entry.write;
        dst_entry.last_access_write = entry.last_access_write;
        if (entry.buffer_state.has_value()) {
            dst_entry.buffer_state = entry.buffer_state;
        }
        if (entry.texture_state.has_value()) {
            dst_entry.texture_state = entry.texture_state;
        }
    }
}

static void ApplyDigestToDirtyWrittenResources(
    DirtyWrittenResources&      dirty_written_resources,
    const ResourceAccessDigest& digest
);

class ResourceAccessCollector {
public:
    ResourceAccessCollector(
        EQueueType              in_queue,
        const TCachedArgArray&  in_cached_args,
        DirtyWrittenResources*  in_dirty_written_resources = nullptr
    ) :
        queue(in_queue),
        cached_args(in_cached_args),
        dirty_written_resources(in_dirty_written_resources) {}

    ResourceAccessDigest Collect(const CmdSubmit& submit) const {
        ResourceAccessDigest digest{};
        DirtyWrittenResources visible_dirty_written_resources{};
        if (dirty_written_resources != nullptr) {
            visible_dirty_written_resources = *dirty_written_resources;
        }
        for (const auto& cmd : submit.cmds) {
            if (!cmd) {
                continue;
            }
            ResourceAccessDigest command_digest{};
            VisitCommand(*cmd, command_digest, visible_dirty_written_resources);
            MergeDigest(digest, command_digest);
            ApplyDigestToDirtyWrittenResources(visible_dirty_written_resources, command_digest);
        }
        return digest;
    }

    ResourceAccessDigest Collect(std::span<const Command* const> commands) const {
        ResourceAccessDigest digest{};
        DirtyWrittenResources visible_dirty_written_resources{};
        if (dirty_written_resources != nullptr) {
            visible_dirty_written_resources = *dirty_written_resources;
        }
        for (const Command* cmd : commands) {
            if (cmd == nullptr) {
                continue;
            }
            ResourceAccessDigest command_digest{};
            VisitCommand(*cmd, command_digest, visible_dirty_written_resources);
            MergeDigest(digest, command_digest);
            ApplyDigestToDirtyWrittenResources(visible_dirty_written_resources, command_digest);
        }
        return digest;
    }

private:
    static bool IsPipelineResourceValid(const PipelineHandle& pipeline, uint32 index) {
        if (index >= 64) {
            return false;
        }
        return (pipeline.valid_bits & (uint64(1) << index)) != 0;
    }

    static bool IsLoadAction(EAttachmentAction action) {
        return GetLoadOp(action) == EAttachmentLoadOp::LOAD;
    }

    static bool IsStoreAction(EAttachmentAction action) {
        return GetStoreOp(action) == EAttachmentStoreOp::STORE;
    }

    void MarkReadBuffer(ResourceAccessDigest& digest, uint64 handle, EBufferState state) const {
        MergeDigestEntry(digest, MakeBufferKey(handle), true, false, state, std::nullopt);
    }

    void MarkWriteBuffer(ResourceAccessDigest& digest, uint64 handle, EBufferState state) const {
        MergeDigestEntry(digest, MakeBufferKey(handle), false, true, state, std::nullopt);
    }

    void MarkReadTexture(ResourceAccessDigest& digest, uint64 handle, ETextureState state) const {
        MarkReadTextureWithRange(
            digest,
            handle,
            state,
            0,
            kRemainingSubresource,
            0,
            kRemainingSubresource
        );
    }

    void MarkWriteTexture(ResourceAccessDigest& digest, uint64 handle, ETextureState state) const {
        MarkWriteTextureWithRange(
            digest,
            handle,
            state,
            0,
            kRemainingSubresource,
            0,
            kRemainingSubresource
        );
    }

    void MarkReadTextureWithRange(
        ResourceAccessDigest& digest,
        uint64                handle,
        ETextureState         state,
        uint8                 mip_level,
        uint8                 mip_count,
        uint8                 array_layer,
        uint8                 array_count
    ) const {
        ForEachTextureSubresourceKey(
            MakeTextureKeyWithRange(handle, mip_level, mip_count, array_layer, array_count),
            [&](const ResourceKey& subresource_key) {
                MergeDigestEntry(digest, subresource_key, true, false, std::nullopt, state);
            }
        );
    }

    void MarkWriteTextureWithRange(
        ResourceAccessDigest& digest,
        uint64                handle,
        ETextureState         state,
        uint8                 mip_level,
        uint8                 mip_count,
        uint8                 array_layer,
        uint8                 array_count
    ) const {
        ForEachTextureSubresourceKey(
            MakeTextureKeyWithRange(handle, mip_level, mip_count, array_layer, array_count),
            [&](const ResourceKey& subresource_key) {
                MergeDigestEntry(digest, subresource_key, false, true, std::nullopt, state);
            }
        );
    }

    void MarkReadBindless(ResourceAccessDigest& digest, uint64 handle, EBufferState state) const {
        MergeDigestEntry(digest, MakeBindlessKey(handle), true, false, state, std::nullopt);
    }

    void MarkWriteBindless(ResourceAccessDigest& digest, uint64 handle, EBufferState state) const {
        MergeDigestEntry(digest, MakeBindlessKey(handle), false, true, state, std::nullopt);
    }

    void MarkReadAccel(ResourceAccessDigest& digest, uint64 handle, EBufferState state) const {
        MergeDigestEntry(digest, MakeAccelKey(handle), true, false, state, std::nullopt);
    }

    void MarkWriteAccel(ResourceAccessDigest& digest, uint64 handle, EBufferState state) const {
        MergeDigestEntry(digest, MakeAccelKey(handle), false, true, state, std::nullopt);
    }

    static std::optional<ETextureState> GetBindlessReadTextureState(uint64 handle) {
        auto* texture = reinterpret_cast<Texture*>(handle);
        if (texture == nullptr) {
            return std::nullopt;
        }
        const auto usage = texture->GetUsage();
        if ((usage & ETextureUsageFlags::SAMPLED) == ETextureUsageFlags::SAMPLED) {
            return ETextureState::SAMPLE;
        }
        return ETextureState::SHADER_RESOURCE;
    }

    static std::optional<EBufferState> GetBindlessReadBufferState(uint64 handle) {
        auto* buffer = reinterpret_cast<Buffer*>(handle);
        if (buffer == nullptr) {
            return std::nullopt;
        }
        return EBufferState::SHADER_RESOURCE;
    }

    void CollectBindlessReads(
        ResourceAccessDigest&   digest,
        uint64                  bindless_handle,
        DirtyWrittenResources&  visible_dirty_written_resources
    ) const {
        if (visible_dirty_written_resources.empty()) {
            return;
        }

        auto* bindless_array = reinterpret_cast<VulkanBindlessArray*>(bindless_handle);
        if (bindless_array == nullptr) {
            return;
        }

        Array<ResourceKey> consumed_resources{};
        consumed_resources.reserve(visible_dirty_written_resources.size());

        for (const auto& resource_key : visible_dirty_written_resources) {
            if (resource_key.handle == 0) {
                continue;
            }

            if (resource_key.type == ETrackedResourceType::Texture) {
                auto state = GetBindlessReadTextureState(resource_key.handle);
                if (state.has_value()) {
                    if (!bindless_array->IsTextureViewAllocated(
                            resource_key.handle,
                            resource_key.mip_level,
                            resource_key.mip_count,
                            resource_key.array_layer,
                            resource_key.array_count
                        )) {
                        continue;
                    }
                    MarkReadTextureWithRange(
                        digest,
                        resource_key.handle,
                        state.value(),
                        resource_key.mip_level,
                        resource_key.mip_count,
                        resource_key.array_layer,
                        resource_key.array_count
                    );
                    consumed_resources.emplace_back(resource_key);
                }
            } else if (resource_key.type == ETrackedResourceType::Buffer) {
                auto state = GetBindlessReadBufferState(resource_key.handle);
                if (state.has_value()) {
                    if (!bindless_array->IsResourceAllocated(resource_key.handle)) {
                        continue;
                    }
                    MarkReadBuffer(digest, resource_key.handle, state.value());
                    consumed_resources.emplace_back(resource_key);
                }
            }
        }

        for (const auto& resource_key : consumed_resources) {
            visible_dirty_written_resources.erase(resource_key);
        }
    }

    void CollectRenderPassInfo(const RenderPassInfo& pass_info, ResourceAccessDigest& digest) const {
        if (pass_info.depth_attachment.Valid()) {
            const auto depth_action = GetDepthAction(pass_info.depth_attachment.action);
            const auto depth_handle = uint64(pass_info.depth_attachment.target);
            if (IsLoadAction(depth_action)) {
                MarkReadTexture(digest, depth_handle, ETextureState::DEPTH_STENCIL);
            }
            if (IsStoreAction(depth_action)) {
                MarkWriteTexture(digest, depth_handle, ETextureState::DEPTH_STENCIL);
            }
        }

        for (const auto& color_attachment : pass_info.color_attachments) {
            const auto color_handle = uint64(color_attachment.target);
            if (IsLoadAction(color_attachment.action)) {
                MarkReadTexture(digest, color_handle, ETextureState::RENDER_TARGET);
            }
            if (IsStoreAction(color_attachment.action)) {
                MarkWriteTexture(digest, color_handle, ETextureState::RENDER_TARGET);
            }
        }
    }

    void CollectShaderArg(
        const TArg&            arg,
        ParamInfoFlags         param_info,
        ResourceAccessDigest&  digest,
        DirtyWrittenResources& visible_dirty_written_resources
    ) const {
        VulkanShaderResourceState shader_state(param_info.state_flags);
        if (const auto* bindless = std::get_if<BindlessArrayRef>(&arg)) {
            if (bindless->Get() == nullptr) {
                return;
            }
            const uint64 handle = uint64(bindless->Get());
            MarkReadBindless(digest, handle, EBufferState::SHADER_RESOURCE);
            CollectBindlessReads(digest, handle, visible_dirty_written_resources);
            return;
        }
        if (shader_state.resource_type == SRT_INVALID || shader_state.resource_type == SRT_SAMPLER) {
            return;
        }

        const bool write = shader_state.resource_type == SRT_UAV;
        const bool read  = shader_state.resource_type == SRT_CBV || shader_state.resource_type == SRT_SRV ||
                          shader_state.resource_type == SRT_UAV;
        if (!read && !write) {
            return;
        }

        const EBufferState  buffer_read_state  = EBufferState::SHADER_RESOURCE;
        const EBufferState  buffer_write_state = EBufferState::UNORDERED_ACCESS;
        const ETextureState texture_read_state = shader_state.b_sampled ? ETextureState::SAMPLE :
                                                                       ETextureState::SHADER_RESOURCE;
        const ETextureState texture_write_state = ETextureState::UNORDERED_ACCESS;

        std::visit(
            [&](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, BufferView>) {
                    const uint64 handle = GetHandle(value);
                    if (read) {
                        MarkReadBuffer(digest, handle, buffer_read_state);
                    }
                    if (write) {
                        MarkWriteBuffer(digest, handle, buffer_write_state);
                    }
                } else if constexpr (std::is_same_v<T, TextureView>) {
                    const uint64 handle = GetHandle(value);
                    if (read) {
                        MarkReadTextureWithRange(
                            digest, handle, texture_read_state,
                            value.mip_level, value.num_mips, value.array_layer, value.num_array
                        );
                    }
                    if (write) {
                        MarkWriteTextureWithRange(
                            digest, handle, texture_write_state,
                            value.mip_level, value.num_mips, value.array_layer, value.num_array
                        );
                    }
                } else if constexpr (std::is_same_v<T, std::span<BufferView>>) {
                    for (const auto& view : value) {
                        const uint64 handle = GetHandle(view);
                        if (read) {
                            MarkReadBuffer(digest, handle, buffer_read_state);
                        }
                        if (write) {
                            MarkWriteBuffer(digest, handle, buffer_write_state);
                        }
                    }
                } else if constexpr (std::is_same_v<T, std::span<TextureView>>) {
                    for (const auto& view : value) {
                        const uint64 handle = GetHandle(view);
                        if (read) {
                            MarkReadTextureWithRange(
                                digest, handle, texture_read_state,
                                view.mip_level, view.num_mips, view.array_layer, view.num_array
                            );
                        }
                        if (write) {
                            MarkWriteTextureWithRange(
                                digest, handle, texture_write_state,
                                view.mip_level, view.num_mips, view.array_layer, view.num_array
                            );
                        }
                    }
                } else if constexpr (std::is_same_v<T, BindlessArrayRef>) {
                    if (value.Get() == nullptr) {
                        return;
                    }
                    const uint64 handle = uint64(value.Get());
                    if (read) {
                        MarkReadBindless(digest, handle, buffer_read_state);
                        CollectBindlessReads(digest, handle, visible_dirty_written_resources);
                    }
                    if (write) {
                        MarkWriteBindless(digest, handle, buffer_write_state);
                    }
                } else if constexpr (std::is_same_v<T, RaytracingTlasRef>) {
                    if (value.Get() == nullptr) {
                        return;
                    }
                    const uint64 handle = uint64(value.Get());
                    if (read) {
                        MarkReadAccel(digest, handle, buffer_read_state);
                    }
                    if (write) {
                        MarkWriteAccel(digest, handle, buffer_write_state);
                    }
                }
            },
            arg
        );
    }

    void CollectPipelineArgs(
        const PipelineHandle& pipeline,
        const ArrayArguments& args,
        ResourceAccessDigest& digest,
        DirtyWrittenResources& visible_dirty_written_resources
    ) const {
        const uint32 arg_count = static_cast<uint32>(std::min(args.args.size(), pipeline.binding_infos.size()));
        for (uint32 i = 0; i < arg_count; ++i) {
            if (!IsPipelineResourceValid(pipeline, i)) {
                continue;
            }
            CollectShaderArg(
                args.args[i],
                pipeline.binding_infos[i],
                digest,
                visible_dirty_written_resources
            );
        }
    }

    const ArrayArguments* ResolveShaderArgs(const TShaderArgArray& shader_args) const {
        if (auto* args = std::get_if<ArrayArguments>(&shader_args)) {
            return args;
        }
        if (auto* ref = std::get_if<ArrayArgReference>(&shader_args)) {
            if (ref->handle < cached_args.size()) {
                return &cached_args[ref->handle];
            }
        }
        return nullptr;
    }

    void VisitCommand(
        const Command&         cmd,
        ResourceAccessDigest&  digest,
        DirtyWrittenResources& visible_dirty_written_resources
    ) const {
        switch (cmd.Type()) {
            case Command::EType::UploadBuffer: {
                const auto* upload_cmd = static_cast<const UploadBufferCmd*>(&cmd);
                MarkWriteBuffer(digest, upload_cmd->Handle(), EBufferState::TRANSFER);
                break;
            }
            case Command::EType::UploadTexture: {
                const auto* upload_cmd = static_cast<const UploadTextureCmd*>(&cmd);
                MarkWriteTexture(digest, upload_cmd->Handle(), ETextureState::TRANSFER);
                break;
            }
            case Command::EType::BufferToBuffer: {
                const auto* copy_cmd = static_cast<const CopyBufferCmd*>(&cmd);
                MarkReadBuffer(digest, copy_cmd->SrcHandle(), EBufferState::TRANSFER);
                MarkWriteBuffer(digest, copy_cmd->DstHandle(), EBufferState::TRANSFER);
                break;
            }
            case Command::EType::BufferToTexture: {
                const auto* copy_cmd = static_cast<const CopyBufferToTextureCmd*>(&cmd);
                MarkReadBuffer(digest, copy_cmd->SrcHandle(), EBufferState::TRANSFER);
                MarkWriteTexture(digest, copy_cmd->DstHandle(), ETextureState::TRANSFER);
                break;
            }
            case Command::EType::TextureToBuffer: {
                const auto* copy_cmd = static_cast<const CopyTextureToBufferCmd*>(&cmd);
                MarkReadTexture(digest, copy_cmd->SrcHandle(), ETextureState::TRANSFER);
                MarkWriteBuffer(digest, copy_cmd->DstHandle(), EBufferState::TRANSFER);
                break;
            }
            case Command::EType::TextureToTexture: {
                const auto* copy_cmd = static_cast<const CopyTextureCmd*>(&cmd);
                MarkReadTexture(digest, copy_cmd->SrcHandle(), ETextureState::TRANSFER);
                MarkWriteTexture(digest, copy_cmd->DstHandle(), ETextureState::TRANSFER);
                break;
            }
            case Command::EType::CopyBackBuffer: {
                const auto* copy_cmd = static_cast<const CopyBackBufferCmd*>(&cmd);
                MarkReadBuffer(digest, copy_cmd->Handle(), EBufferState::TRANSFER);
                break;
            }
            case Command::EType::CopyBackTexture: {
                const auto* copy_cmd = static_cast<const CopyBackTextureCmd*>(&cmd);
                MarkReadTexture(digest, copy_cmd->Handle(), ETextureState::TRANSFER);
                break;
            }
            case Command::EType::ShaderDispatch: {
                const auto* dispatch_cmd = static_cast<const DispatchCmd*>(&cmd);
                CollectPipelineArgs(
                    dispatch_cmd->Pipeline(),
                    dispatch_cmd->Args(cached_args),
                    digest,
                    visible_dirty_written_resources
                );
                const auto dispatch_param = dispatch_cmd->Param();
                if (const auto* indirect = std::get_if<DispatchIndirectParam>(&dispatch_param)) {
                    MarkReadBuffer(digest, GetHandle(indirect->indirect), EBufferState::INDIRECT);
                }
                break;
            }
            case Command::EType::SetDrawState: {
                const auto* draw_cmd = static_cast<const SetDrawStateCmd*>(&cmd);
                CollectPipelineArgs(
                    draw_cmd->Pipeline(),
                    draw_cmd->Args(),
                    digest,
                    visible_dirty_written_resources
                );

                for (const auto& [buffer, range] : draw_cmd->VertexBuffers()) {
                    (void)range;
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::VERTEX);
                }
                for (const auto& [buffer, range] : draw_cmd->IndexBuffers()) {
                    (void)range;
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::INDEX);
                }
                for (const auto& [buffer, range] : draw_cmd->IndirectBuffers()) {
                    (void)range;
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::INDIRECT);
                }
                for (const auto& [buffer, range] : draw_cmd->DrawCountBuffers()) {
                    (void)range;
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::INDIRECT);
                }
                CollectRenderPassInfo(draw_cmd->RenderPassInfo(), digest);
                break;
            }
            case Command::EType::MultiDraw: {
                const auto* draw_cmd = static_cast<const MultiDrawCmd*>(&cmd);
                for (const auto& draw : draw_cmd->draw_batch.draw_cmds) {
                    const ArrayArguments* args = ResolveShaderArgs(draw.args);
                    if (args != nullptr) {
                        CollectPipelineArgs(
                            draw.handle,
                            *args,
                            digest,
                            visible_dirty_written_resources
                        );
                    }
                }
                for (const auto& [buffer, range] : draw_cmd->VertexBuffers()) {
                    (void)range;
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::VERTEX);
                }
                for (const auto& [buffer, range] : draw_cmd->IndexBuffers()) {
                    (void)range;
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::INDEX);
                }
                for (const auto& [buffer, range] : draw_cmd->IndirectBuffers()) {
                    (void)range;
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::INDIRECT);
                }
                CollectRenderPassInfo(draw_cmd->RenderPassInfo(), digest);
                break;
            }
            case Command::EType::Barrier: {
                const auto* barrier_cmd = static_cast<const BarrierCmd*>(&cmd);
                for (const auto& buffer : barrier_cmd->ReadBuffers()) {
                    MarkReadBuffer(digest, buffer.handle, buffer.state);
                }
                for (const auto& buffer : barrier_cmd->WriteBuffers()) {
                    MarkWriteBuffer(digest, buffer.handle, buffer.state);
                }
                for (const auto& texture : barrier_cmd->ReadTextures()) {
                    MarkReadTextureWithRange(
                        digest, texture.handle, texture.state,
                        static_cast<uint8>(texture.mip_level),
                        static_cast<uint8>(texture.mip_cnt),
                        static_cast<uint8>(texture.array_layer),
                        static_cast<uint8>(texture.array_count)
                    );
                }
                for (const auto& texture : barrier_cmd->WriteTextures()) {
                    MarkWriteTextureWithRange(
                        digest, texture.handle, texture.state,
                        static_cast<uint8>(texture.mip_level),
                        static_cast<uint8>(texture.mip_cnt),
                        static_cast<uint8>(texture.array_layer),
                        static_cast<uint8>(texture.array_count)
                    );
                }
                break;
            }
            case Command::EType::QueueTransfer: {
                const auto* transfer_cmd = static_cast<const QueueTransferCmd*>(&cmd);
                for (const auto& texture : transfer_cmd->ImportTextures()) {
                    MarkWriteTexture(digest, GetHandle(texture.texture), texture.state);
                }
                for (const auto& texture : transfer_cmd->ExportTextures()) {
                    MarkWriteTexture(digest, GetHandle(texture.texture), texture.state);
                }
                for (const auto& buffer : transfer_cmd->ImportBuffers()) {
                    MarkWriteBuffer(digest, GetHandle(buffer.buffer), buffer.state);
                }
                for (const auto& buffer : transfer_cmd->ExportBuffers()) {
                    MarkWriteBuffer(digest, GetHandle(buffer.buffer), buffer.state);
                }
                break;
            }
            case Command::EType::UpdateBindlessArray: {
                const auto* bindless_cmd = static_cast<const UpdateBindlessArrayCmd*>(&cmd);
                MarkWriteBindless(digest, uint64(bindless_cmd->Handle()), EBufferState::UNORDERED_ACCESS);
                break;
            }
            case Command::EType::ClearResource: {
                const auto* clear_cmd = static_cast<const ClearResourceCmd*>(&cmd);
                if (clear_cmd->IsBuffer()) {
                    MarkWriteBuffer(digest, GetHandle(clear_cmd->Buffer()), EBufferState::TRANSFER);
                } else if (clear_cmd->IsTexture()) {
                    MarkWriteTexture(digest, GetHandle(clear_cmd->Texture()), ETextureState::TRANSFER);
                }
                break;
            }
            case Command::EType::BuildAccel: {
                const auto* build_cmd = static_cast<const BuildAccelerationStructuresCmd*>(&cmd);
                for (const auto& param : build_cmd->Params()) {
                    if (param.geometry.Get() != nullptr) {
                        MarkWriteAccel(
                            digest, uint64(param.geometry.Get()), EBufferState::UNORDERED_ACCESS
                        );
                    }
                }
                // Conservative cross-submit/cross-queue sync anchor for AS build/update chain.
                MarkWriteAccel(digest, kGlobalAccelBuildSyncHandle, EBufferState::UNORDERED_ACCESS);
                for (auto* vtx : build_cmd->VtxBuffers()) {
                    MarkReadBuffer(digest, uint64(vtx), EBufferState::VERTEX);
                }
                for (auto* idx : build_cmd->IdxBuffers()) {
                    MarkReadBuffer(digest, uint64(idx), EBufferState::INDEX);
                }
                if (build_cmd->Scratch().GetBuffer() != nullptr) {
                    MarkWriteBuffer(
                        digest, GetHandle(build_cmd->Scratch()), EBufferState::UNORDERED_ACCESS
                    );
                }
                break;
            }
            case Command::EType::BuildTLAS: {
                const auto* update_cmd = static_cast<const UpdateRaytracingSceneCmd*>(&cmd);
                MarkReadAccel(digest, update_cmd->SceneHandle(), EBufferState::SHADER_RESOURCE);
                if (update_cmd->ForceUpdate() && update_cmd->InstancesToUpdate().empty()) {
                    MarkReadBuffer(
                        digest, update_cmd->InstanceBufferHandle(), EBufferState::SHADER_RESOURCE
                    );
                } else {
                    MarkWriteBuffer(
                        digest, update_cmd->InstanceBufferHandle(), EBufferState::UNORDERED_ACCESS
                    );
                }
                MarkWriteBuffer(
                    digest, update_cmd->ScratchBufferHandle(), EBufferState::UNORDERED_ACCESS
                );
                MarkWriteAccel(digest, update_cmd->TlasHandle(), EBufferState::UNORDERED_ACCESS);
                if (update_cmd->ForceUpdate() || update_cmd->RelatedGeometries().empty()) {
                    MarkReadAccel(digest, kGlobalAccelBuildSyncHandle, EBufferState::SHADER_RESOURCE);
                } else {
                    for (const auto& [handle, count] : update_cmd->RelatedGeometries()) {
                        (void)count;
                        MarkReadAccel(digest, handle, EBufferState::SHADER_RESOURCE);
                    }
                }
                break;
            }
            case Command::EType::TraceRay: {
                const auto* trace_cmd = static_cast<const TraceRayCmd*>(&cmd);
                trace_cmd->IterateArgs(
                    [&](const TArg& arg, ParamInfoFlags state_flags) {
                        CollectShaderArg(
                            arg,
                            state_flags,
                            digest,
                            visible_dirty_written_resources
                        );
                    }
                );
                const auto trace_param = trace_cmd->Param();
                if (const auto* indirect = std::get_if<BufferView>(&trace_param)) {
                    MarkReadBuffer(digest, GetHandle(*indirect), EBufferState::INDIRECT);
                }
                break;
            }
            case Command::EType::Custom: {
                const auto* custom_cmd = static_cast<const CustomCmd*>(&cmd);
                if (custom_cmd->CustomId() == CustomCmd::CustomCmdId::CUSTOM_DISPATCH) {
                    const auto* dispatch_cmd = static_cast<const CustomDispatchCmd*>(custom_cmd);
                    dispatch_cmd->IterateArgs(
                        [&](const TArg& arg, ParamInfoFlags state_flags) {
                            CollectShaderArg(
                                arg,
                                state_flags,
                                digest,
                                visible_dirty_written_resources
                            );
                        }
                    );
                }
                break;
            }
            case Command::EType::Scope:
            case Command::EType::Query:
                break;
            default:
                break;
        }
    }

private:
    EQueueType              queue{EQueueType::Ignore};
    const TCachedArgArray&  cached_args;
    DirtyWrittenResources*  dirty_written_resources{nullptr};
};

static size_t EstimateSubmitCount(const Array<RHIExecOp>& ops) {
    size_t submit_count = 0;
    for (const auto& op : ops) {
        if (const auto* submit_op = std::get_if<RHISubmitCmdList>(&op)) {
            submit_count += submit_op->submits.size();
        }
    }
    return submit_count;
}

static size_t EstimatePlatformOpCount(const Array<RHIExecOp>& ops) {
    size_t op_count = 0;
    for (const auto& op : ops) {
        if (const auto* submit_op = std::get_if<RHISubmitCmdList>(&op)) {
            op_count += submit_op->submits.size();
        } else {
            op_count += 1;
        }
    }
    return op_count;
}

// Single CommandList can be submitted to different queues with QueueTransferCmd in between, 
// so we need to split the submit into segments based on CopyScopeCmd.
// TODO: get CopyScopeIndex directly from CmdSubmit(which comes from commandlist during recording) instead of iterating again here
// need to make sure neighbor segment with same queue are merged together to avoid unnecessary split
static Array<CommandSegmentInfo> SplitCommandSegment(const CmdSubmit& submit) {
    Array<CommandSegmentInfo> segments{};
    size_t parent_begin = 0;

    for (size_t cmd_index = 0; cmd_index < submit.cmds.size(); ++cmd_index) {
        const auto* cmd = submit.cmds[cmd_index].get();
        if (cmd == nullptr || cmd->Type() != Command::EType::CopyScope) {
            continue;
        }

        if (parent_begin != cmd_index) {
            segments.emplace_back(CommandSegmentInfo{
                .type = ESegmentType::Graphics,
                .begin = parent_begin,
                .end = cmd_index,
                .copy_scope_index = 0
            });
        }

        segments.emplace_back(CommandSegmentInfo{
            .type = ESegmentType::Copy,
            .begin = 0,
            .end = 0,
            .copy_scope_index = cmd_index
        });
        parent_begin = cmd_index + 1;
    }

    if (parent_begin != submit.cmds.size()) {
        segments.emplace_back(CommandSegmentInfo{
            .type = ESegmentType::Graphics,
            .begin = parent_begin,
            .end = submit.cmds.size(),
            .copy_scope_index = 0
        });
    }

    if (segments.empty()) {
        segments.emplace_back(CommandSegmentInfo{
            .type = ESegmentType::Graphics,
            .begin = 0,
            .end = 0,
            .copy_scope_index = 0
        });
    }

    return segments;
}

//TODO: optimize, we only need span here, no need to create a new array
static Array<const Command*> GetSegmentCommandPointers(
    const CmdSubmit&                    submit,
    const CommandSegmentInfo& segment_info
) {
    Array<const Command*> commands{};
    if (segment_info.type == ESegmentType::Copy) {
        const auto* copy_scope = static_cast<const CopyScopeCmd*>(submit.cmds[segment_info.copy_scope_index].get());
        commands.reserve(copy_scope->Commands().size());
        for (const auto& cmd : copy_scope->Commands()) {
            commands.emplace_back(cmd.get());
        }
        return commands;
    }

    commands.reserve(segment_info.end - segment_info.begin);
    for (size_t cmd_index = segment_info.begin; cmd_index < segment_info.end; ++cmd_index) {
        commands.emplace_back(submit.cmds[cmd_index].get());
    }
    return commands;
}

static TextureView MakeTextureTransferView(const ResourceKey& key) {
    auto* texture = reinterpret_cast<Texture*>(key.handle);
    if (texture == nullptr) {
        return TextureView{};
    }

    const uint8 mip_count   = ResolveTextureMipCount(texture, key);
    const uint8 array_count = ResolveTextureArrayCount(texture, key);
    TextureView view = texture->GetView(key.mip_level, mip_count);
    if (array_count != texture->GetNumArray() || key.array_layer != 0) {
        view = view.Slice(key.array_layer, array_count);
    }
    return view;
}

static BufferView MakeBufferTransferView(uint64 handle) {
    auto* buffer = reinterpret_cast<Buffer*>(handle);
    return buffer != nullptr ? buffer->GetView() : BufferView{};
}

static void ApplyDigestToDirtyWrittenResources(
    DirtyWrittenResources&       dirty_written_resources,
    const ResourceAccessDigest&  digest
) {
    for (const auto& [resource_key, access] : digest) {
        if (!access.read && !access.write) {
            continue;
        }

        if (IsTextureKey(resource_key)) {
            ForEachTextureSubresourceKey(
                resource_key,
                [&](const ResourceKey& subresource_key) {
                    if (access.last_access_write) {
                        dirty_written_resources.emplace(subresource_key);
                    } else {
                        dirty_written_resources.erase(subresource_key);
                    }
                }
            );
            continue;
        }

        if (resource_key.type != ETrackedResourceType::Buffer) {
            continue;
        }

        if (access.last_access_write) {
            dirty_written_resources.emplace(resource_key);
        } else {
            dirty_written_resources.erase(resource_key);
        }
    }
}

static void AddWaitDependency(
    Array<SubmissionKey>&                          wait_keys,
    UnorderedSet<SubmissionKey, SubmissionKeyHash>& dedup,
    const SubmissionKey&                           dependency
) {
    if (dedup.emplace(dependency).second) {
        wait_keys.emplace_back(dependency);
    }
}

static bool AppendPrefixImport(
    CmdSubmitPreprocessResult&                    result,
    UnorderedSet<ResourceKey, ResourceKeyHash>&   imported_resources,
    const ResourceKey&                            resource_key,
    const ResourceStateValue&                     current_state,
    const ResourceAccessDigestEntry&              desired_access,
    EQueueType                                    src_queue
) {
    if (!imported_resources.emplace(resource_key).second) {
        return false;
    }

    if (result.prefix_transfer_queue.has_value() &&
        result.prefix_transfer_queue.value() != src_queue) {
        LOG_ERROR(
            "Segment ({}, {}) needs imports from multiple source queues ({} and {})",
            result.key.op_seq,
            result.key.submit_idx,
            QueueTypeName(result.prefix_transfer_queue.value()),
            QueueTypeName(src_queue)
        );
        assert(false && "Multiple source queues in one segment import are not supported");
        imported_resources.erase(resource_key);
        return false;
    }
    result.prefix_transfer_queue = src_queue;

    switch (resource_key.type) {
        case ETrackedResourceType::Texture:
            result.prefix_import_textures.emplace_back(
                MakeTextureTransferView(resource_key),
                desired_access.texture_state.value_or(current_state.texture_state),
                desired_access.last_access_write
            );
            RHITRACE_LOG(
                verbose,
                "[RHITrace][PreprocessImport] submit=({}, {}) resource_type={} name={} handle=0x{:x} src_queue={} dst_queue={} desired_tex_state={} desired_buf_state={} last_write={}",
                result.key.op_seq,
                result.key.submit_idx,
                ResourceTypeName(resource_key.type),
                ResourceName(resource_key),
                resource_key.handle,
                QueueTypeName(src_queue),
                QueueTypeName(result.queue),
                int(desired_access.texture_state.value_or(current_state.texture_state)),
                -1,
                desired_access.last_access_write
            );
            return true;
        case ETrackedResourceType::Buffer:
            result.prefix_import_buffers.emplace_back(
                MakeBufferTransferView(resource_key.handle),
                desired_access.buffer_state.value_or(current_state.buffer_state),
                desired_access.last_access_write
            );
            RHITRACE_LOG(
                verbose,
                "[RHITrace][PreprocessImport] submit=({}, {}) resource_type={} name={} handle=0x{:x} src_queue={} dst_queue={} desired_tex_state={} desired_buf_state={} last_write={}",
                result.key.op_seq,
                result.key.submit_idx,
                ResourceTypeName(resource_key.type),
                ResourceName(resource_key),
                resource_key.handle,
                QueueTypeName(src_queue),
                QueueTypeName(result.queue),
                -1,
                int(desired_access.buffer_state.value_or(current_state.buffer_state)),
                desired_access.last_access_write
            );
            return true;
        default:
            imported_resources.erase(resource_key);
            return false;
    }
}

static bool AppendSuffixExport(
    CmdSubmitPreprocessResult&                    result,
    UnorderedSet<ResourceKey, ResourceKeyHash>&   exported_resources,
    const ResourceKey&                            resource_key,
    const ResourceStateValue&                     current_state,
    EQueueType                                    dst_queue
) {
    if (!exported_resources.emplace(resource_key).second) {
        return false;
    }

    if (result.suffix_transfer_queue.has_value() &&
        result.suffix_transfer_queue.value() != dst_queue) {
        LOG_ERROR(
            "Segment ({}, {}) needs exports to multiple destination queues ({} and {})",
            result.key.op_seq,
            result.key.submit_idx,
            QueueTypeName(result.suffix_transfer_queue.value()),
            QueueTypeName(dst_queue)
        );
        assert(false && "Multiple destination queues in one segment export are not supported");
        exported_resources.erase(resource_key);
        return false;
    }
    result.suffix_transfer_queue = dst_queue;

    switch (resource_key.type) {
        case ETrackedResourceType::Texture:
            result.suffix_export_textures.emplace_back(
                MakeTextureTransferView(resource_key),
                current_state.texture_state
            );
            RHITRACE_LOG(
                verbose,
                "[RHITrace][PreprocessExport] submit=({}, {}) resource_type={} name={} handle=0x{:x} src_queue={} dst_queue={} tex_state={} buf_state={}",
                result.key.op_seq,
                result.key.submit_idx,
                ResourceTypeName(resource_key.type),
                ResourceName(resource_key),
                resource_key.handle,
                QueueTypeName(result.queue),
                QueueTypeName(dst_queue),
                int(current_state.texture_state),
                -1
            );
            return true;
        case ETrackedResourceType::Buffer:
            result.suffix_export_buffers.emplace_back(
                MakeBufferTransferView(resource_key.handle),
                current_state.buffer_state
            );
            RHITRACE_LOG(
                verbose,
                "[RHITrace][PreprocessExport] submit=({}, {}) resource_type={} name={} handle=0x{:x} src_queue={} dst_queue={} tex_state={} buf_state={}",
                result.key.op_seq,
                result.key.submit_idx,
                ResourceTypeName(resource_key.type),
                ResourceName(resource_key),
                resource_key.handle,
                QueueTypeName(result.queue),
                QueueTypeName(dst_queue),
                -1,
                int(current_state.buffer_state)
            );
            return true;
        default:
            exported_resources.erase(resource_key);
            return false;
    }
}

static bool HasSubmitSideEffects(const CmdSubmit& submit) {
    return !submit.callbacks.empty() || !submit.wait_events.empty() || !submit.signal_events.empty() ||
           !submit.query_tokens.empty() || !submit.gpu_events.empty() || submit.b_sync ||
           submit.b_tick_profiling || submit.b_delete_resources;
}

static void ApplyDigestToState(
    const ResourceAccessDigest& digest,
    EQueueType                  queue,
    const SubmissionKey&        submit_key,
    ResourceStateSnapshot&      state_snapshot
) {
    for (const auto& [resource_key, access] : digest) {
        auto apply_to_state = [&](const ResourceKey& canonical_key) {
            auto& resource_state = state_snapshot[canonical_key];
            const bool was_known = resource_state.known;
            const bool materialize_unknown_state = resource_state.known || access.write;
            if (materialize_unknown_state) {
                resource_state.known = true;

                if (canonical_key.type == ETrackedResourceType::Texture && access.texture_state.has_value()) {
                    resource_state.texture_state = access.texture_state.value();
                }

                if (canonical_key.type != ETrackedResourceType::Texture && access.buffer_state.has_value()) {
                    resource_state.buffer_state = access.buffer_state.value();
                }

                resource_state.has_writer = access.last_access_write;
            }

            if (materialize_unknown_state && (access.read || access.write)) {
                resource_state.owner_queue    = queue;
                resource_state.last_submission = submit_key;
            }

            if (!was_known && resource_state.known) {
                RHITRACE_LOG(
                    verbose,
                    "[RHITrace][PersistentPromote] submit=({}, {}) queue={} resource_type={} handle=0x{:x} mip={} layer={} read={} write={} tex_state={} buf_state={} last_write={}",
                    submit_key.op_seq,
                    submit_key.submit_idx,
                    QueueTypeName(queue),
                    ResourceTypeName(canonical_key.type),
                    canonical_key.handle,
                    canonical_key.type == ETrackedResourceType::Texture ? int(canonical_key.mip_level) : -1,
                    canonical_key.type == ETrackedResourceType::Texture ? int(canonical_key.array_layer) : -1,
                    access.read,
                    access.write,
                    access.texture_state.has_value() ? int(access.texture_state.value()) : -1,
                    access.buffer_state.has_value() ? int(access.buffer_state.value()) : -1,
                    access.last_access_write
                );
            }
        };

        if (IsTextureKey(resource_key)) {
            ForEachTextureSubresourceKey(resource_key, apply_to_state);
        } else {
            apply_to_state(resource_key);
        }
    }
}

class SubmissionPlanExecutor {
public:
    void Execute(Array<PlatformExecOp>&& ops, bool frame_end) const;
};

struct SubmissionBatch {
    Array<PlatformExecOp> ops{};
    bool frame_end{false};
    uint64 batch_id{0};
    std::shared_ptr<std::promise<void>> completion{};
};

struct PendingPresentCompletion {
    UniquePtr<VulkanPresentor> presentor{};
    uint64 timeline_value{0};
};


static EPassType QueueTypeToPassType(EQueueType queue) {
    switch (queue) {
        case EQueueType::Graphics:
            return EPassType::Graphics;
        case EQueueType::Compute:
            return EPassType::Compute;
        case EQueueType::Copy:
            return EPassType::Copy;
        case EQueueType::Ignore:
        case EQueueType::Num:
        default:
            return EPassType::Graphics;
    }
}

static std::tuple<VkAccessFlags2, VkImageLayout, VkPipelineStageFlags2> GetTrackedTextureState(
    VulkanTexture*            texture,
    const ResourceStateValue& state,
    EQueueType                queue
) {
    VkTracker tracker(queue);
    const EPassType pass_type = QueueTypeToPassType(queue);
    if (state.has_writer) {
        return tracker.WriteTexture(texture, state.texture_state, pass_type);
    }
    return tracker.ReadTexture(texture, state.texture_state, pass_type);
}

static std::tuple<VkAccessFlags2, VkPipelineStageFlags2> GetTrackedBufferState(
    VulkanBuffer*             buffer,
    const ResourceStateValue& state,
    EQueueType                queue
) {
    VkTracker tracker(queue);
    const EPassType pass_type = QueueTypeToPassType(queue);
    if (state.has_writer) {
        return tracker.WriteBuffer(buffer, state.buffer_state, pass_type);
    }
    return tracker.ReadBuffer(buffer, state.buffer_state, pass_type);
}

struct SubmissionHostWaitTask {
    uint64 op_seq{0};
    Array<WaitEvent> wait_events{};
    std::promise<void> completion{};
};

struct SubmissionPayloadTask {
    std::function<void()> callback{};
};

using SubmissionEventTask = std::variant<SubmissionHostWaitTask, SubmissionPayloadTask>;

class SubmissionPresentContext {
public:
    explicit SubmissionPresentContext(EQueueType in_queue_type) :
        queue_type(in_queue_type),
        command_queue(static_cast<VkCommandQueue&>(RenderDevice::Get().GetCommandQueue(in_queue_type))),
        native_queue(in_queue_type, command_queue.vk_device),
        timeline(MoerNew(VulkanFence(command_queue.vk_device))) {
        completion_thread = std::jthread([this]() {
            CompletionThreadMain();
        });
    }

    ~SubmissionPresentContext() {
        Shutdown();
        Array<VulkanPresentor*> cached_presentors{};
        presentors.PopAll(cached_presentors);
        for (auto* presentor : cached_presentors) {
            MoerDelete(presentor);
        }
    }

    SubmissionPresentContext(const SubmissionPresentContext&)                = delete;
    SubmissionPresentContext& operator=(const SubmissionPresentContext&)     = delete;
    SubmissionPresentContext(SubmissionPresentContext&&) noexcept            = delete;
    SubmissionPresentContext& operator=(SubmissionPresentContext&&) noexcept = delete;

    bool Present(const RHIPresentOp& present_op, std::span<const WaitEvent> wait_events) {
        if (!present_op.swapchain || !present_op.target.texture) {
            return false;
        }

        auto* swapchain = ResourceCast(present_op.swapchain.Get());
        if (swapchain == nullptr) {
            return false;
        }

        std::unique_lock<std::mutex> queue_submit_lock(command_queue.GetSubmitMutex());
        std::unique_lock<std::mutex> lock(submit_mutex);
        WaitForReusablePresentSlot(*swapchain);

        auto presentor = AcquirePresentor();
        auto& cmd_list = presentor->GetCmdList();
        auto& tracker  = presentor->GetTracker();

        auto [ready_semaphore, image_index, present_index] = swapchain->AquireNextImage();
        if (image_index == UINT32_MAX) {
            presentor->Reset();
            presentors.Push(presentor.release());
            return false;
        }

        auto* src_texture       = static_cast<VulkanTexture*>(present_op.target.texture);
        auto* swapchain_texture = ResourceCast(swapchain->GetSwapchainImage(image_index).texture);

        cmd_list.Begin();
        cmd_list.BeginLabel("Submission Present", {0.0f, 1.0f, 1.0f, 1.0f});
        tracker.SetPassType(EPassType::Graphics);

        //TODO: should load from a snapshot from preprocess
        ResourceStateValue src_state{};
        src_state.known = true;
        if (src_state.known) {
            auto [src_access, src_layout, src_stage] =
                std::make_tuple(VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
            tracker.SeedSrcState(src_texture, src_access, src_layout, src_stage);
            RHITRACE_RESOURCE_LOG(
                src_texture->GetName(),
                "[ResourceTrace][Present][SeedSrc] {} : persistent known={} layout={} access=0x{:x}",
                src_texture->GetName(),
                src_state.known,
                VkLayoutStr(src_layout),
                uint64(src_access)
            );
        } else {
            RHITRACE_RESOURCE_LOG(
                src_texture->GetName(),
                "[ResourceTrace][Present][SeedSrc] {} : persistent state UNKNOWN",
                src_texture->GetName()
            );
        }
        tracker.RecordState(src_texture, tracker.ReadTexture(src_texture, ETextureState::TRANSFER));
        tracker.RecordState(
            swapchain_texture, tracker.WriteTexture(swapchain_texture, ETextureState::TRANSFER)
        );
        tracker.ResolveBarriers();
        tracker.DispatchBarriers(cmd_list);

        cmd_list.InsertLabel("Copy Present Image", {0.0f, 0.0f, 0.0f, 1.0f});
        cmd_list.CopyTexture(
            src_texture, swapchain_texture, present_op.target.extent, {0, 0, 0}, {0, 0, 0}, 0, 0
        );
        tracker.RecordState(
            swapchain_texture,
            VK_ACCESS_2_NONE,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COPY_BIT
        );
        tracker.ResolveBarriers();
        tracker.DispatchBarriers(cmd_list);
        cmd_list.EndLabel();
        cmd_list.End();
        tracker.Reset();

        const uint64 completion_value = ++last_submitted_timeline;
        native_queue.Signal(timeline.Get(), completion_value, VK_PIPELINE_STAGE_2_COPY_BIT);
        for (const auto& wait_event : wait_events) {
            auto* fence = reinterpret_cast<VulkanFence*>(wait_event.timeline_handle);
            if (fence == nullptr) {
                continue;
            }
            native_queue.Wait(fence, wait_event.value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        }
        native_queue.Wait(ready_semaphore, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        native_queue.Signal(swapchain->GetRenderFinishedFence(), VK_PIPELINE_STAGE_2_COPY_BIT);
        native_queue.Submit(cmd_list);

        VkSemaphore render_finished_semaphore = swapchain->GetRenderFinishedFence();
        VkFence     in_flight_fence           = swapchain->GetInFlightFence(present_index);
        VkSwapchainPresentFenceInfoEXT present_fence_info{
            VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT
        };
        present_fence_info.swapchainCount = 1;
        present_fence_info.pFences        = &in_flight_fence;

        VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present_info.pNext              = &present_fence_info;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores    = &render_finished_semaphore;
        present_info.swapchainCount     = 1;
        present_info.pSwapchains        = &swapchain->handle;
        present_info.pImageIndices      = &image_index;

        VkResult result = vkQueuePresentKHR(command_queue.vk_device.GetPresentQueue(), &present_info);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR) {
            LOG_ERROR("vkQueuePresentKHR failed with result {}", int(result));
        }
        ++swapchain->image_idx;

        EnqueueCompletion(
            PendingPresentCompletion{
                .presentor = std::move(presentor),
                .timeline_value = completion_value
            }
        );
        TexturePersistentState persistent_state{};
        persistent_state.known            = true;
        persistent_state.owner_queue      = queue_type;
        persistent_state.state            = ETextureState::TRANSFER;
        persistent_state.last_access_kind = ERHIResourceLastAccessKind::Read;
        for (uint8 mip = 0; mip < present_op.target.texture->GetNumMips(); ++mip) {
            for (uint8 layer = 0; layer < present_op.target.texture->GetNumArray(); ++layer) {
                present_op.target.texture->SetPersistentState(mip, layer, persistent_state);
            }
        }
        RHITRACE_RESOURCE_LOG(
            src_texture->GetName(),
            "[ResourceTrace][Present][SetPersistent] {} : state=TRANSFER owner_queue={} access=Read",
            src_texture->GetName(),
            int(queue_type)
        );
        return true;
    }

    void Flush() {
        Complete(last_submitted_timeline.load(std::memory_order_acquire));
    }

    void Shutdown() {
        Flush();
        bool expected = true;
        if (!running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return;
        }
        completion_cv.notify_all();
        if (completion_thread.joinable()) {
            completion_thread.join();
        }
    }

private:
    UniquePtr<VulkanPresentor> AcquirePresentor() {
        std::lock_guard<std::mutex> lock(presentor_mutex);
        auto presentor = UniquePtr<VulkanPresentor>(presentors.Pop());
        if (presentor) {
            return presentor;
        }
        return MakeUnique<VulkanPresentor>(&command_queue.vk_device, queue_type);
    }

    void EnqueueCompletion(PendingPresentCompletion&& completion) {
        {
            std::lock_guard<std::mutex> lock(completion_mutex);
            pending_presentors.emplace_back(std::move(completion));
        }
        completion_cv.notify_one();
    }

    void Complete(uint64 target_timeline) {
        while (completed_timeline.load(std::memory_order_acquire) < target_timeline) {
            PumpCompletions(false);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    void CompletionThreadMain() {
        Platform::SetCurrentThreadName("SubmissionPresentThread");
        while (running.load(std::memory_order_acquire)) {
            PumpCompletions(true);
        }
        PumpCompletions(false);
    }

    void PumpCompletions(bool wait_for_work) {
        while (true) {
            PendingPresentCompletion completion{};
            {
                std::unique_lock<std::mutex> lock(completion_mutex);
                if (pending_presentors.empty()) {
                    if (wait_for_work) {
                        completion_cv.wait_for(lock, std::chrono::milliseconds(1), [this]() {
                            return !running.load(std::memory_order_acquire) ||
                                   !pending_presentors.empty();
                        });
                    }
                    if (pending_presentors.empty()) {
                        return;
                    }
                }

                const uint64 device_value = timeline->GetDeviceValue();
                if (device_value < pending_presentors.front().timeline_value) {
                    if (!wait_for_work) {
                        return;
                    }
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    continue;
                }

                timeline->Notify(device_value);
                completion = std::move(pending_presentors.front());
                pending_presentors.pop_front();
                completed_timeline.store(
                    std::max(completed_timeline.load(std::memory_order_relaxed), completion.timeline_value),
                    std::memory_order_release
                );
            }

            completion.presentor->VulkanAllocatorBase::Complete(
                timeline.Get(), completion.timeline_value
            );
            completion.presentor->Reset();
            presentors.Push(completion.presentor.release());
        }
    }

    void WaitForReusablePresentSlot(VkSwapchain& swapchain) {
        const uint64 present_index = swapchain.image_idx;
        if (present_index < swapchain.max_frames_in_flight) {
            return;
        }

        VkFence fence = swapchain.GetInFlightFence(present_index);
        while (true) {
            const VkResult result = vkGetFenceStatus(command_queue.vk_device.GetDevice(), fence);
            if (result == VK_SUCCESS) {
                vkResetFences(command_queue.vk_device.GetDevice(), 1, &fence);
                return;
            }
            if (result != VK_NOT_READY) {
                LOG_ERROR(
                    "vkGetFenceStatus failed on present fence slot {} with result {}",
                    present_index % swapchain.max_frames_in_flight,
                    int(result)
                );
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

private:
    EQueueType     queue_type{EQueueType::Graphics};
    VkCommandQueue& command_queue;
    VkNativeQueue   native_queue;
    VulkanFenceRef  timeline = nullptr;

    std::atomic_bool   running{true};
    std::atomic_uint64_t last_submitted_timeline{0};
    std::atomic_uint64_t completed_timeline{0};

    std::mutex submit_mutex{};
    std::mutex presentor_mutex{};
    LockFreeQueueBase<VulkanPresentor, false> presentors{};

    std::mutex completion_mutex{};
    std::condition_variable completion_cv{};
    std::deque<PendingPresentCompletion> pending_presentors{};
    std::jthread completion_thread{};
};

class SubmissionPresentContextManager {
public:
    SubmissionPresentContext& Get(EQueueType queue_type) {
        std::lock_guard<std::mutex> lock(context_mutex);
        auto& context = contexts[static_cast<size_t>(queue_type)];
        if (!context) {
            context = std::make_unique<SubmissionPresentContext>(queue_type);
        }
        return *context;
    }

    void Flush() {
        std::lock_guard<std::mutex> lock(context_mutex);
        for (auto& context : contexts) {
            if (context) {
                context->Flush();
            }
        }
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(context_mutex);
        for (auto& context : contexts) {
            if (context) {
                context->Shutdown();
                context.reset();
            }
        }
    }

private:
    std::mutex context_mutex{};
    std::array<std::unique_ptr<SubmissionPresentContext>, static_cast<size_t>(EQueueType::Num)> contexts{};
};

static SubmissionPresentContextManager& GetSubmissionPresentContextManager();

class SubmissionPlanRuntime {
public:
    SubmissionPlanRuntime() :
        submission_thread([this]() { RunSubmissionThread(); }),
        submission_event_thread([this]() { RunSubmissionEventThread(); }) {}

    ~SubmissionPlanRuntime() {
        Stop();
    }

    void Enqueue(Array<PlatformExecOp>&& ops, bool frame_end) {
        if (ops.empty() && !frame_end) {
            return;
        }
        SubmissionBatch batch{};
        batch.ops = std::move(ops);
        batch.frame_end = frame_end;
        batch.batch_id = next_batch_id.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(submission_mutex);
            submission_queue.emplace_back(std::move(batch));
        }
        submission_cv.notify_one();
    }

    void Flush() {
        if (!running.load(std::memory_order_acquire)) {
            return;
        }
        auto completion = std::make_shared<std::promise<void>>();
        auto future     = completion->get_future();

        SubmissionBatch batch{};
        batch.batch_id   = next_batch_id.fetch_add(1, std::memory_order_relaxed);
        batch.completion = completion;
        {
            std::lock_guard<std::mutex> lock(submission_mutex);
            submission_queue.emplace_back(std::move(batch));
        }
        submission_cv.notify_one();
        future.wait();
    }

    void Shutdown() {
        Flush();
        Stop();
        if (submission_thread.joinable()) {
            submission_thread.join();
        }
        if (submission_event_thread.joinable()) {
            submission_event_thread.join();
        }
    }

private:
    void Stop() {
        bool expected_running = true;
        if (!running.compare_exchange_strong(expected_running, false)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(submission_mutex);
            for (auto& batch : submission_queue) {
                if (batch.completion) {
                    try {
                        batch.completion->set_value();
                    } catch (const std::future_error&) {
                    }
                }
            }
            submission_queue.clear();
        }
        {
            std::lock_guard<std::mutex> lock(submission_event_mutex);
            for (auto& task : submission_event_queue) {
                if (auto* wait_task = std::get_if<SubmissionHostWaitTask>(&task)) {
                    SafeResolveWaitTask(*wait_task);
                }
            }
            submission_event_queue.clear();
        }
        submission_cv.notify_all();
        submission_event_cv.notify_all();
    }

    void RunSubmissionThread() {
        Platform::SetCurrentThreadName("SubmissionThread");
        while (true) {
            SubmissionBatch batch{};
            {
                std::unique_lock<std::mutex> lock(submission_mutex);
                submission_cv.wait(lock, [this]() {
                    return !running.load(std::memory_order_acquire) || !submission_queue.empty();
                });
                if (!running.load(std::memory_order_acquire) && submission_queue.empty()) {
                    return;
                }
                batch = std::move(submission_queue.front());
                submission_queue.pop_front();
            }

            UnorderedMap<SubmissionKey, WaitEvent, SubmissionKeyHash> completion_by_submit{};
            completion_by_submit.reserve(static_cast<uint32>(batch.ops.size()));
            uint32 submission_count = 0;
            uint32 present_count    = 0;
            for (auto& op : batch.ops) {
                std::visit(
                    Overload{
                        [&](QueueSubmissionInfo& submit_info) {
                            if (ExecuteSubmit(submit_info, completion_by_submit)) {
                                ++submission_count;
                            }
                        },
                        [&](PresentInfo& present_info) {
                            if (ExecutePresentTask(present_info, completion_by_submit)) {
                                ++present_count;
                            }
                        }
                    },
                    op
                );
            }

            if (batch.frame_end) {
                QueuePayloadTask([batch_id = batch.batch_id, submission_count, present_count]() {
                    RHITRACE_LOG(
                        basic,
                        "[RHITrace][FrameEnd] batch={} submits={} presents={}",
                        batch_id,
                        submission_count,
                        present_count
                    );
                });
            }
            if (batch.completion) {
                try {
                    batch.completion->set_value();
                } catch (const std::future_error&) {
                }
            }
        }
    }

    bool ExecuteSubmit(
        QueueSubmissionInfo& submit_info,
        UnorderedMap<SubmissionKey, WaitEvent, SubmissionKeyHash>& completion_by_submit
    ) {
        if (!submit_info.valid) {
            return false;
        }

        for (const auto& dependency : submit_info.wait_submission_keys) {
            const auto completion_it = completion_by_submit.find(dependency);
            if (completion_it == completion_by_submit.end()) {
                LOG_ERROR(
                    "Missing dependency completion for submit ({}, {})",
                    dependency.op_seq,
                    dependency.submit_idx
                );
                continue;
            }
            if (submit_info.recorded_submit.has_value() && submit_info.recorded_submit->submit.has_value()) {
                submit_info.recorded_submit->submit->wait_events.emplace_back(completion_it->second);
                RHITRACE_LOG(
                    verbose,
                    "[RHITrace][SubmitPlan] submit=({}, {}) queue={} append_wait fence={} value={}",
                    submit_info.key.op_seq,
                    submit_info.key.submit_idx,
                    QueueTypeName(submit_info.queue),
                    completion_it->second.timeline_handle,
                    completion_it->second.value
                );
            }
        }

        WaitEvent completion_event{};
        if (!submit_info.recorded_submit.has_value()) {
            LOG_ERROR(
                "Missing recorded submit packet for ({}, {})",
                submit_info.key.op_seq,
                submit_info.key.submit_idx
            );
            return false;
        }
        switch (submit_info.queue) {
            case EQueueType::Graphics:
            case EQueueType::Compute: {
                auto& queue =
                    static_cast<VkCommandQueue&>(RenderDevice::Get().GetCommandQueue(submit_info.queue));
                completion_event = queue.SubmitRecorded(std::move(submit_info.recorded_submit.value()));
                break;
            }
            case EQueueType::Copy: {
                auto& copy_queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
                IOWaitEvt io_completion = copy_queue.SubmitRecorded(std::move(submit_info.recorded_submit.value()));
                completion_event = WaitEvent{io_completion.handle, io_completion.timeline};
                break;
            }
            default:
                LOG_ERROR("Invalid queue type in submission plan: {}", QueueTypeName(submit_info.queue));
                return false;
        }
        RHITRACE_LOG(
            basic,
            "[RHITrace][SubmitPlan] submitted=({}, {}) queue={} completion fence={} value={}",
            submit_info.key.op_seq,
            submit_info.key.submit_idx,
            QueueTypeName(submit_info.queue),
            completion_event.timeline_handle,
            completion_event.value
        );

        completion_by_submit.emplace(submit_info.key, completion_event);
        return true;
    }

    bool ExecutePresentTask(
        PresentInfo& present_info,
        const UnorderedMap<SubmissionKey, WaitEvent, SubmissionKeyHash>& completion_by_submit
    ) {
        if (!present_info.valid || !present_info.present.swapchain || !present_info.present.target.texture) {
            return false;
        }

        Array<WaitEvent> wait_events{};
        wait_events.reserve(present_info.wait_submission_keys.size());
        for (const auto& dependency : present_info.wait_submission_keys) {
            const auto completion_it = completion_by_submit.find(dependency);
            if (completion_it == completion_by_submit.end()) {
                LOG_ERROR(
                    "Missing dependency completion for present dependency ({}, {})",
                    dependency.op_seq,
                    dependency.submit_idx
                );
                present_info.valid = false;
                return false;
            }
            wait_events.emplace_back(completion_it->second);
            RHITRACE_LOG(
                basic,
                "[RHITrace][PresentPlan] op_seq={} wait_dep=({}, {}) fence={} value={}",
                present_info.op_seq,
                dependency.op_seq,
                dependency.submit_idx,
                completion_it->second.timeline_handle,
                completion_it->second.value
            );
        }

        if (present_info.present.queue == EQueueType::Copy || present_info.present.queue == EQueueType::Ignore) {
            LOG_ERROR("Invalid present queue type: {}", QueueTypeName(present_info.present.queue));
            return false;
        }
        RHITRACE_LOG(
            basic,
            "[RHITrace][PresentPlan] presenting op_seq={} queue={} wait_count={}",
            present_info.op_seq,
            QueueTypeName(present_info.present.queue),
            wait_events.size()
        );
        return GetSubmissionPresentContextManager().Get(present_info.present.queue)
            .Present(present_info.present, wait_events);
    }

    void RunSubmissionEventThread() {
        Platform::SetCurrentThreadName("SubmissionEventThread");
        while (true) {
            SubmissionEventTask task{};
            {
                std::unique_lock<std::mutex> lock(submission_event_mutex);
                submission_event_cv.wait(lock, [this]() {
                    return !running.load(std::memory_order_acquire) ||
                           !submission_event_queue.empty();
                });
                if (!running.load(std::memory_order_acquire) && submission_event_queue.empty()) {
                    return;
                }
                task = std::move(submission_event_queue.front());
                submission_event_queue.pop_front();
            }
            std::visit(
                Overload{
                    [this](SubmissionHostWaitTask& wait_task) {
                        ExecuteHostWaitTask(wait_task);
                    },
                    [](SubmissionPayloadTask& payload_task) {
                        if (payload_task.callback) {
                            payload_task.callback();
                        }
                    }
                },
                task
            );
        }
    }

    void EnqueueHostWaitAndWait(uint64 op_seq, Array<WaitEvent>&& wait_events) {
        SubmissionHostWaitTask task{};
        task.op_seq      = op_seq;
        task.wait_events = std::move(wait_events);
        auto completion_future = task.completion.get_future();
        {
            std::lock_guard<std::mutex> lock(submission_event_mutex);
            submission_event_queue.emplace_back(std::move(task));
        }
        submission_event_cv.notify_one();
        completion_future.wait();
    }

    void QueuePayloadTask(std::function<void()>&& callback) {
        SubmissionPayloadTask task{};
        task.callback = std::move(callback);
        {
            std::lock_guard<std::mutex> lock(submission_event_mutex);
            submission_event_queue.emplace_back(std::move(task));
        }
        submission_event_cv.notify_one();
    }

    static void SafeResolveWaitTask(SubmissionHostWaitTask& wait_task) {
        try {
            wait_task.completion.set_value();
        } catch (const std::future_error&) {
        }
    }

    static bool IsWaitEventCompleted(const WaitEvent& wait_evt) {
        auto* fence = reinterpret_cast<VulkanFence*>(wait_evt.timeline_handle);
        if (fence == nullptr) {
            return true;
        }
        return fence->GetDeviceValue() >= wait_evt.value;
    }

    static void ExecuteHostWaitTask(SubmissionHostWaitTask& task) {
        for (const auto& wait_evt : task.wait_events) {
            auto* fence = reinterpret_cast<VulkanFence*>(wait_evt.timeline_handle);
            if (fence == nullptr) {
                continue;
            }
            const auto wait_begin = std::chrono::steady_clock::now();
            const auto warn_step  = std::chrono::seconds(2);
            uint32     warn_count = 0;
            while (!IsWaitEventCompleted(wait_evt)) {
                const auto now     = std::chrono::steady_clock::now();
                const auto elapsed = now - wait_begin;
                if (elapsed >= warn_step * (warn_count + 1)) {
                    LOG_WARNING(
                        "Host wait pending op_seq={} fence={} target={} current={} elapsed_ms={}",
                        task.op_seq,
                        wait_evt.timeline_handle,
                        wait_evt.value,
                        fence->GetDeviceValue(),
                        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                    );
                    ++warn_count;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            RHITRACE_LOG(
                verbose,
                "[RHITrace][SubmissionEvent] host_wait done op_seq={} fence={} value={}",
                task.op_seq,
                wait_evt.timeline_handle,
                wait_evt.value
            );
        }
        SafeResolveWaitTask(task);
    }

private:
    std::atomic_bool running{true};
    std::atomic_uint64_t next_batch_id{1};

    std::mutex submission_mutex{};
    std::condition_variable submission_cv{};
    std::deque<SubmissionBatch> submission_queue{};
    std::jthread submission_thread{};

    std::mutex submission_event_mutex{};
    std::condition_variable submission_event_cv{};
    std::deque<SubmissionEventTask> submission_event_queue{};
    std::jthread submission_event_thread{};
};

static std::mutex g_submission_runtime_mutex{};
static std::unique_ptr<SubmissionPlanRuntime> g_submission_runtime{};
static std::mutex g_present_context_mutex{};
static std::unique_ptr<SubmissionPresentContextManager> g_present_context_manager{};
static SubmissionPlanRuntime& GetSubmissionPlanRuntime() {
    std::lock_guard<std::mutex> lock(g_submission_runtime_mutex);
    if (!g_submission_runtime) {
        g_submission_runtime = std::make_unique<SubmissionPlanRuntime>();
    }
    return *g_submission_runtime;
}

static SubmissionPresentContextManager& GetSubmissionPresentContextManager() {
    std::lock_guard<std::mutex> lock(g_present_context_mutex);
    if (!g_present_context_manager) {
        g_present_context_manager = std::make_unique<SubmissionPresentContextManager>();
    }
    return *g_present_context_manager;
}

static void FlushSubmissionPlanRuntime() {
    std::lock_guard<std::mutex> lock(g_submission_runtime_mutex);
    if (g_submission_runtime) {
        g_submission_runtime->Flush();
    }
}

static void FlushSubmissionPresentContexts() {
    std::lock_guard<std::mutex> lock(g_present_context_mutex);
    if (g_present_context_manager) {
        g_present_context_manager->Flush();
    }
}

static void ShutdownSubmissionPlanRuntime() {
    std::unique_ptr<SubmissionPlanRuntime> runtime{};
    {
        std::lock_guard<std::mutex> lock(g_submission_runtime_mutex);
        runtime = std::move(g_submission_runtime);
    }
    if (runtime) {
        runtime->Shutdown();
    }
}

static void ShutdownSubmissionPresentContexts() {
    std::unique_ptr<SubmissionPresentContextManager> manager{};
    {
        std::lock_guard<std::mutex> lock(g_present_context_mutex);
        manager = std::move(g_present_context_manager);
    }
    if (manager) {
        manager->Shutdown();
    }
}

void SubmissionPlanExecutor::Execute(Array<PlatformExecOp>&& ops, bool frame_end) const {
    GetSubmissionPlanRuntime().Enqueue(std::move(ops), frame_end);
}

static ExecutePreprocessStore
PreprocessFrameOps(const Array<RHIExecOp>& ops) {
    TRACE_SCOPE_CAT("Vulkan.PreprocessFrameOps", "RHI");
    ExecutePreprocessStore preprocess_store{};
    preprocess_store.Reserve(static_cast<uint32>(EstimateSubmitCount(ops) * 3 + 1));
    DirtyWrittenResources dirty_written_resources{};

    UnorderedMap<SubmissionKey, UnorderedSet<ResourceKey, ResourceKeyHash>, SubmissionKeyHash>
        exported_resources_by_submit{};

    uint64 op_seq = 0;
    for (const auto& op : ops) {
        if (const auto* submit_op = std::get_if<RHISubmitCmdList>(&op)) {
            uint32 next_segment_submit_idx = 0;
            for (uint32 submit_idx = 0; submit_idx < submit_op->submits.size(); ++submit_idx) {
                const CmdSubmit& submit = submit_op->submits[submit_idx];
                const SourceSubmitKey source_key{op_seq, submit_idx};
                const auto segments = SplitCommandSegment(submit);
                const bool has_side_effects = HasSubmitSideEffects(submit);

                SourceSubmitPlan source_plan{};
                source_plan.source_key   = source_key;
                source_plan.parent_queue = submit_op->queue;

                std::optional<SubmissionKey> previous_segment_key{};
                EQueueType previous_segment_queue = EQueueType::Ignore;

                for (size_t segment_index = 0; segment_index < segments.size(); ++segment_index) {
                    const auto& segment = segments[segment_index];
                    const bool include =
                        !segment.IsEmpty(submit) ||
                        (segments.size() == 1 && segment.type == ESegmentType::Graphics &&
                         has_side_effects);

                    if (!include) {
                        continue;
                    }

                    CmdSubmitPreprocessResult preprocess_result{};
                    preprocess_result.key          = SubmissionKey{op_seq, next_segment_submit_idx++};
                    preprocess_result.source_key   = source_key;
                    preprocess_result.queue        =
                        segment.type == ESegmentType::Copy ? EQueueType::Copy :
                                                                      submit_op->queue;
                    preprocess_result.segment_type = segment.type;
                    preprocess_result.include      = true;

                    ResourceAccessCollector collector(
                        preprocess_result.queue,
                        submit.cached_args,
                        &dirty_written_resources
                    );
                    Array<const Command*> command_ptrs = GetSegmentCommandPointers(submit, segment);
                    preprocess_result.digest = collector.Collect(
                        std::span<const Command* const>(command_ptrs.data(), command_ptrs.size())
                    );
                    TraceDigest(
                        preprocess_result.key,
                        preprocess_result.queue,
                        preprocess_result.segment_type,
                        preprocess_result.digest
                    );

                    // §3.3: Load initial state for this segment's resources directly from each
                    // resource's persistent storage (committed by the previous segment).
                    ResourceStateSnapshot segment_state{};
                    EnsureDigestStateLoaded(segment_state, preprocess_result.digest);
                    preprocess_result.initial_state_snapshot = segment_state;

                    //hint: begin handle cross queue wait/signal
                    if (previous_segment_key.has_value() &&
                        previous_segment_queue != preprocess_result.queue) {
                        UnorderedSet<SubmissionKey, SubmissionKeyHash> local_waits{};
                        AddWaitDependency(
                            preprocess_result.wait_submission_keys,
                            local_waits,
                            previous_segment_key.value()
                        );
                        RHITRACE_LOG(
                            verbose,
                            "[RHITrace][PreprocessWait] submit=({}, {}) queue={} depends_on=({}, {}) reason=segment_queue_change from={}",
                            preprocess_result.key.op_seq,
                            preprocess_result.key.submit_idx,
                            QueueTypeName(preprocess_result.queue),
                            previous_segment_key->op_seq,
                            previous_segment_key->submit_idx,
                            QueueTypeName(previous_segment_queue)
                        );
                    }

                    //TODO: its not that complicated, we simply treat Copy scope as a special segment
                    // if copy scope has (a, b, c, d) as copy target
                    // we cross tranfer from gfx -> copy (a, b, c, d) and skip those has unknown state in gfx
                    // after copy scope, we cross transfer from copy -> gfx for (a, b, c, d)
                    UnorderedSet<ResourceKey, ResourceKeyHash> imported_resources{};
                    UnorderedSet<SubmissionKey, SubmissionKeyHash> dependency_keys{};
                    for (const auto& existing_wait_key : preprocess_result.wait_submission_keys) {
                        dependency_keys.emplace(existing_wait_key);
                    }

                    //TODO: current implementation is too heavy
                    // we currently make sure that gfx and copy are executed subsequently and resources are immediatelytransferred to graphics queue
                    // after each copy scope(done aquire on copyscope's next segment, which has to be graphics segment, do as prefix aquire)
                    // wait signal are quite simple, we make sure this -> gfx_1 -> copy_1 -> gfx_2
                    // so we only need to make dependency copy_1 depends on gfx_1, and gfx_2 depends on copy_1, and we transfer resources from gfx_1 to copy_1, and then from copy_1 to gfx_2,
                    // can store dependency in preprocess result for each segment, and resolve timeline value in submission time, which will be much more efficient, and also much more flexible for future when we have async compute and async copy
                    for (const auto& [resource_key, access] : preprocess_result.digest) {
                        if (!access.read && !access.write) {
                            continue;
                        }

                        auto process_cross_queue_resource =
                            [&](const ResourceKey& canonical_key) {
                                const auto state_it =
                                    preprocess_result.initial_state_snapshot.find(canonical_key);
                                if (state_it == preprocess_result.initial_state_snapshot.end()) {
                                    return;
                                }

                                const auto& resource_state = state_it->second;
                                if (!resource_state.known ||
                                    resource_state.owner_queue == EQueueType::Ignore ||
                                    resource_state.owner_queue == preprocess_result.queue ||
                                    !resource_state.last_submission.has_value()) {
                                    return;
                                }

                                AddWaitDependency(
                                    preprocess_result.wait_submission_keys,
                                    dependency_keys,
                                    resource_state.last_submission.value()
                                );
                                const bool imported = AppendPrefixImport(
                                    preprocess_result,
                                    imported_resources,
                                    canonical_key,
                                    resource_state,
                                    access,
                                    resource_state.owner_queue
                                );
                                if (imported) {
                                    auto& seed_state =
                                        preprocess_result.initial_state_snapshot[canonical_key];
                                    seed_state.known       = false;
                                    seed_state.has_writer  = false;
                                    seed_state.owner_queue = EQueueType::Ignore;
                                    if (access.texture_state.has_value()) {
                                        seed_state.texture_state = access.texture_state.value();
                                    }
                                    if (access.buffer_state.has_value()) {
                                        seed_state.buffer_state = access.buffer_state.value();
                                    }
                                }

                                auto* producer_result =
                                    preprocess_store.FindMutable(resource_state.last_submission.value());
                                if (producer_result != nullptr) {
                                    auto& exported_resources = exported_resources_by_submit
                                        [resource_state.last_submission.value()];
                                    AppendSuffixExport(
                                        *producer_result,
                                        exported_resources,
                                        canonical_key,
                                        resource_state,
                                        preprocess_result.queue
                                    );
                                }
                            };

                        if (IsTextureKey(resource_key)) {
                            ForEachTextureSubresourceKey(resource_key, process_cross_queue_resource);
                        } else {
                            process_cross_queue_resource(resource_key);
                        }
                    }

                    std::sort(
                        preprocess_result.wait_submission_keys.begin(),
                        preprocess_result.wait_submission_keys.end(),
                        SubmissionKeyLess
                    );

                    ApplyDigestToState(
                        preprocess_result.digest,
                        preprocess_result.queue,
                        preprocess_result.key,
                        segment_state
                    );
                    preprocess_result.last_state_snapshot = segment_state;
                    RHITRACE_LOG(
                        basic,
                        "[RHITrace][PreprocessSegment] submit=({}, {}) source=({}, {}) queue={} segment={} digest_count={} wait_count={} import_tex={} import_buf={} export_tex={} export_buf={}",
                        preprocess_result.key.op_seq,
                        preprocess_result.key.submit_idx,
                        preprocess_result.source_key.op_seq,
                        preprocess_result.source_key.submit_idx,
                        QueueTypeName(preprocess_result.queue),
                        SegmentKindName(preprocess_result.segment_type),
                        preprocess_result.digest.size(),
                        preprocess_result.wait_submission_keys.size(),
                        preprocess_result.prefix_import_textures.size(),
                        preprocess_result.prefix_import_buffers.size(),
                        preprocess_result.suffix_export_textures.size(),
                        preprocess_result.suffix_export_buffers.size()
                    );

                    source_plan.segments.emplace_back(SourceSubmitSegmentPlan{
                        .key = preprocess_result.key,
                        .queue = preprocess_result.queue,
                        .kind = preprocess_result.segment_type,
                        .include = true
                    });
                    previous_segment_key   = preprocess_result.key;
                    previous_segment_queue = preprocess_result.queue;
                    // §3.3: Commit each segment's final state back to resource persistent storage so
                    // subsequent segments (and future frames) read the latest state from the resource
                    // objects themselves, not from a frame-level chained snapshot.
                    CommitPersistentResourceStates(preprocess_result.last_state_snapshot);
                    ApplyDigestToDirtyWrittenResources(
                        dirty_written_resources,
                        preprocess_result.digest
                    );
                    preprocess_store.Add(std::move(preprocess_result));
                }

                preprocess_store.AddSourcePlan(std::move(source_plan));
            }
        } else if (const auto* present_op = std::get_if<RHIPresentOp>(&op)) {
            // §present: Record present target as TRANSFER read so the next frame's tracker seed
            // knows the image is in TRANSFER_SRC_OPTIMAL after the blit (VUID-09592).
            // The target texture's actual persistent state is read via LoadPersistentState (called
            // by EnsureDigestStateLoaded) — no hardcoded state here.
            if (present_op->target.texture) {
                ResourceAccessDigest present_digest;
                MergeDigestEntry(
                    present_digest,
                    MakeTextureKey(uint64(present_op->target.texture)),
                    true,
                    false,
                    std::nullopt,
                    ETextureState::TRANSFER
                );
                ResourceStateSnapshot present_snapshot{};
                EnsureDigestStateLoaded(present_snapshot, present_digest);
                ApplyDigestToState(
                    present_digest,
                    present_op->queue,
                    SubmissionKey{op_seq, 0},
                    present_snapshot
                );
                CommitPersistentResourceStates(present_snapshot);
                ApplyDigestToDirtyWrittenResources(dirty_written_resources, present_digest);
                RHITRACE_RESOURCE_LOG(
                    static_cast<VulkanTexture*>(present_op->target.texture)->GetName(),
                    "[ResourceTrace][Preprocess][Present] {} : recorded TRANSFER read, committed to persistent state (queue={})",
                    static_cast<VulkanTexture*>(present_op->target.texture)->GetName(),
                    QueueTypeName(present_op->queue)
                );
            }
        }
        ++op_seq;
    }

    return preprocess_store;
}

static Array<PlatformExecOp>
AssemblePlatformOps(Array<RHIExecOp>&& ops, const ExecutePreprocessStore& preprocess_store) {
    TRACE_SCOPE_CAT("Vulkan.AssemblePlatformOps", "RHI");
    Array<PlatformExecOp> platform_ops{};
    platform_ops.reserve(EstimatePlatformOpCount(ops) * 3 + 1);

    auto build_segment_submit =
        [](CmdSubmit& source_submit,
           const CommandSegmentInfo& descriptor,
           bool attach_waits,
           bool attach_signals_and_callbacks,
           bool attach_parent_runtime_payload) -> CmdSubmit {
        Array<UniquePtr<Command>> commands{};
        if (descriptor.type == ESegmentType::Copy) {
            UniquePtr<Command> copy_scope_holder = std::move(source_submit.cmds[descriptor.copy_scope_index]);
            auto* copy_scope = static_cast<CopyScopeCmd*>(copy_scope_holder.get());
            commands = copy_scope->StealCommands();
        } else {
            commands.reserve(descriptor.end - descriptor.begin);
            for (size_t cmd_index = descriptor.begin; cmd_index < descriptor.end; ++cmd_index) {
                commands.emplace_back(std::move(source_submit.cmds[cmd_index]));
            }
        }

        Array<std::function<void(void)>> callbacks{};
        Array<QueryToken>                query_tokens{};
        Array<GPUEvent>                  gpu_events{};
        Array<WaitEvent>                 wait_events{};
        Array<SignalEvent>               signal_events{};

        if (attach_waits) {
            wait_events = std::move(source_submit.wait_events);
        }
        if (attach_signals_and_callbacks) {
            callbacks     = std::move(source_submit.callbacks);
            signal_events = std::move(source_submit.signal_events);
        }
        if (attach_parent_runtime_payload) {
            query_tokens = source_submit.query_tokens;
            gpu_events   = source_submit.gpu_events;
        }

        CmdSubmit segment_submit{
            std::move(commands),
            std::move(callbacks),
            TCachedArgArray(source_submit.cached_args),
            std::move(query_tokens),
            std::move(gpu_events)
        };
        segment_submit.wait_events   = std::move(wait_events);
        segment_submit.signal_events = std::move(signal_events);
        if (attach_signals_and_callbacks) {
            segment_submit.b_sync             = source_submit.b_sync;
            segment_submit.b_tick_profiling   = source_submit.b_tick_profiling && descriptor.type != ESegmentType::Copy;
            segment_submit.b_delete_resources = source_submit.b_delete_resources;
        }
        return segment_submit;
    };

    auto inject_queue_transfer_commands =
        [](CmdSubmit& segment_submit, const CmdSubmitPreprocessResult& preprocess_result) {
        if ((preprocess_result.prefix_import_textures.size() > 0 ||
             preprocess_result.prefix_import_buffers.size() > 0) &&
            preprocess_result.prefix_transfer_queue.has_value()) {
            segment_submit.cmds.insert(
                segment_submit.cmds.begin(),
                MakeUnique<QueueTransferCmd>(
                    preprocess_result.prefix_transfer_queue.value(),
                    Array<ImportTexture>(preprocess_result.prefix_import_textures),
                    Array<ImportBuffer>(preprocess_result.prefix_import_buffers)
                )
            );
        }

        if ((preprocess_result.suffix_export_textures.size() > 0 ||
             preprocess_result.suffix_export_buffers.size() > 0) &&
            preprocess_result.suffix_transfer_queue.has_value()) {
            segment_submit.cmds.emplace_back(
                MakeUnique<QueueTransferCmd>(
                    preprocess_result.suffix_transfer_queue.value(),
                    Array<ExportTexture>(preprocess_result.suffix_export_textures),
                    Array<ExportBuffer>(preprocess_result.suffix_export_buffers)
                )
            );
        }
    };

    std::optional<SubmissionKey> last_submit_key{};
    EQueueType                   last_submit_queue = EQueueType::Ignore;

    uint64 op_seq = 0;
    for (auto& op : ops) {
        std::visit(
            Overload{
                [&](RHISubmitCmdList& submit_op) {
                    for (uint32 submit_idx = 0; submit_idx < submit_op.submits.size(); ++submit_idx) {
                        auto& submit = submit_op.submits[submit_idx];
                        const SourceSubmitKey source_key{op_seq, submit_idx};
                        const auto* source_plan = preprocess_store.FindSourcePlan(source_key);
                        if (source_plan == nullptr) {
                            continue;
                        }

                        const auto descriptors = SplitCommandSegment(submit);
                        std::optional<size_t> last_parent_segment_plan_index{};
                        for (size_t segment_plan_index = source_plan->segments.size();
                             segment_plan_index > 0;
                             --segment_plan_index) {
                            if (source_plan->segments[segment_plan_index - 1].kind ==
                                ESegmentType::Graphics) {
                                last_parent_segment_plan_index = segment_plan_index - 1;
                                break;
                            }
                        }

                        size_t segment_plan_cursor = 0;
                        for (const auto& descriptor : descriptors) {
                            const bool include =
                                !descriptor.IsEmpty(submit) ||
                                (descriptors.size() == 1 && descriptor.type == ESegmentType::Graphics &&
                                 HasSubmitSideEffects(submit));
                            if (!include) {
                                continue;
                            }

                            const auto& segment_plan = source_plan->segments[segment_plan_cursor++];
                            const auto* preprocess_result = preprocess_store.Find(segment_plan.key);
                            if (preprocess_result == nullptr) {
                                continue;
                            }

                            const bool attach_waits = segment_plan_cursor == 1;
                            const bool attach_signals_and_callbacks =
                                segment_plan_cursor == source_plan->segments.size();
                            const bool attach_parent_runtime_payload =
                                descriptor.type == ESegmentType::Graphics &&
                                last_parent_segment_plan_index.has_value() &&
                                (segment_plan_cursor - 1) == last_parent_segment_plan_index.value();

                            CmdSubmit segment_submit = build_segment_submit(
                                submit,
                                descriptor,
                                attach_waits,
                                attach_signals_and_callbacks,
                                attach_parent_runtime_payload
                            );
                            inject_queue_transfer_commands(segment_submit, *preprocess_result);

                            QueueSubmissionInfo submit_info{
                                segment_plan.key,
                                op_seq,
                                preprocess_result->queue,
                                std::move(segment_submit)
                            };
                            submit_info.digest                  = preprocess_result->digest;
                            submit_info.initial_state_snapshot  = preprocess_result->initial_state_snapshot;
                            submit_info.wait_submission_keys    = preprocess_result->wait_submission_keys;
                            platform_ops.emplace_back(std::move(submit_info));

                            last_submit_key   = segment_plan.key;
                            last_submit_queue = preprocess_result->queue;
                        }
                    }
                },
                [&](RHIPresentOp& present_op) {
                    PresentInfo present_info{op_seq, std::move(present_op)};
                    if (!last_submit_key.has_value() || last_submit_queue != EQueueType::Graphics) {
                        present_info.valid = false;
                        present_info.error =
                            "Present requires the last translated submission to be Graphics";
                        LOG_ERROR("{}", present_info.error);
                    } else {
                        present_info.wait_submission_keys.emplace_back(last_submit_key.value());
                    }
                    platform_ops.emplace_back(std::move(present_info));
                }
            },
            op
        );
        ++op_seq;
    }

    return platform_ops;
}

// §7.2 / §9.3: Convert ResourceStateSnapshot to TrackerSeed for VkTracker::InitFromSeed.
static TrackerSeed BuildTrackerSeed(const ResourceStateSnapshot& snapshot) {
    TrackerSeed seed;
    seed.textures.reserve(snapshot.size());
    seed.buffers.reserve(snapshot.size());
    for (const auto& [key, value] : snapshot) {
        if (key.type == ETrackedResourceType::Texture) {
            auto* texture = reinterpret_cast<VulkanTexture*>(key.handle);
            TrackerSeedTextureEntry entry{};
            entry.known         = value.known;
            entry.has_writer    = value.has_writer;
            entry.owner_queue   = value.owner_queue;
            entry.texture_state = value.texture_state;
            entry.texture       = texture;
            // Use the subresource range from the ResourceKey directly so that per-range
            // digest entries (e.g. individual mip views from GenerateMips) seed only their
            // specific mip/layer range, not the whole resource.
            entry.mip_level   = key.mip_level;
            entry.mip_count   = key.mip_count;
            entry.array_layer = key.array_layer;
            entry.array_count = key.array_count;
            seed.textures.push_back(entry);
            RHITRACE_RESOURCE_LOG(
                texture->GetName(),
                "[ResourceTrace][BuildSeed] {} : known={} state={} has_writer={} owner_queue={} mip={}/{} layer={}/{}",
                texture->GetName(),
                entry.known,
                int(entry.texture_state),
                entry.has_writer,
                int(entry.owner_queue),
                entry.mip_level,
                entry.mip_count,
                entry.array_layer,
                entry.array_count
            );
        } else if (key.type == ETrackedResourceType::Buffer) {
            auto* buffer = reinterpret_cast<VulkanBuffer*>(key.handle);
            TrackerSeedBufferEntry entry{};
            entry.known         = value.known;
            entry.has_writer    = value.has_writer;
            entry.owner_queue   = value.owner_queue;
            entry.buffer_state  = value.buffer_state;
            entry.buffer        = buffer;
            seed.buffers.push_back(entry);
        }
    }
    return seed;
}

static Array<PlatformExecOp> TranslateRHI(Array<PlatformExecOp>&& ops) {
    TRACE_SCOPE_CAT("Vulkan.TranslateRHI", "RHI");
    for (size_t op_index = 0; op_index < ops.size(); ++op_index) {
        auto* submit_info = std::get_if<QueueSubmissionInfo>(&ops[op_index]);
        if (submit_info == nullptr || !submit_info->valid) {
            continue;
        }

        const EQueueType queue_type = submit_info->queue;
        CmdSubmit        submit     = std::move(submit_info->submit);
        TrackerSeed seed = BuildTrackerSeed(submit_info->initial_state_snapshot);
        switch (queue_type) {
            case EQueueType::Graphics:
            case EQueueType::Compute: {
                auto& queue = static_cast<VkCommandQueue&>(
                    RenderDevice::Get().GetCommandQueue(queue_type)
                );
                submit_info->recorded_submit = queue.Translate(std::move(submit), nullptr, std::move(seed));
                break;
            }
            case EQueueType::Copy: {
                auto& queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
                submit_info->recorded_submit = queue.Translate(std::move(submit), nullptr);
                break;
            }
            default:
                submit_info->valid = false;
                break;
        }
    }

    return ops;
}

static bool ValidateFrameEndState(const ResourceStateSnapshot& snapshot) {
    for (const auto& [resource_key, state] : snapshot) {
        if (!state.known) {
            continue;
        }
        if (state.owner_queue == EQueueType::Graphics || state.owner_queue == EQueueType::Ignore) {
            continue;
        }

        LOG_ERROR(
            "FrameEnd validation failed: resource handle={} type={} is still owned by {}",
            resource_key.handle,
            static_cast<uint32>(resource_key.type),
            QueueTypeName(state.owner_queue)
        );
        assert(false && "FrameEnd requires resources to be returned to Graphics or Ignore");
        return false;
    }
    return true;
}

} // namespace

void VulkanSubmissionExecutor::Execute(
    Array<RHIExecOp>&&             ops,
    const RHIExecSubmitOptions& options
) {
    TRACE_SCOPE_CAT("VulkanSubmissionExecutor.Execute", "RHI");
    const uint64 trace_frame = NextRHITraceFrameIndex();
    ScopedRHITraceFrame trace_scope(trace_frame);
    RHITRACE_LOG(
        basic,
        "[RHITrace][Frame] frame={} op_count={} frame_end={}",
        trace_frame,
        ops.size(),
        options.frame_end
    );
    if (ops.empty()) {
        if (options.frame_end) {
            SubmissionPlanExecutor executor{};
            TRACE_SCOPE_CAT("Vulkan.ExecuteSubmissionPlan", "RHI");
            executor.Execute({}, true);
        }
        return;
    }

    ExecutePreprocessStore preprocess_store{};
    {
        TRACE_SCOPE_CAT("Vulkan.Preprocess", "RHI");
        preprocess_store = PreprocessFrameOps(ops);
    }
    if (options.frame_end) {
        // Collect all resource keys seen during preprocess and validate their persistent state.
        UnorderedSet<ResourceKey, ResourceKeyHash> seen_resources{};
        for (const auto& result : preprocess_store.results) {
            for (const auto& [key, _] : result.last_state_snapshot) {
                seen_resources.emplace(key);
            }
        }
        ResourceStateSnapshot frame_end_state{};
        for (const auto& key : seen_resources) {
            frame_end_state.emplace(key, LoadPersistentState(key));
        }
        // if (!ValidateFrameEndState(frame_end_state)) {
        //     return;
        // }
    }

    // All persistent state has been committed incrementally during preprocess (per segment).
    // No additional CommitPersistentResourceStates call needed here.

    Array<PlatformExecOp> platform_ops{};
    {
        TRACE_SCOPE_CAT("Vulkan.Assemble", "RHI");
        platform_ops = AssemblePlatformOps(std::move(ops), preprocess_store);
    }
    {
        TRACE_SCOPE_CAT("Vulkan.Translate", "RHI");
        platform_ops = TranslateRHI(std::move(platform_ops));
    }

    SubmissionPlanExecutor executor{};
    {
        TRACE_SCOPE_CAT("Vulkan.ExecuteSubmissionPlan", "RHI");
        executor.Execute(std::move(platform_ops), options.frame_end);
    }
}

void VulkanSubmissionExecutor::Flush() {
    FlushSubmissionPlanRuntime();
    FlushSubmissionPresentContexts();
}

void VulkanSubmissionExecutor::Shutdown() {
    ShutdownSubmissionPlanRuntime();
    ShutdownSubmissionPresentContexts();
}

} // namespace Moer::Render
