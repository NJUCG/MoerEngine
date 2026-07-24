#include "rendergraph/RenderGraphResourcePool.h"

#include "rendergraph/RenderGraph.h"
#include "rhi/RHI.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Moer::Render {
namespace {

[[nodiscard]] bool HasAny(ETextureAspectFlags value, ETextureAspectFlags flags) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flags)) != 0;
}

TextureRef CreateTexture(
    std::string_view               name,
    const RGTransientTextureDesc& desc
) {
    auto& device = RenderDevice::Get();
    switch (desc.dimension) {
        case ETextureDimension::TEX_2D:
        case ETextureDimension::TEX_2D_ARRAY:
        case ETextureDimension::TEX_3D:
            return device.CreateTexture(
                name,
                desc.extent,
                desc.format,
                desc.usage,
                desc.mip_count,
                desc.array_size
            );
        case ETextureDimension::TEX_CUBE:
            return device.CreateCubeMap(
                name,
                Extent2D(desc.extent.x, desc.extent.y),
                desc.format,
                desc.usage,
                desc.mip_count
            );
        case ETextureDimension::TEX_CUBE_ARRAY:
        case ETextureDimension::NumBits:
            throw std::invalid_argument(
                "the current RHI factory cannot allocate transient cube arrays"
            );
    }
    throw std::invalid_argument("unsupported transient texture dimension");
}

BufferRef CreateBuffer(
    std::string_view              name,
    const RGTransientBufferDesc& desc
) {
    return RenderDevice::Get().CreateBuffer(
        name,
        BufferInfo{desc.element_count, desc.stride, desc.usage, desc.format}
    );
}

void ValidateTextureResult(
    const RGTransientTextureDesc& desc,
    const TextureRef&             resource
) {
    if (!resource.IsValid()) {
        throw std::runtime_error("transient texture factory returned null");
    }
    const uint3 extent = resource->GetExtent();
    if (extent.x != desc.extent.x || extent.y != desc.extent.y ||
        extent.z != desc.extent.z ||
        resource->GetDimension() != desc.dimension ||
        resource->GetFormat() != desc.format ||
        resource->GetUsage() != desc.usage ||
        resource->GetNumMips() != desc.mip_count ||
        resource->GetNumArray() != desc.PhysicalLayerCount() ||
        resource->GetAspectFlags() != desc.aspect_flags) {
        throw std::runtime_error(
            "transient texture factory returned a resource that does not match its descriptor"
        );
    }
}

void ValidateBufferResult(
    const RGTransientBufferDesc& desc,
    const BufferRef&             resource
) {
    if (!resource.IsValid()) {
        throw std::runtime_error("transient buffer factory returned null");
    }
    if (resource->GetNumElement() != desc.element_count ||
        resource->GetStride() != desc.stride ||
        resource->GetByteSize() != desc.ByteSize() ||
        resource->GetUsage() != desc.usage ||
        resource->GetFormat() != desc.format) {
        throw std::runtime_error(
            "transient buffer factory returned a resource that does not match its descriptor"
        );
    }
}

} // namespace

uint32_t RGTransientTextureDesc::PhysicalLayerCount() const {
    const uint64_t multiplier =
        dimension == ETextureDimension::TEX_CUBE ||
                dimension == ETextureDimension::TEX_CUBE_ARRAY ?
            6u :
            1u;
    const uint64_t layers = multiplier * array_size;
    return layers > std::numeric_limits<uint32_t>::max() ?
               0u :
               static_cast<uint32_t>(layers);
}

