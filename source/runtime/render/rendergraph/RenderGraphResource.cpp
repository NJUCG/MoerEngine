#include "rendergraph/RenderGraphResource.h"

#include <cassert>
#include <utility>

namespace Moer::Render {

bool RGTextureRange::Overlaps(const RGTextureRange& other) const {
    if (mip_count == 0 || array_count == 0 || other.mip_count == 0 || other.array_count == 0) {
        return false;
    }
    if (!EnumHasAnyFlag(aspect, other.aspect)) {
        return false;
    }

    const uint32_t mip_end         = mip_min + mip_count;
    const uint32_t other_mip_end   = other.mip_min + other.mip_count;
    const uint32_t array_end       = array_min + array_count;
    const uint32_t other_array_end = other.array_min + other.array_count;

    return mip_min < other_mip_end && other.mip_min < mip_end && array_min < other_array_end &&
           other.array_min < array_end;
}

bool RGTextureRange::Contains(const RGTextureRange& other) const {
    if (mip_count == 0 || array_count == 0 || other.mip_count == 0 || other.array_count == 0) {
        return false;
    }

    const uint32_t aspect_mask       = static_cast<uint32_t>(aspect);
    const uint32_t other_aspect_mask = static_cast<uint32_t>(other.aspect);
    if ((aspect_mask & other_aspect_mask) != other_aspect_mask) {
        return false;
    }

    const uint32_t mip_end         = mip_min + mip_count;
    const uint32_t other_mip_end   = other.mip_min + other.mip_count;
    const uint32_t array_end       = array_min + array_count;
    const uint32_t other_array_end = other.array_min + other.array_count;

    return mip_min <= other.mip_min && other_mip_end <= mip_end && array_min <= other.array_min &&
           other_array_end <= array_end;
}

bool RGBufferRange::IsWholeResource() const {
    return offset == 0 && size == 0;
}

bool RGBufferRange::Contains(const RGBufferRange& other) const {
    if (IsWholeResource()) {
        return true;
    }
    if (other.IsWholeResource() || size == 0 || other.size == 0) {
        return false;
    }

    const uint64_t end       = offset + size;
    const uint64_t other_end = other.offset + other.size;
    return offset <= other.offset && other_end <= end;
}

bool RGBufferRange::Overlaps(const RGBufferRange& other) const {
    if ((!IsWholeResource() && size == 0) || (!other.IsWholeResource() && other.size == 0)) {
        return false;
    }
    if (IsWholeResource() || other.IsWholeResource()) {
        return true;
    }

    const uint64_t end       = offset + size;
    const uint64_t other_end = other.offset + other.size;
    return offset < other_end && other.offset < end;
}

bool RGTextureStateWrites(Render::ETextureState state) {
    return RGTextureStateInitializesContent(state);
}

bool RGBufferStateWrites(Render::EBufferState state) {
    return RGBufferStateInitializesContent(state);
}

bool RGTextureStateInitializesContent(Render::ETextureState state) {
    switch (state) {
        case Render::ETextureState::TRANSFER_DST:
        case Render::ETextureState::RENDER_TARGET:
        case Render::ETextureState::DEPTH_STENCIL_WRITE:
        case Render::ETextureState::UNORDERED_ACCESS:
            return true;
        default:
            return false;
    }
}

bool RGBufferStateInitializesContent(Render::EBufferState state) {
    switch (state) {
        case Render::EBufferState::TRANSFER_DST:
        case Render::EBufferState::UNORDERED_ACCESS:
        case Render::EBufferState::ACCELERATION_STRUCTURE_WRITE:
            return true;
        default:
            return false;
    }
}

bool RGTextureStatesCompatible(Render::ETextureState previous, Render::ETextureState next) {
    if (previous == Render::ETextureState::UNDEFINED || next == Render::ETextureState::UNDEFINED) {
        return true;
    }
    if (previous == next) {
        return true;
    }
    const bool previous_shader_read = previous == Render::ETextureState::SHADER_RESOURCE ||
                                      previous == Render::ETextureState::SAMPLED;
    const bool next_shader_read = next == Render::ETextureState::SHADER_RESOURCE ||
                                  next == Render::ETextureState::SAMPLED;
    return previous_shader_read && next_shader_read;
}

bool RGBufferStatesCompatible(Render::EBufferState previous, Render::EBufferState next) {
    if (previous == Render::EBufferState::UNDEFINED || next == Render::EBufferState::UNDEFINED) {
        return true;
    }
    return previous == next;
}

bool RGTextureStatesMergeable(Render::ETextureState previous, Render::ETextureState next) {
    if (!RGTextureStatesCompatible(previous, next)) {
        return false;
    }
    return !RGTextureStateInitializesContent(previous) && !RGTextureStateInitializesContent(next);
}

bool RGBufferStatesMergeable(Render::EBufferState previous, Render::EBufferState next) {
    if (!RGBufferStatesCompatible(previous, next)) {
        return false;
    }
    return !RGBufferStateInitializesContent(previous) && !RGBufferStateInitializesContent(next);
}

void RGTransientResource::ResetCompileState() {
    owner_queue = Render::EQueueType::Ignore;
    owner_pass  = invalid_pass;
}

bool RGTransientResource::HasOwner() const {
    return owner_queue != Render::EQueueType::Ignore && owner_pass != invalid_pass;
}

bool RGTransientResource::NeedsOwnerTransfer(Render::EQueueType next_queue) const {
    return enabled && HasOwner() && next_queue != Render::EQueueType::Ignore && owner_queue != next_queue;
}

void RGTransientResource::SetOwner(Render::EQueueType queue, uint32_t pass_index) {
    owner_queue = queue;
    owner_pass  = pass_index;
}

void RGResourceCompileInfo::Reset() {
    first_pass = invalid_pass;
    last_pass  = invalid_pass;
}

void RGResourceCompileInfo::RecordAccess(uint32_t pass_index) {
    if (first_pass == invalid_pass) {
        first_pass = pass_index;
    }
    last_pass = pass_index;
}

RGResource::RGResource(
    StringView resource_name,
    ERGResourceKind resource_kind,
    bool imported_resource,
    bool transient_resource
) :
    name(resource_name),
    kind(resource_kind),
    imported(imported_resource) {
    transient.enabled = transient_resource;
}

RGTexture::RGTexture(StringView name, const RGTextureDesc& desc) :
    RGResource(name, ERGResourceKind::Texture, false, true),
    m_desc(desc) {}

RGTexture::RGTexture(StringView name, PooledTextureRef texture, bool imported) :
    RGResource(name, ERGResourceKind::Texture, imported, false),
    m_desc(texture ? texture->Desc() : RGTextureDesc{}),
    m_texture(std::move(texture)) {}

void RGTexture::Bind(PooledTextureRef texture) {
    assert(texture && texture->IsAllocated());
    m_desc    = texture->Desc();
    m_texture = std::move(texture);
}

void RGTexture::ReleaseTransient() {
    if (!imported) {
        m_texture.reset();
        alias_initial_state_ranges.clear();
    }
}

Render::TextureView RGTexture::GetView(const RGTextureRange& range) const {
    auto* texture = m_texture ? m_texture->RHI().Get() : nullptr;
    assert(texture != nullptr);
    const uint8 mip_count = range.mip_count == 0 ? 1 : static_cast<uint8>(range.mip_count);
    return texture->GetView(static_cast<uint8>(range.mip_min), mip_count)
        .Slice(range.array_min, range.array_count);
}

RGBuffer::RGBuffer(StringView name, const RGBufferDesc& desc) :
    RGResource(name, ERGResourceKind::Buffer, false, true),
    m_desc(desc) {}

RGBuffer::RGBuffer(StringView name, PooledBufferRef buffer, bool imported) :
    RGResource(name, ERGResourceKind::Buffer, imported, false),
    m_desc(buffer ? buffer->Desc() : RGBufferDesc{}),
    m_buffer(std::move(buffer)) {}

void RGBuffer::Bind(PooledBufferRef buffer) {
    assert(buffer && buffer->IsAllocated());
    m_desc   = buffer->Desc();
    m_buffer = std::move(buffer);
}

void RGBuffer::ReleaseTransient() {
    if (!imported) {
        m_buffer.reset();
        alias_initial_state_ranges.clear();
    }
}

Render::BufferView RGBuffer::GetView(const RGBufferRange& range) const {
    auto* buffer = m_buffer ? m_buffer->RHI().Get() : nullptr;
    assert(buffer != nullptr);
    if (range.IsWholeResource()) {
        return buffer->GetView();
    }
    return buffer->GetView(range.offset, range.size);
}

} // namespace Moer::Render
