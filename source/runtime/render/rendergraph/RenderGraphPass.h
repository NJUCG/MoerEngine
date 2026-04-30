#pragma once

#include "RenderGraphResource.h"
#include "misc/STL.h"

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <typeindex>

namespace Moer {

class RenderGraph;

class RGContext {
public:
    explicit RGContext(RenderGraph& graph) : m_graph(graph) {}

    RenderGraph& Graph() const { return m_graph; }

private:
    RenderGraph& m_graph;
};

class RGSetupContext {
public:
    RGSetupContext(RenderGraph& graph, uint32_t pass_index) : m_graph(graph), m_pass_index(pass_index) {}

    void ReadTexture(
        RenderGraphHandle      handle,
        Render::ETextureState  state = Render::ETextureState::SHADER_RESOURCE,
        RGTextureRange         range = {},
        Render::EQueueType     queue = Render::EQueueType::Graphics,
        bool                   bindless = false
    );

    void WriteTexture(
        RenderGraphHandle      handle,
        Render::ETextureState  state = Render::ETextureState::RENDER_TARGET,
        RGTextureRange         range = {},
        Render::EQueueType     queue = Render::EQueueType::Graphics,
        bool                   bindless = false
    );

    void ReadWriteTexture(
        RenderGraphHandle      handle,
        Render::ETextureState  state = Render::ETextureState::UNORDERED_ACCESS,
        RGTextureRange         range = {},
        Render::EQueueType     queue = Render::EQueueType::Graphics,
        bool                   bindless = false
    );

    void ReadBuffer(
        RenderGraphHandle     handle,
        Render::EBufferState  state = Render::EBufferState::SHADER_RESOURCE,
        RGBufferRange         range = {},
        Render::EQueueType    queue = Render::EQueueType::Graphics,
        bool                  bindless = false
    );

    void WriteBuffer(
        RenderGraphHandle     handle,
        Render::EBufferState  state = Render::EBufferState::UNORDERED_ACCESS,
        RGBufferRange         range = {},
        Render::EQueueType    queue = Render::EQueueType::Graphics,
        bool                  bindless = false
    );

    void ReadWriteBuffer(
        RenderGraphHandle     handle,
        Render::EBufferState  state = Render::EBufferState::UNORDERED_ACCESS,
        RGBufferRange         range = {},
        Render::EQueueType    queue = Render::EQueueType::Graphics,
        bool                  bindless = false
    );

private:
    RenderGraph& m_graph;
    uint32_t     m_pass_index{0};
};

enum class ERGPassFlags : uint8_t {
    None       = 0,
    Graphics   = 1 << 0,
    Compute    = 1 << 1,
    Copy       = 1 << 2,
    Raytracing = 1 << 3
};

constexpr ERGPassFlags operator|(ERGPassFlags lhs, ERGPassFlags rhs) {
    return static_cast<ERGPassFlags>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
}

constexpr bool EnumHasAnyFlag(ERGPassFlags value, ERGPassFlags flag) {
    return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
}

Render::EQueueType RGPassQueue(ERGPassFlags flags);
EPassType          RGPassType(ERGPassFlags flags);
bool               RGPassHasSingleExecutionDomain(ERGPassFlags flags);

struct RGPass {
    using Execute = std::function<void(RHICommandList& cmd_list, RGContext context)>;

    std::string               name{};
    void*                     parameters{nullptr};
    std::type_index           parameter_type{typeid(void)};
    uint32_t                  parameter_size{0};
    ERGPassFlags              flags{ERGPassFlags::None};
    Execute                   execute{};
    Moer::Array<RGTextureAccess> texture_accesses{};
    Moer::Array<RGBufferAccess>  buffer_accesses{};
};

struct RGSetupPass {
    std::string name{};
};

} // namespace Moer