bool RGTransientTextureDesc::IsValid() const {
    constexpr uint32_t supported_aspects =
        static_cast<uint32_t>(ETextureAspectFlags::COLOR) |
        static_cast<uint32_t>(ETextureAspectFlags::DEPTH_SLICE) |
        static_cast<uint32_t>(ETextureAspectFlags::STENCIL_SLICE);
    const uint32_t aspect_mask = static_cast<uint32_t>(aspect_flags);
    const uint32_t physical_layers = PhysicalLayerCount();
    if (extent.x == 0 || extent.y == 0 || extent.z == 0 ||
        format == PF_UNDEFINED || usage == ETextureUsageFlags::UNDEFINED ||
        aspect_flags == ETextureAspectFlags::NONE || mip_count == 0 ||
        array_size == 0 || physical_layers == 0 ||
        extent.x > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        extent.y > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        extent.z > std::numeric_limits<uint16_t>::max() ||
        mip_count > std::numeric_limits<uint8_t>::max() ||
        physical_layers > std::numeric_limits<uint8_t>::max() ||
        (aspect_mask & ~supported_aspects) != 0) {
        return false;
    }

    const bool depth_or_stencil = HasAny(
        aspect_flags,
        ETextureAspectFlags::DEPTH_SLICE | ETextureAspectFlags::STENCIL_SLICE
    );
    const bool color = HasAny(aspect_flags, ETextureAspectFlags::COLOR);
    if (depth_or_stencil && color) {
        return false;
    }

    switch (dimension) {
        case ETextureDimension::TEX_2D:
            return extent.z == 1 && array_size == 1;
        case ETextureDimension::TEX_2D_ARRAY:
            return extent.z == 1 && array_size > 1;
        case ETextureDimension::TEX_3D:
            return extent.z > 1 && array_size == 1;
        case ETextureDimension::TEX_CUBE:
            return extent.z == 1 && extent.x == extent.y && array_size == 1;
        case ETextureDimension::TEX_CUBE_ARRAY:
        case ETextureDimension::NumBits:
            return false;
    }
    return false;
}

uint64_t RGTransientBufferDesc::ByteSize() const {
    if (stride != 0 &&
        element_count > std::numeric_limits<uint64_t>::max() / stride) {
        return 0;
    }
    return static_cast<uint64_t>(element_count) * stride;
}

bool RGTransientBufferDesc::IsValid() const {
    return element_count > 0 && stride > 0 &&
           usage != EBufferUsageFlags::NONE && ByteSize() > 0;
}

RenderGraphResourcePool::RenderGraphResourcePool(
    uint32_t       retire_after_idle_frames,
    TextureFactory texture_factory,
    BufferFactory  buffer_factory
) :
    retire_after_idle_frames(retire_after_idle_frames),
    texture_factory(
        texture_factory ? std::move(texture_factory) : TextureFactory{CreateTexture}
    ),
    buffer_factory(
        buffer_factory ? std::move(buffer_factory) : BufferFactory{CreateBuffer}
    ) {}

RenderGraphResourcePool& RenderGraphResourcePool::Global() {
    static RenderGraphResourcePool pool{};
    return pool;
}

TextureRef RenderGraphResourcePool::AcquireTexture(
    std::string_view               name,
    const RGTransientTextureDesc& desc
) {
    if (!desc.IsValid()) {
        throw std::invalid_argument("invalid transient texture descriptor");
    }
    std::lock_guard lock(mutex);
    return AcquireTextureLocked(name, desc);
}

BufferRef RenderGraphResourcePool::AcquireBuffer(
    std::string_view              name,
    const RGTransientBufferDesc& desc
) {
    if (!desc.IsValid()) {
        throw std::invalid_argument("invalid transient buffer descriptor");
    }
    std::lock_guard lock(mutex);
    return AcquireBufferLocked(name, desc);
}

TextureRef RenderGraphResourcePool::AcquireTextureLocked(
    std::string_view               name,
    const RGTransientTextureDesc& desc
) {
    for (auto& entry : textures) {
        if (entry.resource.IsValid() && entry.resource->IsUniquelyReferenced() &&
            entry.desc == desc) {
            entry.idle_frames = 0;
            entry.resource->SetName(name);
            return entry.resource;
        }
    }

    TextureRef resource = texture_factory(name, desc);
    ValidateTextureResult(desc, resource);
    textures.push_back(TextureEntry{.desc = desc, .resource = resource});
    return resource;
}

