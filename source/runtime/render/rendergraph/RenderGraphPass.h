#pragma once
#include "DepdencyGraph.h"
#include "rhi/RHICommand.h"

#include <functional>

namespace Moer {
class RenderGraph;
// class RHIGraphicsCommandList;
enum class RENDER_GRAPH_PASS_TYPE : uint8_t {
    UNDEFINED  = 0,
    GRAPHICS   = 1,
    COMPUTE    = 1 << 1,
    RAYTRACING = 1 << 2,
    ALL        = GRAPHICS | COMPUTE | RAYTRACING,
};

// struct RenderPassSettings {
//     RENDER_GRAPH_PASS_TYPE type = RENDER_GRAPH_PASS_TYPE::UNDEFINED;
// };

struct RenderPassContext {
    RenderGraph&            graph;
    RHIGraphicsCommandList* cmd_list;
    Extent3D                render_extent;
    EPassType               pass_type;
};

// class RENDER_API {
//
// };

using GraphicsExecute   = std::function<void(RenderPassContext& _context)>;
using ComputeExecute    = std::function<void(RenderPassContext& _context)>;
using RaytracingExecute = std::function<void(RenderPassContext& _context)>;
using CopyExecute       = std::function<void(RenderPassContext& _context)>;

class RenderGraphPass {
public:
    // virtual void execute() = 0;

    virtual ~RenderGraphPass() = default;

public:
    explicit RenderGraphPass(GraphicsExecute&& _execute) noexcept : m_execute(std::move(_execute)) {}

    void Execute(RenderPassContext& _data) {
        m_execute(_data);
    }

    // RenderPassContext& getData() { return data; }

protected:
    // RenderPassContext data;
    GraphicsExecute m_execute;
};

// template<typename Data, typename Execute>
// class RenderGraphPass : public RenderGraphPassBase {
//     Data    data;
//     Execute mExecute;
//
// public:
//     explicit RenderGraphPass(Execute&& execute) noexcept
//         : mExecute(std::move(execute)) {
//     }
//
//     void execute() override {
//         mExecute();
//     }
//
//     Data& getData() { return data; }
// };

// using GraphicRenderGraphPass    = RenderGraphPass<RenderPassSettings, GraphicsExecute>;
// using ComputeRenderGraphPass    = RenderGraphPass<RenderPassSettings, ComputeExecute>;
// using RaytracingRenderGraphPass = RenderGraphPass<RenderPassSettings, RaytracingExecute>;

// class RENDER_API GraphicsPass : public PassNode {
//
// };
//
// class RENDER_API ComputePass : public PassNode {
// };
//
// class RENDER_API RayTracingPass : public PassNode {
// };
} // namespace Moer