#pragma once

#include "RenderGraphResource.h"
#include "misc/STL.h"
#include "string/String.h"

#include <cstdint>
#include <functional>
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
    Copy       = 1 << 2
};

constexpr ERGPassFlags operator|(ERGPassFlags lhs, ERGPassFlags rhs) {
    return static_cast<ERGPassFlags>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
}

constexpr bool EnumHasAnyFlag(ERGPassFlags value, ERGPassFlags flag) {
    return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
}

RENDER_API Render::EQueueType RGPassQueue(ERGPassFlags flags);
RENDER_API EPassType          RGPassType(ERGPassFlags flags);
RENDER_API bool               RGPassHasValidQueueFlags(ERGPassFlags flags);
RENDER_API bool               RGPassHasQueue(ERGPassFlags flags);

class RGParameterAccessCollector {
public:
    // Called by parameter DeclareRGAccess() implementations to register one texture dependency.
    void AddTexture(const RGTextureView& view) {
        texture_accesses.push_back(view.ToAccess());
    }

    void AddTextures(const RGTextureAccessArray& views) {
        for (const RGTextureView& view : views.Views()) {
            AddTexture(view);
        }
    }

    void AddTextures(const RGTextureAccessArray* views) {
        if (views != nullptr) {
            AddTextures(*views);
        }
    }

    // Called by parameter DeclareRGAccess() implementations to register one buffer dependency.
    void AddBuffer(const RGBufferView& view) {
        buffer_accesses.push_back(view.ToAccess());
    }

    void AddBuffers(const RGBufferAccessArray& views) {
        for (const RGBufferView& view : views.Views()) {
            AddBuffer(view);
        }
    }

    void AddBuffers(const RGBufferAccessArray* views) {
        if (views != nullptr) {
            AddBuffers(*views);
        }
    }

    void Add(const RGTextureView& view) { AddTexture(view); }
    void Add(const RGBufferView& view) { AddBuffer(view); }
    void Add(const RGTextureAccessArray& views) { AddTextures(views); }
    void Add(const RGBufferAccessArray& views) { AddBuffers(views); }
    void Add(const RGTextureAccessArray* views) { AddTextures(views); }
    void Add(const RGBufferAccessArray* views) { AddBuffers(views); }

    const Moer::Array<RGTextureAccess>& Textures() const { return texture_accesses; }
    const Moer::Array<RGBufferAccess>& Buffers() const { return buffer_accesses; }

private:
    Moer::Array<RGTextureAccess> texture_accesses{};
    Moer::Array<RGBufferAccess>  buffer_accesses{};
};

inline void CollectRGParameterAccess(RGParameterAccessCollector&) {}

template<typename T, typename... Rest>
void CollectRGParameterAccess(RGParameterAccessCollector& collector, const T& access, const Rest&... rest) {
    collector.Add(access);
    CollectRGParameterAccess(collector, rest...);
}

#define DEFINE_RG_TEXTURE_ACCESS(name) Moer::RGTextureView name{}
#define DEFINE_RG_BUFFER_ACCESS(name) Moer::RGBufferView name{}
#define DEFINE_RG_TEXTURE_ACCESS_ARRAY(name) Moer::RGTextureAccessArray* name{nullptr}
#define DEFINE_RG_BUFFER_ACCESS_ARRAY(name) Moer::RGBufferAccessArray* name{nullptr}
#define DEFINE_RG_PARAMETER_ACCESS(...)                                      \
    void DeclareRGAccess(Moer::RGParameterAccessCollector& collector) const { \
        Moer::CollectRGParameterAccess(collector __VA_OPT__(, ) __VA_ARGS__); \
    }

enum class ERGPassExecutionMode : uint8_t {
    Serial,
    Parallel
};

struct RGPass {
    using ParallelExecute = std::function<void(RHICommandList& cmd_list, RGContext context)>;
    using SerialExecute = std::function<void(RGContext context)>;
    using CollectAccess = std::function<void(const void* parameters, RGParameterAccessCollector& collector)>;

    String                    name{};
    void*                     parameters{nullptr};
    std::type_index           parameter_type{typeid(void)};
    uint32_t                  parameter_size{0};
    ERGPassFlags              flags{ERGPassFlags::None};
    ERGPassExecutionMode      execution_mode{ERGPassExecutionMode::Parallel};
    ParallelExecute           parallel_execute{};
    SerialExecute             serial_execute{};
    CollectAccess             collect_access{};
    Moer::Array<RGTextureAccess> texture_accesses{};
    Moer::Array<RGBufferAccess>  buffer_accesses{};
};

struct RGSetupPass {
    String name{};
    // Empty callbacks are legal no-op slots for future compiled setup scheduling.
    std::function<void(RGSetupContext& setup)> execute{};
};

} // namespace Moer