BufferRef RenderGraphResourcePool::AcquireBufferLocked(
    std::string_view              name,
    const RGTransientBufferDesc& desc
) {
    for (auto& entry : buffers) {
        if (entry.resource.IsValid() && entry.resource->IsUniquelyReferenced() &&
            entry.desc == desc) {
            entry.idle_frames = 0;
            entry.resource->SetName(name);
            return entry.resource;
        }
    }

    BufferRef resource = buffer_factory(name, desc);
    ValidateBufferResult(desc, resource);
    buffers.push_back(BufferEntry{.desc = desc, .resource = resource});
    return resource;
}

void RenderGraphResourcePool::Tick() {
    std::lock_guard lock(mutex);
    std::erase_if(textures, [&](TextureEntry& entry) {
        if (!entry.resource.IsValid() ||
            !entry.resource->IsUniquelyReferenced()) {
            entry.idle_frames = 0;
            return false;
        }
        return ++entry.idle_frames > retire_after_idle_frames;
    });
    std::erase_if(buffers, [&](BufferEntry& entry) {
        if (!entry.resource.IsValid() ||
            !entry.resource->IsUniquelyReferenced()) {
            entry.idle_frames = 0;
            return false;
        }
        return ++entry.idle_frames > retire_after_idle_frames;
    });
}

void RenderGraphResourcePool::Reset() {
    std::lock_guard lock(mutex);
    textures.clear();
    buffers.clear();
}

uint32_t RenderGraphResourcePool::TextureCount() const {
    std::lock_guard lock(mutex);
    return static_cast<uint32_t>(textures.size());
}

uint32_t RenderGraphResourcePool::BufferCount() const {
    std::lock_guard lock(mutex);
    return static_cast<uint32_t>(buffers.size());
}

uint32_t RenderGraphResourcePool::AvailableTextureCount() const {
    std::lock_guard lock(mutex);
    return static_cast<uint32_t>(std::count_if(
        textures.begin(),
        textures.end(),
        [](const TextureEntry& entry) {
            return entry.resource.IsValid() &&
                   entry.resource->IsUniquelyReferenced();
        }
    ));
}

uint32_t RenderGraphResourcePool::AvailableBufferCount() const {
    std::lock_guard lock(mutex);
    return static_cast<uint32_t>(std::count_if(
        buffers.begin(),
        buffers.end(),
        [](const BufferEntry& entry) {
            return entry.resource.IsValid() &&
                   entry.resource->IsUniquelyReferenced();
        }
    ));
}

RenderGraphTransientAllocator& RenderGraphTransientAllocator::Global() {
    static RenderGraphTransientAllocator allocator{RenderGraphResourcePool::Global()};
    return allocator;
}

