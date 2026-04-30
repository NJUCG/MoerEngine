#pragma once

#include "RenderGraphHandle.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

#include <cstdint>
#include <string>

namespace Moer {

using RHICommandList = CommandList;

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

    bool Overlaps(const RGTextureRange& other) const;
};

struct RGBufferRange {
    uint64_t offset{0};
    uint64_t size{0};

    bool IsWholeResource() const;
    bool Overlaps(const RGBufferRange& other) const;
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
    Render::EQueueType      queue{Render::EQueueType::Graphics};
    bool                    bindless{false};
};

struct RGBufferAccess {
    RenderGraphHandle      handle{};
    RGBufferRange          range{};
    ERGAccessMode          mode{ERGAccessMode::Read};
    Render::EBufferState   state{Render::EBufferState::SHADER_RESOURCE};
    Render::EQueueType     queue{Render::EQueueType::Graphics};
    bool                   bindless{false};
};

struct RGResource {
    std::string       name{};
    ERGResourceKind   kind{ERGResourceKind::Texture};
    bool              imported{false};
    bool              exported{false};
    RGTextureDesc     texture_desc{};
    RGBufferDesc      buffer_desc{};
    RHITextureRef     imported_texture{};
    RHIBufferRef      imported_buffer{};
    Render::EQueueType owner_queue{Render::EQueueType::Graphics};
    Render::ETextureState initial_texture_state{Render::ETextureState::UNDEFINED};
    Render::EBufferState  initial_buffer_state{Render::EBufferState::UNDEFINED};
    Render::ETextureState final_texture_state{Render::ETextureState::UNDEFINED};
    Render::EBufferState  final_buffer_state{Render::EBufferState::UNDEFINED};
};

bool RGAccessWrites(ERGAccessMode mode);
bool RGAccessConflicts(ERGAccessMode lhs, ERGAccessMode rhs);

} // namespace Moer
