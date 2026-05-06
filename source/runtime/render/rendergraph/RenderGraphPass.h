#pragma once

#include "RenderGraphResource.h"
#include "misc/STL.h"
#include "string/String.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <typeindex>

namespace Moer::Render {

class RenderGraph;
class RGParameterAccessCollector;

template<typename T>
concept RGParameterAccessProvider =
    requires(const T& value, RGParameterAccessCollector& collector) { value.DeclareRGAccess(collector); };

class RGContext {
public:
    explicit RGContext(RenderGraph& graph) : m_graph(graph) {}

    RenderGraph& Graph() const {
        return m_graph;
    }

private:
    RenderGraph& m_graph;
};

class RGSetupContext {
public:
    // CPU preparation context for graph-owned setup work before pass execution.
    // Do not declare resource access, record RHI commands, or submit work here.
    explicit RGSetupContext(RenderGraph& graph) : m_graph(graph) {}

    RenderGraph& Graph() const {
        return m_graph;
    }

private:
    RenderGraph& m_graph;
};

enum class ERGPassFlags : uint8_t {
    None     = 0,
    Graphics = 1 << 0,
    Compute  = 1 << 1,
    Copy     = 1 << 2
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
    explicit RGParameterAccessCollector(Render::EQueueType queue) : m_queue(queue) {}

    // Called by parameter DeclareRGAccess() implementations to register one texture dependency.
    void AddTextureAccess(const RGTextureAccess& access) {
        if (!access.handle) {
            return;
        }
        m_texture_accesses.push_back(access);
    }

    template<Render::ETextureState State>
    void AddTexture(const RGTextureStaticAccess<State>& access) {
        AddTextureAccess(access.ToAccess(m_queue));
    }

    void AddTexture(const RGTextureArrayAccess& access) {
        AddTextureAccess(access.ToAccess(m_queue));
    }

    void AddTextures(const RGTextureAccessArray& views) {
        for (const RGTextureArrayAccess& access : views.Accesses()) {
            AddTexture(access);
        }
    }

    void AddTextures(const RGTextureAccessArray* views) {
        if (views != nullptr) {
            AddTextures(*views);
        }
    }

    // Called by parameter DeclareRGAccess() implementations to register one buffer dependency.
    void AddBufferAccess(const RGBufferAccess& access) {
        if (!access.handle) {
            return;
        }
        m_buffer_accesses.push_back(access);
    }

    template<Render::EBufferState State>
    void AddBuffer(const RGBufferStaticAccess<State>& access) {
        AddBufferAccess(access.ToAccess(m_queue));
    }

    void AddBuffer(const RGBufferArrayAccess& access) {
        AddBufferAccess(access.ToAccess(m_queue));
    }

    void AddBuffers(const RGBufferAccessArray& views) {
        for (const RGBufferArrayAccess& access : views.Accesses()) {
            AddBuffer(access);
        }
    }

    void AddBuffers(const RGBufferAccessArray* views) {
        if (views != nullptr) {
            AddBuffers(*views);
        }
    }

    template<Render::ETextureState State>
    void Add(const RGTextureStaticAccess<State>& access) {
        AddTexture(access);
    }

    template<Render::EBufferState State>
    void Add(const RGBufferStaticAccess<State>& access) {
        AddBuffer(access);
    }

    void Add(const RGTextureAccessArray& views) {
        AddTextures(views);
    }
    void Add(const RGBufferAccessArray& views) {
        AddBuffers(views);
    }
    void Add(const RGTextureAccessArray* views) {
        AddTextures(views);
    }
    void Add(const RGBufferAccessArray* views) {
        AddBuffers(views);
    }

    template<RGParameterAccessProvider T>
    void Add(const T& parameters) {
        parameters.DeclareRGAccess(*this);
    }

    const Moer::Array<RGTextureAccess>& Textures() const {
        return m_texture_accesses;
    }
    const Moer::Array<RGBufferAccess>& Buffers() const {
        return m_buffer_accesses;
    }

private:
    Render::EQueueType           m_queue{Render::EQueueType::Ignore};
    Moer::Array<RGTextureAccess> m_texture_accesses{};
    Moer::Array<RGBufferAccess>  m_buffer_accesses{};
};

inline void CollectRGParameterAccess(RGParameterAccessCollector&) {}

template<typename T, typename... Rest>
void CollectRGParameterAccess(RGParameterAccessCollector& collector, const T& access, const Rest&... rest) {
    collector.Add(access);
    CollectRGParameterAccess(collector, rest...);
}

#define DEFINE_RG_TEXTURE_ACCESS(name, state) \
    Moer::Render::RGTextureStaticAccess<state> name {}
#define DEFINE_RG_BUFFER_ACCESS(name, state) \
    Moer::Render::RGBufferStaticAccess<state> name {}
#define DEFINE_RG_NESTED_PARAMETER(type, name) \
    type name {}
#define DEFINE_RG_TEXTURE_ACCESS_ARRAY(name) \
    Moer::Render::RGTextureAccessArray* name { \
        nullptr                              \
    }
#define DEFINE_RG_BUFFER_ACCESS_ARRAY(name) \
    Moer::Render::RGBufferAccessArray* name { \
        nullptr                             \
    }
#define DEFINE_RG_PARAMETER_ACCESS(...)                                               \
    void DeclareRGAccess(Moer::Render::RGParameterAccessCollector& collector) const { \
        Moer::Render::CollectRGParameterAccess(collector __VA_OPT__(, ) __VA_ARGS__); \
    }

enum class ERGPassExecutionMode : uint8_t {
    Serial,
    Parallel
};

struct RGPass {
    static constexpr uint32_t invalid_pass = std::numeric_limits<uint32_t>::max();
    static constexpr size_t   queue_count  = static_cast<size_t>(Render::EQueueType::Num);

    using ParallelExecute = std::function<void(RHICommandList& cmd_list, RGContext context)>;
    using SerialExecute   = std::function<void(RGContext context)>;

    struct CompileInfo {
        uint32_t                                 last_pass{invalid_pass};
        Moer::StaticArray<uint32_t, queue_count> last_pass_by_queue{};
        Moer::StaticArray<uint32_t, queue_count> next_pass_by_queue{};

        void Reset() {
            last_pass = invalid_pass;
            last_pass_by_queue.fill(invalid_pass);
            next_pass_by_queue.fill(invalid_pass);
        }
    };

    String                       name{};
    void*                        parameters{nullptr};
    std::type_index              parameter_type{typeid(void)};
    uint32_t                     parameter_size{0};
    ERGPassFlags                 flags{ERGPassFlags::None};
    ERGPassExecutionMode         execution_mode{ERGPassExecutionMode::Parallel};
    uint32_t                     workload{1};
    ParallelExecute              parallel_execute{};
    SerialExecute                serial_execute{};
    Moer::Array<RGTextureAccess> texture_accesses{};
    Moer::Array<RGBufferAccess>  buffer_accesses{};
    CompileInfo                  compile{};
};

struct RGSetupPass {
    String name{};
    // Empty callbacks are legal no-op slots for future compiled setup scheduling.
    std::function<void(RGSetupContext& setup)> execute{};
};

} // namespace Moer::Render
