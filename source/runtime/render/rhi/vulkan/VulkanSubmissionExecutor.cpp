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

namespace Moer::Render {
namespace {

enum class ETrackedResourceType : uint8 {
    Buffer,
    Texture,
    Bindless,
    Accel
};

struct ResourceKey {
    ETrackedResourceType type{ETrackedResourceType::Buffer};
    uint64               handle{0};

    bool operator==(const ResourceKey& other) const {
        return type == other.type && handle == other.handle;
    }
};

struct ResourceKeyHash {
    size_t operator()(const ResourceKey& key) const {
        size_t hash = std::hash<uint64>{}(key.handle);
        hash ^= static_cast<size_t>(key.type) + 0x9e3779b9 + (hash << 6u) + (hash >> 2u);
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

struct ResourceStateValue {
    bool       known{false};
    bool       has_writer{false};
    EQueueType owner_queue{EQueueType::Ignore};

    EBufferState  buffer_state{EBufferState::UNDEFINED};
    ETextureState texture_state{ETextureState::UNDEFINED};
};

using ResourceStateSnapshot = UnorderedMap<ResourceKey, ResourceStateValue, ResourceKeyHash>;

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

struct CmdSubmitPreprocessResult {
    SubmissionKey key{};
    EQueueType    queue{EQueueType::Ignore};

    ResourceStateSnapshot initial_state_snapshot{};
    ResourceStateSnapshot last_state_snapshot{};
    ResourceAccessDigest  digest{};
    struct ReorderedSubmitCache {
        TCachedArgArray cached_args{};
        CmdReorderer    reorderer;

        ReorderedSubmitCache(FunctionTable funcs, const TCachedArgArray& src_args) :
            cached_args(src_args),
            reorderer(funcs, cached_args) {}
    };
    std::shared_ptr<ReorderedSubmitCache> reordered_cache{};
};

struct ExecutePreprocessStore {
    Array<CmdSubmitPreprocessResult>                    results{};
    UnorderedMap<SubmissionKey, uint32, SubmissionKeyHash> lookup{};

    void Reserve(uint32 count) {
        results.reserve(count);
        lookup.reserve(count);
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
};

struct FrameStateStore {
    // Frame-lifetime state snapshot: one Execute() call keeps all submit state propagation.
    ResourceStateSnapshot global_last_state{};
};

struct QueueSubmissionInfo {
    SubmissionKey key{};
    uint64        op_seq{0};
    EQueueType    queue{EQueueType::Ignore};
    CmdSubmit     submit;
    std::optional<VulkanRecordedSubmit> recorded_submit{};
    ResourceAccessDigest digest{};
    ResourceStateSnapshot initial_state_snapshot{};  // §7.2: seed_tracker for translate
    std::shared_ptr<CmdSubmitPreprocessResult::ReorderedSubmitCache> reordered_cache{};
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

static ResourceKey MakeTextureKey(uint64 handle) {
    return ResourceKey{ETrackedResourceType::Texture, handle};
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

static bool IsResourceInBindlessArray(uint64 resource, uint64 bindless_handle) {
    auto* bindless_array = reinterpret_cast<VulkanBindlessArray*>(bindless_handle);
    return bindless_array->IsResourceAllocated(resource);
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

class ResourceAccessCollector {
public:
    ResourceAccessCollector(EQueueType in_queue, const TCachedArgArray& in_cached_args) :
        queue(in_queue),
        cached_args(in_cached_args) {}

    ResourceAccessDigest Collect(const CmdSubmit& submit) const {
        ResourceAccessDigest digest{};
        for (const auto& cmd : submit.cmds) {
            if (!cmd) {
                continue;
            }
            VisitCommand(*cmd, digest);
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
        MergeDigestEntry(digest, MakeTextureKey(handle), true, false, std::nullopt, state);
    }

    void MarkWriteTexture(ResourceAccessDigest& digest, uint64 handle, ETextureState state) const {
        MergeDigestEntry(digest, MakeTextureKey(handle), false, true, std::nullopt, state);
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

    void PromoteBindlessContainedResources(ResourceAccessDigest& digest, uint64 bindless_handle) const {
        for (auto& [resource_key, entry] : digest) {
            if (!entry.write || !IsResourceInBindlessArray(resource_key.handle, bindless_handle)) {
                continue;
            }

            switch (resource_key.type) {
                case ETrackedResourceType::Texture:
                    entry.read = true;
                    entry.last_access_write = false;
                    if (auto state = GetBindlessReadTextureState(resource_key.handle); state.has_value()) {
                        entry.texture_state = state.value();
                    }
                    break;
                case ETrackedResourceType::Buffer:
                    entry.read = true;
                    entry.last_access_write = false;
                    if (auto state = GetBindlessReadBufferState(resource_key.handle); state.has_value()) {
                        entry.buffer_state = state.value();
                    }
                    break;
                default:
                    break;
            }
        }
    }

    void CollectBindlessReads(ResourceAccessDigest& digest, uint64 bindless_handle) const {
        auto* bindless_array = reinterpret_cast<VulkanBindlessArray*>(bindless_handle);
        if (bindless_array == nullptr) {
            return;
        }

        bindless_array->Lock();
        for (const auto& handle : bindless_array->handles) {
            const uint64 resource = handle.Ptr();
            if (resource == 0) {
                continue;
            }

            if (handle.IsTexture()) {
                auto state = GetBindlessReadTextureState(resource);
                if (state.has_value()) {
                    MarkReadTexture(digest, resource, state.value());
                }
            } else if (handle.IsBuffer()) {
                auto state = GetBindlessReadBufferState(resource);
                if (state.has_value()) {
                    MarkReadBuffer(digest, resource, state.value());
                }
            }
        }
        bindless_array->Unlock();
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

    void CollectShaderArg(const TArg& arg, ParamInfoFlags param_info, ResourceAccessDigest& digest) const {
        VulkanShaderResourceState shader_state(param_info.state_flags);
        if (const auto* bindless = std::get_if<BindlessArrayRef>(&arg)) {
            if (bindless->Get() == nullptr) {
                return;
            }
            const uint64 handle = uint64(bindless->Get());
            MarkReadBindless(digest, handle, EBufferState::SHADER_RESOURCE);
            CollectBindlessReads(digest, handle);
            PromoteBindlessContainedResources(digest, handle);
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
                        MarkReadTexture(digest, handle, texture_read_state);
                    }
                    if (write) {
                        MarkWriteTexture(digest, handle, texture_write_state);
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
                            MarkReadTexture(digest, handle, texture_read_state);
                        }
                        if (write) {
                            MarkWriteTexture(digest, handle, texture_write_state);
                        }
                    }
                } else if constexpr (std::is_same_v<T, BindlessArrayRef>) {
                    if (value.Get() == nullptr) {
                        return;
                    }
                    const uint64 handle = uint64(value.Get());
                    if (read) {
                        MarkReadBindless(digest, handle, buffer_read_state);
                        CollectBindlessReads(digest, handle);
                        PromoteBindlessContainedResources(digest, handle);
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
        ResourceAccessDigest& digest
    ) const {
        const uint32 arg_count = static_cast<uint32>(std::min(args.args.size(), pipeline.binding_infos.size()));
        for (uint32 i = 0; i < arg_count; ++i) {
            if (!IsPipelineResourceValid(pipeline, i)) {
                continue;
            }
            CollectShaderArg(args.args[i], pipeline.binding_infos[i], digest);
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

    void VisitCommand(const Command& cmd, ResourceAccessDigest& digest) const {
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
                CollectPipelineArgs(dispatch_cmd->Pipeline(), dispatch_cmd->Args(cached_args), digest);
                const auto dispatch_param = dispatch_cmd->Param();
                if (const auto* indirect = std::get_if<DispatchIndirectParam>(&dispatch_param)) {
                    MarkReadBuffer(digest, GetHandle(indirect->indirect), EBufferState::INDIRECT);
                }
                break;
            }
            case Command::EType::SetDrawState: {
                const auto* draw_cmd = static_cast<const SetDrawStateCmd*>(&cmd);
                CollectPipelineArgs(draw_cmd->Pipeline(), draw_cmd->Args(), digest);

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
                        CollectPipelineArgs(draw.handle, *args, digest);
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
                    MarkReadTexture(digest, texture.handle, texture.state);
                }
                for (const auto& texture : barrier_cmd->WriteTextures()) {
                    MarkWriteTexture(digest, texture.handle, texture.state);
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
                    MarkWriteBuffer(digest, GetHandle(clear_cmd->Buffer()), EBufferState::UNORDERED_ACCESS);
                } else if (clear_cmd->IsTexture()) {
                    MarkWriteTexture(
                        digest, GetHandle(clear_cmd->Texture()), ETextureState::UNORDERED_ACCESS
                    );
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
                        CollectShaderArg(arg, state_flags, digest);
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
                            CollectShaderArg(arg, state_flags, digest);
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
    EQueueType             queue{EQueueType::Ignore};
    const TCachedArgArray& cached_args;
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

static void ApplyDigestToState(
    const ResourceAccessDigest& digest,
    EQueueType                  queue,
    ResourceStateSnapshot&      state_snapshot
) {
    for (const auto& [resource_key, access] : digest) {
        auto& resource_state = state_snapshot[resource_key];
        resource_state.known = true;

        if (resource_key.type == ETrackedResourceType::Texture && access.texture_state.has_value()) {
            resource_state.texture_state = access.texture_state.value();
        }

        if (resource_key.type != ETrackedResourceType::Texture && access.buffer_state.has_value()) {
            resource_state.buffer_state = access.buffer_state.value();
        }

        resource_state.has_writer = access.last_access_write;
        if (access.write) {
            resource_state.owner_queue = queue;
        }
    }
}

class SubmissionDependencyResolver {
public:
    Array<PlatformExecOp> Resolve(Array<PlatformExecOp>&& ops) const {
        bool seen_present = false;

        UnorderedMap<ResourceKey, ResourceHazardState, ResourceKeyHash> hazard_states{};
        UnorderedMap<SubmissionKey, EQueueType, SubmissionKeyHash>      submission_queue{};

        for (auto& op : ops) {
            std::visit(
                Overload{
                    [&](QueueSubmissionInfo& submit_info) {
                        if (seen_present) {
                            submit_info.valid = false;
                            LOG_ERROR(
                                "RHIExecutor ordering validation failed: submit appears after present "
                                "(op_seq={}, submit_idx={})",
                                submit_info.key.op_seq,
                                submit_info.key.submit_idx
                            );
                            return;
                        }
                        submission_queue.emplace(submit_info.key, submit_info.queue);

                        if (submit_info.digest.empty()) {
                            return;
                        }

                        UnorderedSet<SubmissionKey, SubmissionKeyHash> dependency_set{};
                        for (const auto& [resource_key, access] : submit_info.digest) {
                            auto& hazard_state = hazard_states[resource_key];

                            if ((access.read || access.write) && hazard_state.last_writer.has_value()) {
                                const SubmissionKey& producer_key = hazard_state.last_writer.value();
                                const auto queue_it = submission_queue.find(producer_key);
                                if (queue_it != submission_queue.end() &&
                                    queue_it->second != submit_info.queue) {
                                    dependency_set.emplace(producer_key);
                                }
                            }

                            if (access.write) {
                                for (const auto& reader_key : hazard_state.last_readers) {
                                    const auto queue_it = submission_queue.find(reader_key);
                                    if (queue_it != submission_queue.end() &&
                                        queue_it->second != submit_info.queue) {
                                        dependency_set.emplace(reader_key);
                                    }
                                }
                            }
                        }

                        submit_info.wait_submission_keys.assign(
                            dependency_set.begin(), dependency_set.end()
                        );
                        std::sort(
                            submit_info.wait_submission_keys.begin(),
                            submit_info.wait_submission_keys.end(),
                            SubmissionKeyLess
                        );
                        RHITRACE_LOG(
                            basic,
                            "[RHITrace][Resolve] submit=({}, {}) queue={} wait_count={}",
                            submit_info.key.op_seq,
                            submit_info.key.submit_idx,
                            QueueTypeName(submit_info.queue),
                            submit_info.wait_submission_keys.size()
                        );
                        for (const auto& wait_key : submit_info.wait_submission_keys) {
                            RHITRACE_LOG(
                                basic,
                                "[RHITrace][Resolve]   wait_key=({}, {})",
                                wait_key.op_seq,
                                wait_key.submit_idx
                            );
                        }

                        for (const auto& [resource_key, access] : submit_info.digest) {
                            auto& hazard_state = hazard_states[resource_key];
                            if (access.read) {
                                const auto iter = std::find(
                                    hazard_state.last_readers.begin(),
                                    hazard_state.last_readers.end(),
                                    submit_info.key
                                );
                                if (iter == hazard_state.last_readers.end()) {
                                    hazard_state.last_readers.emplace_back(submit_info.key);
                                }
                            }
                            if (access.write) {
                                hazard_state.last_writer = submit_info.key;
                                hazard_state.last_readers.clear();
                            }
                        }
                    },
                    [&](PresentInfo& present_info) {
                        seen_present = true;

                        if (!present_info.present.target.texture) {
                            return;
                        }

                        const ResourceKey present_target_key =
                            MakeTextureKey(uint64(present_info.present.target.texture));
                        const auto hazard_it = hazard_states.find(present_target_key);
                        if (hazard_it == hazard_states.end() ||
                            !hazard_it->second.last_writer.has_value()) {
                            return;
                        }

                        const SubmissionKey& writer_key = hazard_it->second.last_writer.value();
                        const auto queue_it = submission_queue.find(writer_key);
                        if (queue_it == submission_queue.end()) {
                            return;
                        }
                        present_info.wait_submission_keys.emplace_back(writer_key);
                        RHITRACE_LOG(
                            basic,
                            "[RHITrace][Resolve] present op_seq={} wait=({}, {}) queue={}",
                            present_info.op_seq,
                            writer_key.op_seq,
                            writer_key.submit_idx,
                            QueueTypeName(present_info.present.queue)
                        );
                    }
                },
                op
            );
        }

        return ops;
    }
};

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

        // The presenter's tracker is transient and does not know that the
        // source texture was last written by COLOR_ATTACHMENT_OUTPUT.
        // Seed the src state before the read-for-transfer so the barrier has
        // the correct srcAccessMask / srcStageMask.
        tracker.SeedSrcState(
            src_texture,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
        );
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
PreprocessFrameOps(const Array<RHIExecOp>& ops, FrameStateStore& frame_state_store) {
    TRACE_SCOPE_CAT("Vulkan.PreprocessFrameOps", "RHI");
    ExecutePreprocessStore preprocess_store{};
    preprocess_store.Reserve(static_cast<uint32>(EstimateSubmitCount(ops)));

    uint64 op_seq = 0;
    for (const auto& op : ops) {
        if (const auto* submit_op = std::get_if<RHISubmitCmdList>(&op)) {
            ResourceStateSnapshot chained_state = frame_state_store.global_last_state;
            for (uint32 submit_idx = 0; submit_idx < submit_op->submits.size(); ++submit_idx) {
                const CmdSubmit& submit = submit_op->submits[submit_idx];

                ResourceAccessCollector collector(submit_op->queue, submit.cached_args);
                ResourceAccessDigest    digest = collector.Collect(submit);

                if (submit_idx + 1 == submit_op->submits.size()) {
                    for (const auto& written_texture : submit_op->write_textures) {
                        if (!written_texture.texture) {
                            continue;
                        }
                        MergeDigestEntry(
                            digest,
                            MakeTextureKey(uint64(written_texture.texture)),
                            false,
                            true,
                            std::nullopt,
                            ETextureState::RENDER_TARGET
                        );
                    }
                }

                CmdSubmitPreprocessResult preprocess_result{};
                preprocess_result.key       = SubmissionKey{op_seq, submit_idx};
                preprocess_result.queue     = submit_op->queue;
                preprocess_result.initial_state_snapshot = chained_state;
                preprocess_result.digest = std::move(digest);
                {
                    FunctionTable reorder_funcs{
                        .is_resource_write       = &IsBufferTextureWrite,
                        .is_resource_read        = &IsBufferTextureRead,
                        .is_texture_sampled      = &IsTextureSampled,
                        .is_resource_in_bindless = &IsResourceInBindlessArray,
                        .lock_bdls_array         = &LockBindlessArray,
                        .unlock_bdls_array       = &UnlockBindlessArray
                    };
                    auto reorderer =
                        std::make_shared<CmdSubmitPreprocessResult::ReorderedSubmitCache>(
                            reorder_funcs, submit.cached_args
                        );
                    for (const auto& cmd : submit.cmds) {
                        reorderer->reorderer.AcceptCmd(cmd.get());
                    }
                    preprocess_result.reordered_cache = std::move(reorderer);
                }

                ApplyDigestToState(preprocess_result.digest, submit_op->queue, chained_state);
                preprocess_result.last_state_snapshot = chained_state;

                preprocess_store.Add(std::move(preprocess_result));
            }
            frame_state_store.global_last_state = chained_state;
        }
        ++op_seq;
    }

    return preprocess_store;
}

static Array<PlatformExecOp>
AssemblePlatformOps(Array<RHIExecOp>&& ops, const ExecutePreprocessStore& preprocess_store) {
    TRACE_SCOPE_CAT("Vulkan.AssemblePlatformOps", "RHI");
    Array<PlatformExecOp> platform_ops{};
    platform_ops.reserve(EstimatePlatformOpCount(ops));

    uint64 op_seq = 0;
    for (auto& op : ops) {
        std::visit(
            Overload{
                [&](RHISubmitCmdList& submit_op) {
                    for (uint32 submit_idx = 0; submit_idx < submit_op.submits.size(); ++submit_idx) {
                        SubmissionKey key{op_seq, submit_idx};
                        const auto*   preprocess_result = preprocess_store.Find(key);

                        QueueSubmissionInfo submit_info{
                            key,
                            op_seq,
                            submit_op.queue,
                            std::move(submit_op.submits[submit_idx])
                        };
                        if (preprocess_result != nullptr) {
                            submit_info.digest = preprocess_result->digest;
                            submit_info.reordered_cache = preprocess_result->reordered_cache;
                            submit_info.initial_state_snapshot = preprocess_result->initial_state_snapshot;
                        }
                        platform_ops.emplace_back(std::move(submit_info));
                    }
                },
                [&](RHIPresentOp& present_op) {
                    platform_ops.emplace_back(PresentInfo{op_seq, std::move(present_op)});
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
            entry.mip_level     = 0;
            entry.mip_count     = texture->GetNumMips();
            entry.array_layer   = 0;
            entry.array_count   = texture->GetNumArray();
            seed.textures.push_back(entry);
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
        const CmdReorderer* reordered = nullptr;
        if (submit_info->reordered_cache != nullptr) {
            reordered = &submit_info->reordered_cache->reorderer;
        }
        TrackerSeed seed = BuildTrackerSeed(submit_info->initial_state_snapshot);
        switch (queue_type) {
            case EQueueType::Graphics:
            case EQueueType::Compute: {
                auto& queue = static_cast<VkCommandQueue&>(
                    RenderDevice::Get().GetCommandQueue(queue_type)
                );
                submit_info->recorded_submit = queue.Translate(std::move(submit), reordered, std::move(seed));
                break;
            }
            case EQueueType::Copy: {
                auto& queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
                submit_info->recorded_submit = queue.Translate(std::move(submit), reordered);
                break;
            }
            default:
                submit_info->valid = false;
                break;
        }
    }

    return ops;
}

} // namespace

void VulkanSubmissionExecutor::Execute(
    Array<RHIExecOp>&&             ops,
    const RHIExecSubmitOptions& options
) {
    TRACE_SCOPE_CAT("VulkanSubmissionExecutor.Execute", "RHI");
    if (ops.empty()) {
        if (options.frame_end) {
            SubmissionPlanExecutor executor{};
            TRACE_SCOPE_CAT("Vulkan.ExecuteSubmissionPlan", "RHI");
            executor.Execute({}, true);
        }
        return;
    }

    FrameStateStore frame_state_store{};
    ExecutePreprocessStore preprocess_store{};
    {
        TRACE_SCOPE_CAT("Vulkan.Preprocess", "RHI");
        preprocess_store = PreprocessFrameOps(ops, frame_state_store);
    }

    Array<PlatformExecOp> platform_ops{};
    {
        TRACE_SCOPE_CAT("Vulkan.Assemble", "RHI");
        platform_ops = AssemblePlatformOps(std::move(ops), preprocess_store);
    }

    SubmissionDependencyResolver dependency_resolver{};
    {
        TRACE_SCOPE_CAT("Vulkan.ResolveDependencies", "RHI");
        platform_ops = dependency_resolver.Resolve(std::move(platform_ops));
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
