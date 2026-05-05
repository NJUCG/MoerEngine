#pragma once

#include "RenderGraphHandle.h"
#include "RenderAPI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

#include <cstdint>
#include <span>
#include <string>

namespace Moer {

using RHICommandList = Render::CommandList;

struct RGTextureDesc {
    Extent3D           extent{};
    EPixelFormat       format{PF_UNDEFINED};
    ETextureUsageFlags usage{};
    uint32_t           mip_levels{1};
    uint32_t           array_layers{1};
};

struct RGBufferDesc {
    uint64_t          size{0};
    EBufferUsageFlags usage{};
};

struct RGTextureRange {
    ETextureAspectFlags aspect{ETextureAspectFlags::COLOR};
    uint32_t            mip_min{0};
    uint32_t            mip_count{1};
    uint32_t            array_min{0};
    uint32_t            array_count{1};

    // Overlaps when aspect flags intersect and mip/array intervals both intersect.
    RENDER_API bool Overlaps(const RGTextureRange& other) const;
};

struct RGBufferRange {
    uint64_t offset{0};
    uint64_t size{0};

    // {0, 0} represents the whole buffer.
    RENDER_API bool IsWholeResource() const;
    // Whole-buffer ranges overlap every valid range; partial ranges overlap when byte intervals intersect.
    RENDER_API bool Overlaps(const RGBufferRange& other) const;
};

enum class ERGResourceKind : uint8_t {
    Texture,
    Buffer
};

enum class ERGAccessMode : uint8_t {
    Read,
    Write,
    ReadWrite
};

struct RGTextureAccess {
    RenderGraphHandle       handle{};
    RGTextureRange          range{};
    ERGAccessMode           mode{ERGAccessMode::Read};
    Render::ETextureState   state{Render::ETextureState::SHADER_RESOURCE};
    Render::EQueueType      queue{Render::EQueueType::Ignore};
    bool                    bindless{false};
};

struct RGBufferAccess {
    RenderGraphHandle      handle{};
    RGBufferRange          range{};
    ERGAccessMode          mode{ERGAccessMode::Read};
    Render::EBufferState   state{Render::EBufferState::SHADER_RESOURCE};
    Render::EQueueType     queue{Render::EQueueType::Ignore};
    bool                   bindless{false};
};

struct RGTextureView {
    RenderGraphHandle      handle{};
    RGTextureRange         range{};
    ERGAccessMode          access{ERGAccessMode::Read};
    Render::ETextureState  state{Render::ETextureState::SHADER_RESOURCE};
    Render::EQueueType     queue{Render::EQueueType::Ignore};
    bool                   bindless{false};

    RENDER_API RGTextureAccess ToAccess() const;
};

struct RGBufferView {
    RenderGraphHandle     handle{};
    RGBufferRange         range{};
    ERGAccessMode         access{ERGAccessMode::Read};
    Render::EBufferState  state{Render::EBufferState::SHADER_RESOURCE};
    Render::EQueueType    queue{Render::EQueueType::Ignore};
    bool                  bindless{false};

    RENDER_API RGBufferAccess ToAccess() const;
};

using RGTextureAccessArray = std::span<const RGTextureView>;
using RGBufferAccessArray  = std::span<const RGBufferView>;

struct RGResource {
    std::string       name{};
    ERGResourceKind   kind{ERGResourceKind::Texture};
    bool              imported{false};
    bool              exported{false};
    RGTextureDesc     texture_desc{};
    RGBufferDesc      buffer_desc{};
    Render::TextureRef imported_texture{};
    Render::BufferRef  imported_buffer{};
    Render::EQueueType owner_queue{Render::EQueueType::Graphics};
    Render::ETextureState initial_texture_state{Render::ETextureState::UNDEFINED};
    Render::EBufferState  initial_buffer_state{Render::EBufferState::UNDEFINED};
    Render::ETextureState final_texture_state{Render::ETextureState::UNDEFINED};
    Render::EBufferState  final_buffer_state{Render::EBufferState::UNDEFINED};
};

RENDER_API bool RGAccessWrites(ERGAccessMode mode);
RENDER_API bool RGAccessConflicts(ERGAccessMode lhs, ERGAccessMode rhs);

} // namespace Moer
