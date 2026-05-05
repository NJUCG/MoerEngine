#pragma once

#include "RenderGraphResource.h"
#include "misc/STL.h"

#include <cstdint>
#include <functional>
#include <string>
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
    // CPU preparation context for graph-owned setup work before pass execution.
    // Do not declare resource access, record RHI commands, or submit work here.
    explicit RGSetupContext(RenderGraph& graph) : m_graph(graph) {}

    RenderGraph& Graph() const { return m_graph; }

private:
    RenderGraph& m_graph;
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

RENDER_API Render::EQueueType RGPassQueue(ERGPassFlags flags);
RENDER_API EPassType          RGPassType(ERGPassFlags flags);
RENDER_API bool               RGPassHasSingleExecutionDomain(ERGPassFlags flags);

class RGParameterAccessCollector {
public:
    // Called by parameter DeclareRGAccess() implementations to register one texture dependency.
    void AddTexture(const RGTextureView& view) {
        texture_accesses.push_back(view.ToAccess());
    }

    void AddTextures(RGTextureAccessArray views) {
        for (const RGTextureView& view : views) {
            AddTexture(view);
        }
    }

    // Called by parameter DeclareRGAccess() implementations to register one buffer dependency.
    void AddBuffer(const RGBufferView& view) {
        buffer_accesses.push_back(view.ToAccess());
    }

    void AddBuffers(RGBufferAccessArray views) {
        for (const RGBufferView& view : views) {
            AddBuffer(view);
        }
    }

    const Moer::Array<RGTextureAccess>& Textures() const { return texture_accesses; }
    const Moer::Array<RGBufferAccess>& Buffers() const { return buffer_accesses; }

private:
    Moer::Array<RGTextureAccess> texture_accesses{};
    Moer::Array<RGBufferAccess>  buffer_accesses{};
};

struct RGPass {
    using Execute = std::function<void(RHICommandList& cmd_list, RGContext context)>;
    using CollectAccess = std::function<void(const void* parameters, RGParameterAccessCollector& collector)>;

    std::string               name{};
    void*                     parameters{nullptr};
    std::type_index           parameter_type{typeid(void)};
    uint32_t                  parameter_size{0};
    ERGPassFlags              flags{ERGPassFlags::None};
    Execute                   execute{};
    CollectAccess             collect_access{};
    Moer::Array<RGTextureAccess> texture_accesses{};
    Moer::Array<RGBufferAccess>  buffer_accesses{};
};

struct RGSetupPass {
    std::string name{};
    // Empty callbacks are legal no-op slots for future compiled setup scheduling.
    std::function<void(RGSetupContext& setup)> execute{};
};

} // namespace Moer