bool RenderGraphTransientAllocator::Prepare(
    RenderGraph& graph,
    std::string& error
) {
    error.clear();
    if (!graph.compiled) {
        error = "transient allocation requires a successfully compiled graph";
        return false;
    }
    if (graph.compiled_plan.resources.size() != graph.resources.size()) {
        error = "compiled transient resource table is incomplete";
        return false;
    }

    std::unordered_map<uint32_t, TextureRef> texture_slots{};
    std::unordered_map<uint32_t, BufferRef>  buffer_slots{};
    std::unordered_map<uint32_t, RGTransientTextureDesc> texture_slot_descs{};
    std::unordered_map<uint32_t, RGTransientBufferDesc>  buffer_slot_descs{};
    std::unordered_map<uint32_t, RenderGraph::ResourceKind> slot_kinds{};
    std::vector<TextureRef> texture_bindings(graph.resources.size());
    std::vector<BufferRef>  buffer_bindings(graph.resources.size());

    try {
        for (const auto& compiled_resource : graph.compiled_plan.resources) {
            if (!graph.IsValidResource(compiled_resource.resource)) {
                error = "compiled transient resource references an invalid handle";
                return false;
            }
            auto& resource = graph.resources[compiled_resource.resource.index];
            if (resource.imported || resource.kind == RenderGraph::ResourceKind::Token ||
                compiled_resource.first_use == RenderGraph::PassHandle::InvalidIndex) {
                continue;
            }
            if (compiled_resource.transient_slot ==
                RenderGraph::PassHandle::InvalidIndex) {
                error =
                    "active transient resource lacks a physical allocation descriptor: '" +
                    resource.name + "'";
                return false;
            }
            const auto [kind, inserted_kind] = slot_kinds.emplace(
                compiled_resource.transient_slot,
                resource.kind
            );
            if (!inserted_kind && kind->second != resource.kind) {
                error =
                    "transient slot aliases different resource kinds: '" +
                    resource.name + "'";
                return false;
            }

            if (resource.kind == RenderGraph::ResourceKind::Texture) {
                if (!resource.transient_texture_desc.has_value()) {
                    error =
                        "transient texture lacks a physical allocation descriptor: '" +
                        resource.name + "'";
                    return false;
                }
                const auto [slot_desc, inserted_desc] =
                    texture_slot_descs.emplace(
                        compiled_resource.transient_slot,
                        *resource.transient_texture_desc
                    );
                if (!inserted_desc &&
                    slot_desc->second != *resource.transient_texture_desc) {
                    error =
                        "transient texture slot aliases incompatible descriptors: '" +
                        resource.name + "'";
                    return false;
                }
                auto& slot = texture_slots[compiled_resource.transient_slot];
                if (!slot.IsValid()) {
                    slot = resource.physical_texture.IsValid() ?
                               resource.physical_texture :
                               pool.AcquireTexture(
                                   resource.name,
                                   *resource.transient_texture_desc
                               );
                } else if (resource.physical_texture.IsValid() &&
                           resource.physical_texture.Get() != slot.Get()) {
                    error =
                        "transient texture slot has conflicting physical bindings: '" +
                        resource.name + "'";
                    return false;
                }
                texture_bindings[compiled_resource.resource.index] = slot;
            } else {
                if (!resource.transient_buffer_desc.has_value()) {
                    error =
                        "transient buffer lacks a physical allocation descriptor: '" +
                        resource.name + "'";
                    return false;
                }
                const auto [slot_desc, inserted_desc] =
                    buffer_slot_descs.emplace(
                        compiled_resource.transient_slot,
                        *resource.transient_buffer_desc
                    );
                if (!inserted_desc &&
                    slot_desc->second != *resource.transient_buffer_desc) {
                    error =
                        "transient buffer slot aliases incompatible descriptors: '" +
                        resource.name + "'";
                    return false;
                }
                auto& slot = buffer_slots[compiled_resource.transient_slot];
                if (!slot.IsValid()) {
                    slot = resource.physical_buffer.IsValid() ?
                               resource.physical_buffer :
                               pool.AcquireBuffer(
                                   resource.name,
                                   *resource.transient_buffer_desc
                               );
                } else if (resource.physical_buffer.IsValid() &&
                           resource.physical_buffer.Get() != slot.Get()) {
                    error =
                        "transient buffer slot has conflicting physical bindings: '" +
                        resource.name + "'";
                    return false;
                }
                buffer_bindings[compiled_resource.resource.index] = slot;
            }
        }
    } catch (const std::exception& exception) {
        error = std::string("transient physical allocation failed: ") +
                exception.what();
        return false;
    } catch (...) {
        error = "transient physical allocation failed";
        return false;
    }

    for (uint32_t index = 0; index < graph.resources.size(); ++index) {
        auto& resource = graph.resources[index];
        if (texture_bindings[index].IsValid()) {
            resource.physical_texture  = std::move(texture_bindings[index]);
            resource.physical_identity = resource.physical_texture.Get();
        } else if (buffer_bindings[index].IsValid()) {
            resource.physical_buffer   = std::move(buffer_bindings[index]);
            resource.physical_identity = resource.physical_buffer.Get();
        }
    }
    return true;
}

void RenderGraphTransientAllocator::ReleaseNonExported(RenderGraph& graph) noexcept {
    graph.ReleaseNonExportedTransientBindings();
}

} // namespace Moer::Render
