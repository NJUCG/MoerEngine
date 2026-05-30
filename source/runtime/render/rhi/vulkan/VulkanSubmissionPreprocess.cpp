#include "VulkanSubmissionExecutorPrivate.h"

#include <span>
#include <type_traits>

namespace Moer::Render {

namespace {

static const TranslateFenceCmd* TryGetTranslateFenceCmd(const Command* cmd) {
    if (cmd == nullptr || cmd->Type() != Command::EType::Custom) {
        return nullptr;
    }
    const auto* custom_cmd = static_cast<const CustomCmd*>(cmd);
    if (custom_cmd->CustomId() != CustomCmd::CustomCmdId::CUSTOM_TRANSLATE_FENCE) {
        return nullptr;
    }
    return static_cast<const TranslateFenceCmd*>(custom_cmd);
}

static const FrameTickCmd* TryGetFrameTickCmd(const Command* cmd) {
    if (cmd == nullptr || cmd->Type() != Command::EType::Custom) {
        return nullptr;
    }
    const auto* custom_cmd = static_cast<const CustomCmd*>(cmd);
    if (custom_cmd->CustomId() != CustomCmd::CustomCmdId::CUSTOM_FRAME_TICK) {
        return nullptr;
    }
    return static_cast<const FrameTickCmd*>(custom_cmd);
}

static ResourceKey MakeBufferKey(uint64 handle) {
    return ResourceKey{ETrackedResourceType::Buffer, handle};
}

static ResourceKey MakeTextureKey(uint64 handle) {
    return ResourceKey{
        ETrackedResourceType::Texture, handle,
        0, kRemainingSubresource, 0, kRemainingSubresource
    };
}

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

static const char* SegmentOriginName(const RHISubmitSegment& segment) {
    return segment.UsesCopyScope() ? "CopyScope" : "Inline";
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

static StringView ResourceName(const ResourceKey& key) {
    switch (key.type) {
        case ETrackedResourceType::Buffer: {
            auto* buffer = reinterpret_cast<const Buffer*>(key.handle);
            return buffer != nullptr ? buffer->GetName() : MOER_TEXT("<null>");
        }
        case ETrackedResourceType::Texture: {
            auto* texture = reinterpret_cast<const Texture*>(key.handle);
            return texture != nullptr ? texture->GetName() : MOER_TEXT("<null>");
        }
        default:
            return MOER_TEXT("<non-resource>");
    }
}

static void TraceDigest(
    const SubmissionKey&       key,
    EQueueType                 queue,
    const RHISubmitSegment&    segment,
    const ResourceAccessDigest& digest
) {
    for (const auto& [resource_key, access] : digest) {
        RHITRACE_LOG(
            verbose,
            "[RHITrace][PreprocessDigest] submit=({}, {}) queue={} segment={} resource_type={} name={} handle=0x{:x} read={} write={} last_write={} update_tracked_state={} owner={} buffer_state={} texture_state={}",
            key.op_seq,
            key.submit_idx,
            QueueTypeName(queue),
            SegmentOriginName(segment),
            ResourceTypeName(resource_key.type),
            ResourceName(resource_key),
            resource_key.handle,
            access.read,
            access.write,
            access.last_access_write,
            access.update_tracked_state,
            access.owner_queue.has_value() ? QueueTypeName(access.owner_queue.value()) : "<segment>",
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

static uint64 GetHandle(const BufferView& view) {
    return uint64(view.GetBuffer());
}

static uint64 GetHandle(const TextureView& view) {
    return uint64(view.GetTexture());
}

static void MergeDigestEntry(
    ResourceAccessDigest&        digest,
    const ResourceKey&           key,
    bool                         read,
    bool                         write,
    std::optional<EBufferState>  buffer_state,
    std::optional<ETextureState> texture_state,
    bool                         update_tracked_state = true,
    std::optional<EQueueType>    owner_queue = std::nullopt
) {
    if (key.handle == 0) {
        return;
    }
    auto& entry = digest[key];
    entry.read |= read;
    entry.write |= write;
    entry.last_access_write = write;
    entry.update_tracked_state = update_tracked_state;
    entry.owner_queue = owner_queue;
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
        dst_entry.update_tracked_state = entry.update_tracked_state;
        dst_entry.owner_queue = entry.owner_queue;
        if (entry.buffer_state.has_value()) {
            dst_entry.buffer_state = entry.buffer_state;
        }
        if (entry.texture_state.has_value()) {
            dst_entry.texture_state = entry.texture_state;
        }
    }
}

static ResourceAccessDigest FilterExplicitImplicitBufferDigest(
    const ResourceAccessDigest&         digest,
    const Array<TrackedBufferState>&    explicit_buffers
) {
    UnorderedSet<uint64> explicit_buffer_handles{};
    explicit_buffer_handles.reserve(explicit_buffers.size());
    for (const TrackedBufferState& buffer : explicit_buffers) {
        const uint64 handle = GetHandle(buffer.buffer);
        if (handle != 0) {
            explicit_buffer_handles.emplace(handle);
        }
    }

    ResourceAccessDigest filtered{};
    for (const auto& [resource_key, access] : digest) {
        if (resource_key.type != ETrackedResourceType::Buffer) {
            continue;
        }
        if (explicit_buffer_handles.contains(resource_key.handle)) {
            continue;
        }
        filtered.emplace(resource_key, access);
    }
    return filtered;
}

static void ApplyDigestToDirtyWrittenResources(
    DirtyWrittenResources&      dirty_written_resources,
    const ResourceAccessDigest& digest
);

class ResourceAccessCollector {
public:
    ResourceAccessCollector(
        EQueueType             in_queue,
        const TCachedArgArray& in_cached_args,
        DirtyWrittenResources* in_dirty_written_resources = nullptr
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

    ResourceAccessDigest CollectGraphicsSegment(
        const CmdSubmit& submit,
        size_t           begin,
        size_t           end
    ) const {
        ResourceAccessDigest digest{};
        DirtyWrittenResources visible_dirty_written_resources{};
        if (dirty_written_resources != nullptr) {
            visible_dirty_written_resources = *dirty_written_resources;
        }
        for (size_t cmd_index = begin; cmd_index < end; ++cmd_index) {
            const Command* cmd = submit.cmds[cmd_index].get();
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

    ResourceAccessDigest CollectExplicitSegment(
        const CmdSubmit& submit,
        size_t           begin,
        size_t           end
    ) const {
        ResourceAccessDigest digest{};
        DirtyWrittenResources visible_dirty_written_resources{};
        if (dirty_written_resources != nullptr) {
            visible_dirty_written_resources = *dirty_written_resources;
        }
        for (size_t cmd_index = begin; cmd_index < end; ++cmd_index) {
            const Command* cmd = submit.cmds[cmd_index].get();
            if (cmd == nullptr) {
                continue;
            }
            switch (cmd->Type()) {
                case Command::EType::Barrier:
                case Command::EType::SetTrackedState:
                case Command::EType::QueueTransfer:
                    break;
                default:
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

    void MarkReadBuffer(
        ResourceAccessDigest& digest,
        uint64                handle,
        EBufferState          state,
        bool                  update_tracked_state = true
    ) const {
        MergeDigestEntry(digest, MakeBufferKey(handle), true, false, state, std::nullopt, update_tracked_state);
    }

    void MarkWriteBuffer(
        ResourceAccessDigest& digest,
        uint64                handle,
        EBufferState          state,
        bool                  update_tracked_state = true
    ) const {
        MergeDigestEntry(digest, MakeBufferKey(handle), false, true, state, std::nullopt, update_tracked_state);
    }

    void MarkReadTexture(
        ResourceAccessDigest& digest,
        uint64                handle,
        ETextureState         state,
        bool                  update_tracked_state = true
    ) const {
        MarkReadTextureWithRange(
            digest,
            handle,
            state,
            0,
            kRemainingSubresource,
            0,
            kRemainingSubresource,
            update_tracked_state
        );
    }

    void MarkWriteTexture(
        ResourceAccessDigest& digest,
        uint64                handle,
        ETextureState         state,
        bool                  update_tracked_state = true
    ) const {
        MarkWriteTextureWithRange(
            digest,
            handle,
            state,
            0,
            kRemainingSubresource,
            0,
            kRemainingSubresource,
            update_tracked_state
        );
    }

    void MarkReadTextureWithRange(
        ResourceAccessDigest& digest,
        uint64                handle,
        ETextureState         state,
        uint8                 mip_level,
        uint8                 mip_count,
        uint8                 array_layer,
        uint8                 array_count,
        bool                  update_tracked_state = true
    ) const {
        ForEachTextureSubresourceKey(
            MakeTextureKeyWithRange(handle, mip_level, mip_count, array_layer, array_count),
            [&](const ResourceKey& subresource_key) {
                MergeDigestEntry(
                    digest,
                    subresource_key,
                    true,
                    false,
                    std::nullopt,
                    state,
                    update_tracked_state
                );
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
        uint8                 array_count,
        bool                  update_tracked_state = true
    ) const {
        ForEachTextureSubresourceKey(
            MakeTextureKeyWithRange(handle, mip_level, mip_count, array_layer, array_count),
            [&](const ResourceKey& subresource_key) {
                MergeDigestEntry(
                    digest,
                    subresource_key,
                    false,
                    true,
                    std::nullopt,
                    state,
                    update_tracked_state
                );
            }
        );
    }

    void MarkTrackedBufferState(
        ResourceAccessDigest&      digest,
        const TrackedBufferState&  state
    ) const {
        const ResourceKey key = MakeBufferKey(GetHandle(state.buffer));
        if (key.handle == 0) {
            return;
        }
        auto& entry = digest[key];
        entry.last_access_write = state.access_write;
        entry.update_tracked_state = true;
        entry.owner_queue = state.owner_queue;
        entry.buffer_state = state.state;
    }

    void MarkTrackedTextureState(
        ResourceAccessDigest&       digest,
        const TrackedTextureState&  state
    ) const {
        const ResourceKey key = MakeTextureKeyWithRange(
            GetHandle(state.texture),
            state.texture.mip_level,
            state.texture.num_mips,
            state.texture.array_layer,
            state.texture.num_array
        );
        if (key.handle == 0) {
            return;
        }
        ForEachTextureSubresourceKey(
            key,
            [&](const ResourceKey& subresource_key) {
                auto& entry = digest[subresource_key];
                entry.last_access_write = state.access_write;
                entry.update_tracked_state = true;
                entry.owner_queue = state.owner_queue;
                entry.texture_state = state.state;
            }
        );
    }

    void MarkReadBindless(
        ResourceAccessDigest& digest,
        uint64                handle,
        EBufferState          state,
        bool                  update_tracked_state = true
    ) const {
        MergeDigestEntry(digest, MakeBindlessKey(handle), true, false, state, std::nullopt);
        auto* bindless_array = reinterpret_cast<VulkanBindlessArray*>(handle);
        if (bindless_array == nullptr) {
            return;
        }
        MarkReadBuffer(digest, uint64(bindless_array->bindless_array_buffer), state, update_tracked_state);
        MarkReadBuffer(digest, uint64(bindless_array->bindless_texture_descs), state, update_tracked_state);
        MarkReadBuffer(digest, uint64(bindless_array->bindless_buffer_descs), state, update_tracked_state);
    }

    void MarkWriteBindless(
        ResourceAccessDigest& digest,
        uint64                handle,
        EBufferState          state,
        bool                  update_tracked_state = true
    ) const {
        MergeDigestEntry(digest, MakeBindlessKey(handle), false, true, state, std::nullopt);
        auto* bindless_array = reinterpret_cast<VulkanBindlessArray*>(handle);
        if (bindless_array == nullptr) {
            return;
        }
        MarkWriteBuffer(digest, uint64(bindless_array->bindless_array_buffer), state, update_tracked_state);
        MarkWriteBuffer(digest, uint64(bindless_array->bindless_texture_descs), state, update_tracked_state);
        MarkWriteBuffer(digest, uint64(bindless_array->bindless_buffer_descs), state, update_tracked_state);
    }

    void MarkReadAccel(
        ResourceAccessDigest& digest,
        uint64                handle,
        EBufferState          state,
        bool                  update_tracked_state = true
    ) const {
        MergeDigestEntry(digest, MakeAccelKey(handle), true, false, state, std::nullopt);
        if (handle == 0 || handle == kGlobalAccelBuildSyncHandle) {
            return;
        }
        auto* resource = reinterpret_cast<RHIResource*>(handle);
        if (resource == nullptr) {
            return;
        }
        switch (resource->GetResourceType()) {
            case RRT_RAYTRACING_TLAS: {
                auto* tlas = reinterpret_cast<VulkanAccelerationStructure*>(handle);
                MarkReadBuffer(digest, uint64(tlas->underlying_buffer.Get()), state, update_tracked_state);
                break;
            }
            case RRT_RAYTRACING_GEOMETRY: {
                auto* geometry = reinterpret_cast<VulkanRaytracingGeometry*>(handle);
                MarkReadBuffer(digest, uint64(geometry->GetUnderlyingBuffer()), state, update_tracked_state);
                break;
            }
            default:
                break;
        }
    }

    void MarkWriteAccel(
        ResourceAccessDigest& digest,
        uint64                handle,
        EBufferState          state,
        bool                  update_tracked_state = true
    ) const {
        MergeDigestEntry(digest, MakeAccelKey(handle), false, true, state, std::nullopt);
        if (handle == 0 || handle == kGlobalAccelBuildSyncHandle) {
            return;
        }
        auto* resource = reinterpret_cast<RHIResource*>(handle);
        if (resource == nullptr) {
            return;
        }
        switch (resource->GetResourceType()) {
            case RRT_RAYTRACING_TLAS: {
                auto* tlas = reinterpret_cast<VulkanAccelerationStructure*>(handle);
                MarkWriteBuffer(digest, uint64(tlas->underlying_buffer.Get()), state, update_tracked_state);
                break;
            }
            case RRT_RAYTRACING_GEOMETRY: {
                auto* geometry = reinterpret_cast<VulkanRaytracingGeometry*>(handle);
                MarkWriteBuffer(digest, uint64(geometry->GetUnderlyingBuffer()), state, update_tracked_state);
                break;
            }
            default:
                break;
        }
    }

    static std::optional<ETextureState> GetBindlessReadTextureState(uint64 handle) {
        auto* texture = reinterpret_cast<Texture*>(handle);
        if (texture == nullptr) {
            return std::nullopt;
        }
        const auto usage = texture->GetUsage();
        if ((usage & ETextureUsageFlags::SAMPLED) == ETextureUsageFlags::SAMPLED) {
            return ETextureState::SAMPLED;
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
        ResourceAccessDigest&  digest,
        uint64                 bindless_handle,
        DirtyWrittenResources& visible_dirty_written_resources
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
                MarkReadTextureWithRange(
                    digest,
                    depth_handle,
                    ETextureState::DEPTH_STENCIL_READ,
                    static_cast<uint8>(pass_info.depth_attachment.mip_level),
                    1,
                    static_cast<uint8>(pass_info.depth_attachment.array_layer),
                    1
                );
            }
            if (IsStoreAction(depth_action)) {
                MarkWriteTextureWithRange(
                    digest,
                    depth_handle,
                    ETextureState::DEPTH_STENCIL_WRITE,
                    static_cast<uint8>(pass_info.depth_attachment.mip_level),
                    1,
                    static_cast<uint8>(pass_info.depth_attachment.array_layer),
                    1
                );
            }
        }

        for (const auto& color_attachment : pass_info.color_attachments) {
            const auto color_handle = uint64(color_attachment.target);
            if (IsLoadAction(color_attachment.action)) {
                MarkReadTextureWithRange(
                    digest,
                    color_handle,
                    ETextureState::RENDER_TARGET,
                    static_cast<uint8>(color_attachment.mip_level),
                    1,
                    static_cast<uint8>(color_attachment.array_layer),
                    1
                );
            }
            if (IsStoreAction(color_attachment.action)) {
                MarkWriteTextureWithRange(
                    digest,
                    color_handle,
                    ETextureState::RENDER_TARGET,
                    static_cast<uint8>(color_attachment.mip_level),
                    1,
                    static_cast<uint8>(color_attachment.array_layer),
                    1
                );
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
        const bool read = shader_state.resource_type == SRT_CBV || shader_state.resource_type == SRT_SRV ||
                          shader_state.resource_type == SRT_UAV;
        if (!read && !write) {
            return;
        }

        const EBufferState  buffer_read_state = EBufferState::SHADER_RESOURCE;
        const EBufferState  buffer_write_state = EBufferState::UNORDERED_ACCESS;
        const ETextureState texture_read_state = shader_state.b_sampled ? ETextureState::SAMPLED :
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
        const PipelineHandle&  pipeline,
        const ArrayArguments&  args,
        ResourceAccessDigest&  digest,
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
                MarkWriteBuffer(digest, upload_cmd->Handle(), EBufferState::TRANSFER_DST);
                break;
            }
            case Command::EType::UploadTexture: {
                const auto* upload_cmd = static_cast<const UploadTextureCmd*>(&cmd);
                MarkWriteTexture(digest, upload_cmd->Handle(), ETextureState::TRANSFER_DST);
                break;
            }
            case Command::EType::BufferToBuffer: {
                const auto* copy_cmd = static_cast<const CopyBufferCmd*>(&cmd);
                MarkReadBuffer(digest, copy_cmd->SrcHandle(), EBufferState::TRANSFER_SRC);
                MarkWriteBuffer(digest, copy_cmd->DstHandle(), EBufferState::TRANSFER_DST);
                break;
            }
            case Command::EType::BufferToTexture: {
                const auto* copy_cmd = static_cast<const CopyBufferToTextureCmd*>(&cmd);
                MarkReadBuffer(digest, copy_cmd->SrcHandle(), EBufferState::TRANSFER_SRC);
                MarkWriteTexture(digest, copy_cmd->DstHandle(), ETextureState::TRANSFER_DST);
                break;
            }
            case Command::EType::TextureToBuffer: {
                const auto* copy_cmd = static_cast<const CopyTextureToBufferCmd*>(&cmd);
                MarkReadTexture(digest, copy_cmd->SrcHandle(), ETextureState::TRANSFER_SRC);
                MarkWriteBuffer(digest, copy_cmd->DstHandle(), EBufferState::TRANSFER_DST);
                break;
            }
            case Command::EType::TextureToTexture: {
                const auto* copy_cmd = static_cast<const CopyTextureCmd*>(&cmd);
                MarkReadTexture(digest, copy_cmd->SrcHandle(), ETextureState::TRANSFER_SRC);
                MarkWriteTexture(digest, copy_cmd->DstHandle(), ETextureState::TRANSFER_DST);
                break;
            }
            case Command::EType::CopyBackBuffer: {
                break;
            }
            case Command::EType::CopyBackTexture: {
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
                    MarkReadBuffer(digest, GetHandle(indirect->indirect), EBufferState::INDIRECT_ARGUMENT);
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
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::VERTEX_BUFFER);
                }
                for (const auto& [buffer, range] : draw_cmd->IndexBuffers()) {
                    (void)range;
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::INDEX_BUFFER);
                }
                for (const auto& [buffer, range] : draw_cmd->IndirectBuffers()) {
                    (void)range;
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::INDIRECT_ARGUMENT);
                }
                for (const auto& [buffer, range] : draw_cmd->DrawCountBuffers()) {
                    (void)range;
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::INDIRECT_ARGUMENT);
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
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::VERTEX_BUFFER);
                }
                for (const auto& [buffer, range] : draw_cmd->IndexBuffers()) {
                    (void)range;
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::INDEX_BUFFER);
                }
                for (const auto& [buffer, range] : draw_cmd->IndirectBuffers()) {
                    (void)range;
                    MarkReadBuffer(digest, uint64(buffer), EBufferState::INDIRECT_ARGUMENT);
                }
                CollectRenderPassInfo(draw_cmd->RenderPassInfo(), digest);
                break;
            }
            case Command::EType::Barrier: {
                const auto* barrier_cmd = static_cast<const BarrierCmd*>(&cmd);
                const bool update_tracked_state = barrier_cmd->ShouldUpdateTrackedState();
                for (const auto& buffer : barrier_cmd->Buffers()) {
                    assert(buffer.tracked_state.has_value() || !update_tracked_state);
                    const EBufferState tracked_state = buffer.tracked_state.value_or(EBufferState::UNDEFINED);
                    if (buffer.access_write) {
                        MarkWriteBuffer(digest, buffer.handle, tracked_state, update_tracked_state);
                    } else {
                        MarkReadBuffer(digest, buffer.handle, tracked_state, update_tracked_state);
                    }
                }
                for (const auto& bindless : barrier_cmd->ReadBindlessArrays()) {
                    MarkReadBindless(digest, bindless.handle, bindless.state, update_tracked_state);
                }
                for (const auto& bindless : barrier_cmd->WriteBindlessArrays()) {
                    MarkWriteBindless(digest, bindless.handle, bindless.state, update_tracked_state);
                }
                for (const auto& accel : barrier_cmd->ReadAccelerationStructures()) {
                    MarkReadAccel(digest, accel.handle, accel.state, update_tracked_state);
                }
                for (const auto& accel : barrier_cmd->WriteAccelerationStructures()) {
                    MarkWriteAccel(digest, accel.handle, accel.state, update_tracked_state);
                }
                for (const auto& texture : barrier_cmd->Textures()) {
                    assert(texture.tracked_state.has_value() || !update_tracked_state);
                    const ETextureState tracked_state = texture.tracked_state.value_or(ETextureState::UNDEFINED);
                    if (texture.access_write) {
                        MarkWriteTextureWithRange(
                            digest,
                            texture.handle,
                            tracked_state,
                            static_cast<uint8>(texture.mip_level),
                            static_cast<uint8>(texture.mip_cnt),
                            static_cast<uint8>(texture.array_layer),
                            static_cast<uint8>(texture.array_count),
                            update_tracked_state
                        );
                    } else {
                        MarkReadTextureWithRange(
                            digest,
                            texture.handle,
                            tracked_state,
                            static_cast<uint8>(texture.mip_level),
                            static_cast<uint8>(texture.mip_cnt),
                            static_cast<uint8>(texture.array_layer),
                            static_cast<uint8>(texture.array_count),
                            update_tracked_state
                        );
                    }
                }
                break;
            }
            case Command::EType::SetTrackedState: {
                const auto* tracked_state_cmd = static_cast<const SetTrackedStateCmd*>(&cmd);
                for (const TrackedTextureState& texture : tracked_state_cmd->Textures()) {
                    MarkTrackedTextureState(digest, texture);
                }
                for (const TrackedBufferState& buffer : tracked_state_cmd->Buffers()) {
                    MarkTrackedBufferState(digest, buffer);
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
                    MarkWriteBuffer(digest, GetHandle(clear_cmd->Buffer()), EBufferState::TRANSFER_DST);
                } else if (clear_cmd->IsTexture()) {
                    MarkWriteTexture(digest, GetHandle(clear_cmd->Texture()), ETextureState::TRANSFER_DST);
                }
                break;
            }
            case Command::EType::BuildAccel: {
                const auto* build_cmd = static_cast<const BuildAccelerationStructuresCmd*>(&cmd);
                for (const auto& param : build_cmd->Params()) {
                    if (param.geometry.Get() != nullptr) {
                        MarkWriteAccel(
                            digest, uint64(param.geometry.Get()), EBufferState::ACCELERATION_STRUCTURE_WRITE
                        );
                    }
                }
                MarkWriteAccel(digest, kGlobalAccelBuildSyncHandle, EBufferState::ACCELERATION_STRUCTURE_WRITE);
                for (auto* vtx : build_cmd->VtxBuffers()) {
                    MarkReadBuffer(digest, uint64(vtx), EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT);
                }
                for (auto* idx : build_cmd->IdxBuffers()) {
                    MarkReadBuffer(digest, uint64(idx), EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT);
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
                MarkReadAccel(digest, update_cmd->SceneHandle(), EBufferState::ACCELERATION_STRUCTURE_READ);
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
                MarkWriteAccel(digest, update_cmd->TlasHandle(), EBufferState::ACCELERATION_STRUCTURE_WRITE);
                if (update_cmd->RelatedGeometries().empty()) {
                    MarkReadAccel(digest, kGlobalAccelBuildSyncHandle, EBufferState::ACCELERATION_STRUCTURE_READ);
                } else {
                    for (const auto& [handle, count] : update_cmd->RelatedGeometries()) {
                        (void)count;
                        MarkReadAccel(digest, handle, EBufferState::ACCELERATION_STRUCTURE_READ);
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
                    MarkReadBuffer(digest, GetHandle(*indirect), EBufferState::INDIRECT_ARGUMENT);
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
            case Command::EType::BufferOverlap:
                break;
            default:
                break;
        }
    }

private:
    EQueueType             queue{EQueueType::Ignore};
    const TCachedArgArray& cached_args;
    DirtyWrittenResources* dirty_written_resources{nullptr};
};

static Array<const Command*> GetSegmentCommandPointers(
    const CmdSubmit&        submit,
    const RHISubmitSegment& segment_info
) {
    Array<const Command*> commands{};
    if (!segment_info.UsesCopyScope()) {
        return commands;
    }
    const auto* copy_scope = static_cast<const CopyScopeCmd*>(submit.cmds[segment_info.copy_scope_index].get());
    commands.reserve(copy_scope->Commands().size());
    for (const auto& cmd : copy_scope->Commands()) {
        commands.emplace_back(cmd.get());
    }
    return commands;
}

static Array<GraphEventRef> CollectSegmentFenceEvents(
    const CmdSubmit&        submit,
    const RHISubmitSegment& segment
) {
    Array<GraphEventRef> fence_events{};
    auto append_fence_event = [&](const Command* cmd) {
        if (const auto* fence_cmd = TryGetTranslateFenceCmd(cmd); fence_cmd != nullptr && fence_cmd->Fence().event) {
            fence_events.emplace_back(fence_cmd->Fence().event);
        }
    };

    if (segment.UsesCopyScope()) {
        const auto* copy_scope = static_cast<const CopyScopeCmd*>(submit.cmds[segment.copy_scope_index].get());
        for (const auto& cmd : copy_scope->Commands()) {
            append_fence_event(cmd.get());
        }
        return fence_events;
    }

    for (size_t cmd_index = segment.begin; cmd_index < segment.end; ++cmd_index) {
        append_fence_event(submit.cmds[cmd_index].get());
    }
    return fence_events;
}

static bool SegmentContainsFrameTick(const CmdSubmit& submit, const RHISubmitSegment& segment) {
    if (segment.UsesCopyScope()) {
        return false;
    }
    for (size_t cmd_index = segment.begin; cmd_index < segment.end; ++cmd_index) {
        if (TryGetFrameTickCmd(submit.cmds[cmd_index].get()) != nullptr) {
            return true;
        }
    }
    return false;
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
    DirtyWrittenResources&      dirty_written_resources,
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

static void AddLogicalDependency(
    LogicalDependencyGraph&                         dependency_graph,
    const SubmissionKey&                            consumer_key,
    UnorderedSet<SubmissionKey, SubmissionKeyHash>& local_dedup,
    const SubmissionKey&                            producer_key
) {
    if (local_dedup.emplace(producer_key).second) {
        dependency_graph.AddEdge(consumer_key, producer_key);
    }
}

static bool AppendPrefixImport(
    TranslateInfo&                               result,
    UnorderedSet<ResourceKey, ResourceKeyHash>&  imported_resources,
    const ResourceKey&                           resource_key,
    const ResourceStateValue&                    current_state,
    const ResourceAccessDigestEntry&             desired_access,
    EQueueType                                   src_queue
) {
    if (!imported_resources.emplace(resource_key).second) {
        return false;
    }

    if (result.prefix_transfer_queue.has_value() &&
        result.prefix_transfer_queue.value() != src_queue) {
        LOG_ERROR(
            MOER_TEXT("Segment ({}, {}) needs imports from multiple source queues ({} and {})"),
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
    TranslateInfo&                               result,
    UnorderedSet<ResourceKey, ResourceKeyHash>&  exported_resources,
    const ResourceKey&                           resource_key,
    const ResourceStateValue&                    current_state,
    EQueueType                                   dst_queue
) {
    if (!exported_resources.emplace(resource_key).second) {
        return false;
    }

    if (result.suffix_transfer_queue.has_value() &&
        result.suffix_transfer_queue.value() != dst_queue) {
        LOG_ERROR(
            MOER_TEXT("Segment ({}, {}) needs exports to multiple destination queues ({} and {})"),
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
        if (!access.update_tracked_state) {
            continue;
        }

        auto apply_to_state = [&](const ResourceKey& canonical_key) {
            auto& resource_state = state_snapshot[canonical_key];
            const bool was_known = resource_state.known;
            const bool materialize_unknown_state = resource_state.known || access.write ||
                                                    access.buffer_state.has_value() ||
                                                    access.texture_state.has_value();
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

            if (materialize_unknown_state) {
                const EQueueType owner_queue = access.owner_queue.value_or(queue);
                resource_state.owner_queue = owner_queue;
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

static void EnsureExplicitTrackedStateLoaded(
    ResourceStateSnapshot&            snapshot,
    const Array<TrackedTextureState>& textures,
    const Array<TrackedBufferState>&  buffers
) {
    for (const TrackedTextureState& texture : textures) {
        const ResourceKey key = MakeTextureKeyWithRange(
            GetHandle(texture.texture),
            texture.texture.mip_level,
            texture.texture.num_mips,
            texture.texture.array_layer,
            texture.texture.num_array
        );
        ForEachTextureSubresourceKey(
            key,
            [&](const ResourceKey& subresource_key) {
                if (!snapshot.contains(subresource_key)) {
                    snapshot.emplace(subresource_key, LoadPersistentState(subresource_key));
                }
            }
        );
    }

    for (const TrackedBufferState& buffer : buffers) {
        const ResourceKey key = MakeBufferKey(GetHandle(buffer.buffer));
        if (key.handle != 0 && !snapshot.contains(key)) {
            snapshot.emplace(key, LoadPersistentState(key));
        }
    }
}

static void ApplyExplicitTrackedState(
    const Array<TrackedTextureState>& textures,
    const Array<TrackedBufferState>&  buffers,
    const SubmissionKey&              submit_key,
    ResourceStateSnapshot&            state_snapshot,
    DirtyWrittenResources&            dirty_written_resources
) {
    for (const TrackedTextureState& texture : textures) {
        const ResourceKey key = MakeTextureKeyWithRange(
            GetHandle(texture.texture),
            texture.texture.mip_level,
            texture.texture.num_mips,
            texture.texture.array_layer,
            texture.texture.num_array
        );
        ForEachTextureSubresourceKey(
            key,
            [&](const ResourceKey& subresource_key) {
                ResourceStateValue& state = state_snapshot[subresource_key];
                state.known         = true;
                state.has_writer    = texture.access_write;
                state.owner_queue   = texture.owner_queue;
                state.texture_state = texture.state;
                state.last_submission = submit_key;
                if (texture.access_write) {
                    dirty_written_resources.emplace(subresource_key);
                } else {
                    dirty_written_resources.erase(subresource_key);
                }
            }
        );
    }

    for (const TrackedBufferState& buffer : buffers) {
        const ResourceKey key = MakeBufferKey(GetHandle(buffer.buffer));
        if (key.handle == 0) {
            continue;
        }
        ResourceStateValue& state = state_snapshot[key];
        state.known          = true;
        state.has_writer     = buffer.access_write;
        state.owner_queue    = buffer.owner_queue;
        state.buffer_state   = buffer.state;
        state.last_submission = submit_key;
        if (buffer.access_write) {
            dirty_written_resources.emplace(key);
        } else {
            dirty_written_resources.erase(key);
        }
    }
}

static PreprocessTranslateStore PreprocessFrameOps(
    const Array<ExecutorOp>& ops,
    uint64                   op_seq_base,
    PreprocessDependencyState& dependency_state
) {
    TRACE_SCOPE_CAT("Vulkan.PreprocessFrameOps", "RHI");
    PreprocessTranslateStore preprocess_store{};
    preprocess_store.Reserve(static_cast<uint32>(EstimateSubmitCount(ops) * 3 + 1));
    DirtyWrittenResources dirty_written_resources{};

    UnorderedMap<SubmissionKey, UnorderedSet<ResourceKey, ResourceKeyHash>, SubmissionKeyHash>
        exported_resources_by_submit{};

    uint64 op_seq = op_seq_base;
    for (const auto& op : ops) {
        if (const auto* submit_op = std::get_if<ExecutorSubmitOp>(&op)) {
            uint32 next_segment_submit_idx = 0;
            for (uint32 submit_idx = 0; submit_idx < submit_op->submits.size(); ++submit_idx) {
                const CmdSubmit& submit = submit_op->submits[submit_idx];
                const SourceSubmitKey source_key{op_seq, submit_idx};
                const auto& segments = submit.segments;
                const bool has_side_effects = HasSubmitSideEffects(submit);

                SourceSubmitPlan source_plan{};
                source_plan.source_key   = source_key;
                source_plan.parent_queue = submit_op->queue;

                if (submit.preprocess_mode == ERHISubmitPreprocessMode::ExplicitStateNoSync) {
                    std::optional<SubmissionKey> last_explicit_segment_key{};
                    for (size_t segment_index = 0; segment_index < segments.size(); ++segment_index) {
                        const RHISubmitSegment& segment = segments[segment_index];
                        const bool include = !segment.IsEmpty() || (segments.size() == 1 && has_side_effects);
                        if (!include) {
                            continue;
                        }
                        MOER_ASSERT(
                            !segment.UsesCopyScope(),
                            "Explicit RenderGraph submits must not contain CopyScope segments"
                        );

                        TranslateInfo translate_info{};
                        translate_info.key = SubmissionKey{op_seq, next_segment_submit_idx++};
                        translate_info.source_key = source_key;
                        translate_info.queue = segment.queue;
                        translate_info.segment = segment;
                        translate_info.include = true;
                        translate_info.completion_event = GraphEvent::CreateGraphEvent();

                        AppendUniqueDependency(translate_info.task_dependencies, submit.record_complete_event);
                        AppendUniqueDependency(translate_info.task_dependencies, dependency_state.last_fence_event);
                        if (submit.translate_execution_class != ERHITranslateExecutionClass::Parallel) {
                            AppendUniqueDependency(
                                translate_info.task_dependencies,
                                dependency_state.last_translate_event
                            );
                        }

                        ResourceStateSnapshot segment_state{};
                        EnsureExplicitTrackedStateLoaded(
                            segment_state,
                            submit.explicit_tracked_textures,
                            submit.explicit_tracked_buffers
                        );

                        ResourceAccessCollector collector(
                            translate_info.queue,
                            submit.cached_args,
                            &dirty_written_resources
                        );
                        translate_info.digest = FilterExplicitImplicitBufferDigest(
                            collector.CollectExplicitSegment(
                                submit,
                                segment.begin,
                                segment.end
                            ),
                            submit.explicit_tracked_buffers
                        );
                        EnsureDigestStateLoaded(segment_state, translate_info.digest);
                        translate_info.initial_state_snapshot = segment_state;

                        ApplyExplicitTrackedState(
                            submit.explicit_tracked_textures,
                            submit.explicit_tracked_buffers,
                            translate_info.key,
                            segment_state,
                            dirty_written_resources
                        );
                        ApplyDigestToState(
                            translate_info.digest,
                            translate_info.queue,
                            translate_info.key,
                            segment_state
                        );
                        translate_info.last_state_snapshot = segment_state;
                        CommitPersistentResourceStates(translate_info.last_state_snapshot);
                        ApplyDigestToDirtyWrittenResources(
                            dirty_written_resources,
                            translate_info.digest
                        );

                        TraceDigest(
                            translate_info.key,
                            translate_info.queue,
                            translate_info.segment,
                            translate_info.digest
                        );

                        RHITRACE_LOG(
                            basic,
                            "[RHITrace][ExplicitSegment] submit=({}, {}) source=({}, {}) queue={} segment={} tracked_tex={} tracked_buf={} digest_count={} explicit_waits={}",
                            translate_info.key.op_seq,
                            translate_info.key.submit_idx,
                            translate_info.source_key.op_seq,
                            translate_info.source_key.submit_idx,
                            QueueTypeName(translate_info.queue),
                            SegmentOriginName(translate_info.segment),
                            submit.explicit_tracked_textures.size(),
                            submit.explicit_tracked_buffers.size(),
                            translate_info.digest.size(),
                            0
                        );

                        source_plan.segments.emplace_back(SourceSubmitSegmentPlan{
                            .key = translate_info.key,
                            .queue = translate_info.queue,
                            .inherit_source_wait_events = false,
                            .inherit_source_signal_events_and_callbacks = false,
                            .inherit_source_runtime_payload = false,
                            .include = true
                        });
                        dependency_state.last_translate_event = ChainGraphEvents(
                            dependency_state.last_translate_event,
                            translate_info.completion_event
                        );
                        for (const GraphEventRef& fence_event : CollectSegmentFenceEvents(submit, segment)) {
                            dependency_state.last_fence_event = ChainGraphEvents(
                                dependency_state.last_fence_event,
                                fence_event
                            );
                        }
                        if (SegmentContainsFrameTick(submit, segment)) {
                            if (segment_index + 1 != segments.size()) {
                                LOG_ERROR(MOER_TEXT("FrameTick must terminate the recorded CommandList segment sequence"));
                                assert(false && "FrameTick must terminate the segment sequence");
                            }
                            dependency_state = {};
                        }
                        last_explicit_segment_key = translate_info.key;
                        preprocess_store.Add(std::move(translate_info));
                    }

                    if (!source_plan.segments.empty()) {
                        source_plan.segments.front().inherit_source_wait_events = true;
                        source_plan.segments.back().inherit_source_signal_events_and_callbacks = true;
                        for (auto segment_iter = source_plan.segments.rbegin();
                             segment_iter != source_plan.segments.rend();
                             ++segment_iter) {
                            if (segment_iter->queue != EQueueType::Copy) {
                                segment_iter->inherit_source_runtime_payload = true;
                                break;
                            }
                        }
                    }
                    preprocess_store.AddSourcePlan(std::move(source_plan));
                    continue;
                }

                std::optional<SubmissionKey> previous_gpu_segment_key{};
                EQueueType previous_gpu_segment_queue = EQueueType::Ignore;
                GraphEventRef source_last_translate_event{nullptr};

                for (size_t segment_index = 0; segment_index < segments.size(); ++segment_index) {
                    const auto& segment = segments[segment_index];
                    const bool include =
                        !segment.IsEmpty() || (segments.size() == 1 && has_side_effects);

                    if (!include) {
                        continue;
                    }

                    TranslateInfo translate_info{};
                    translate_info.key = SubmissionKey{op_seq, next_segment_submit_idx++};
                    translate_info.source_key = source_key;
                    translate_info.queue = segment.queue;
                    translate_info.segment = segment;
                    translate_info.include = true;
                    translate_info.completion_event = GraphEvent::CreateGraphEvent();

                    AppendUniqueDependency(
                        translate_info.task_dependencies,
                        submit.record_complete_event
                    );
                    AppendUniqueDependency(
                        translate_info.task_dependencies,
                        dependency_state.last_fence_event
                    );
                    if (submit.translate_execution_class != ERHITranslateExecutionClass::Parallel) {
                        AppendUniqueDependency(
                            translate_info.task_dependencies,
                            dependency_state.last_translate_event
                        );
                    }

                    ResourceAccessCollector collector(
                        translate_info.queue,
                        submit.cached_args,
                        &dirty_written_resources
                    );
                    if (!segment.UsesCopyScope()) {
                        translate_info.digest = collector.CollectGraphicsSegment(
                            submit,
                            segment.begin,
                            segment.end
                        );
                    } else {
                        Array<const Command*> command_ptrs = GetSegmentCommandPointers(submit, segment);
                        translate_info.digest = collector.Collect(
                            std::span<const Command* const>(command_ptrs.data(), command_ptrs.size())
                        );
                    }
                    TraceDigest(
                        translate_info.key,
                        translate_info.queue,
                        translate_info.segment,
                        translate_info.digest
                    );

                    ResourceStateSnapshot segment_state{};
                    EnsureDigestStateLoaded(segment_state, translate_info.digest);
                    translate_info.initial_state_snapshot = segment_state;

                    if (previous_gpu_segment_key.has_value() &&
                        previous_gpu_segment_queue != translate_info.queue) {
                        UnorderedSet<SubmissionKey, SubmissionKeyHash> local_waits{};
                        AddLogicalDependency(
                            preprocess_store.dependency_graph,
                            translate_info.key,
                            local_waits,
                            previous_gpu_segment_key.value()
                        );
                        RHITRACE_LOG(
                            verbose,
                            "[RHITrace][PreprocessWait] submit=({}, {}) queue={} depends_on=({}, {}) reason=segment_queue_change from={}",
                            translate_info.key.op_seq,
                            translate_info.key.submit_idx,
                            QueueTypeName(translate_info.queue),
                            previous_gpu_segment_key->op_seq,
                            previous_gpu_segment_key->submit_idx,
                            QueueTypeName(previous_gpu_segment_queue)
                        );
                    }

                    UnorderedSet<ResourceKey, ResourceKeyHash> imported_resources{};
                    UnorderedSet<SubmissionKey, SubmissionKeyHash> dependency_keys{};
                    for (const auto& [resource_key, access] : translate_info.digest) {
                        if (!access.read && !access.write) {
                            continue;
                        }

                        auto process_cross_queue_resource =
                            [&](const ResourceKey& canonical_key) {
                                const auto state_it =
                                    translate_info.initial_state_snapshot.find(canonical_key);
                                if (state_it == translate_info.initial_state_snapshot.end()) {
                                    return;
                                }

                                const auto& resource_state = state_it->second;
                                if (!resource_state.known ||
                                    resource_state.owner_queue == EQueueType::Ignore ||
                                    resource_state.owner_queue == translate_info.queue ||
                                    !resource_state.last_submission.has_value()) {
                                    return;
                                }

                                AddLogicalDependency(
                                    preprocess_store.dependency_graph,
                                    translate_info.key,
                                    dependency_keys,
                                    resource_state.last_submission.value()
                                );
                                const bool imported = AppendPrefixImport(
                                    translate_info,
                                    imported_resources,
                                    canonical_key,
                                    resource_state,
                                    access,
                                    resource_state.owner_queue
                                );
                                if (imported) {
                                    auto& seed_state =
                                        translate_info.initial_state_snapshot[canonical_key];
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
                                        translate_info.queue
                                    );
                                }
                            };

                        if (IsTextureKey(resource_key)) {
                            ForEachTextureSubresourceKey(resource_key, process_cross_queue_resource);
                        } else {
                            process_cross_queue_resource(resource_key);
                        }
                    }

                    ApplyDigestToState(
                        translate_info.digest,
                        translate_info.queue,
                        translate_info.key,
                        segment_state
                    );
                    translate_info.last_state_snapshot = segment_state;
                    RHITRACE_LOG(
                        basic,
                        "[RHITrace][PreprocessSegment] submit=({}, {}) source=({}, {}) queue={} segment={} digest_count={} wait_count={} import_tex={} import_buf={} export_tex={} export_buf={}",
                        translate_info.key.op_seq,
                        translate_info.key.submit_idx,
                        translate_info.source_key.op_seq,
                        translate_info.source_key.submit_idx,
                        QueueTypeName(translate_info.queue),
                        SegmentOriginName(translate_info.segment),
                        translate_info.digest.size(),
                        preprocess_store.dependency_graph.Count(translate_info.key),
                        translate_info.prefix_import_textures.size(),
                        translate_info.prefix_import_buffers.size(),
                        translate_info.suffix_export_textures.size(),
                        translate_info.suffix_export_buffers.size()
                    );

                    source_plan.segments.emplace_back(SourceSubmitSegmentPlan{
                        .key = translate_info.key,
                        .queue = translate_info.queue,
                        .inherit_source_wait_events = false,
                        .inherit_source_signal_events_and_callbacks = false,
                        .inherit_source_runtime_payload = false,
                        .include = true
                    });
                    source_last_translate_event = ChainGraphEvents(
                        source_last_translate_event,
                        translate_info.completion_event
                    );
                    dependency_state.last_translate_event = ChainGraphEvents(
                        dependency_state.last_translate_event,
                        translate_info.completion_event
                    );
                    previous_gpu_segment_key = translate_info.key;
                    previous_gpu_segment_queue = translate_info.queue;
                    CommitPersistentResourceStates(translate_info.last_state_snapshot);
                    ApplyDigestToDirtyWrittenResources(
                        dirty_written_resources,
                        translate_info.digest
                    );

                    for (const GraphEventRef& fence_event : CollectSegmentFenceEvents(submit, segment)) {
                        dependency_state.last_fence_event = ChainGraphEvents(
                            dependency_state.last_fence_event,
                            fence_event
                        );
                    }
                    if (SegmentContainsFrameTick(submit, segment)) {
                        if (segment_index + 1 != segments.size()) {
                            LOG_ERROR(MOER_TEXT("FrameTick must terminate the recorded CommandList segment sequence"));
                            assert(false && "FrameTick must terminate the segment sequence");
                        }
                        dependency_state = {};
                    }
                    preprocess_store.Add(std::move(translate_info));
                }

                if (!source_plan.segments.empty()) {
                    for (auto& segment_plan : source_plan.segments) {
                        segment_plan.inherit_source_wait_events = true;
                        break;
                    }
                    for (auto segment_iter = source_plan.segments.rbegin();
                         segment_iter != source_plan.segments.rend();
                         ++segment_iter) {
                        segment_iter->inherit_source_signal_events_and_callbacks = true;
                        break;
                    }
                    for (auto segment_iter = source_plan.segments.rbegin();
                         segment_iter != source_plan.segments.rend();
                         ++segment_iter) {
                        if (segment_iter->queue != EQueueType::Copy) {
                            segment_iter->inherit_source_runtime_payload = true;
                            break;
                        }
                    }
                }
                preprocess_store.AddSourcePlan(std::move(source_plan));
            }
        } else if (const auto* present_op = std::get_if<ExecutorPresentOp>(&op)) {
            PresentCandidateMetadata present_result{.op_seq = op_seq};
            if (present_op->target.texture) {
                const ResourceKey present_source_key = MakeTextureKeyWithRange(
                    uint64(present_op->target.texture),
                    present_op->target.mip_level,
                    1,
                    present_op->target.array_layer,
                    1
                );
                ResourceAccessDigest present_digest;
                MergeDigestEntry(
                    present_digest,
                    present_source_key,
                    true,
                    false,
                    std::nullopt,
                    ETextureState::TRANSFER_SRC
                );
                ResourceStateSnapshot present_snapshot{};
                EnsureDigestStateLoaded(present_snapshot, present_digest);
                if (const auto it = present_snapshot.find(present_source_key);
                    it != present_snapshot.end()) {
                    present_result.has_source_texture_state = true;
                    present_result.source_texture_state     = it->second;
                }
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
                    "[ResourceTrace][Preprocess][Present] {} : recorded TRANSFER read, committed to persistent state (queue={} mip={} layer={})",
                    static_cast<VulkanTexture*>(present_op->target.texture)->GetName(),
                    QueueTypeName(present_op->queue),
                    int(present_op->target.mip_level),
                    int(present_op->target.array_layer)
                );
            }
            preprocess_store.AddPresent(std::move(present_result));
        }
        ++op_seq;
    }

    preprocess_store.dependency_graph.SortEdges();
    return preprocess_store;
}

} // namespace

void LogicalDependencyGraph::SortEdges() {
    for (auto& [_, producer_keys] : producer_keys_by_consumer) {
        std::sort(producer_keys.begin(), producer_keys.end(), SubmissionKeyLess);
    }
}

PreprocessTranslateStore SubmissionPreprocessor::Process(
    const Array<ExecutorOp>& ops,
    uint64                   op_seq_base
) {
    return PreprocessFrameOps(ops, op_seq_base, dependency_state);
}

} // namespace Moer::Render