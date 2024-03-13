#pragma once
#include "DepdencyGraph.h"
#include "rhi/RHICommand.h"

namespace Moer {

    enum class RENDER_GRAPH_PASS_TYPE : uint8_t {
        UNDEFINED  = 0,
        GRAPHICS   = 1,
        COMPUTE    = 1 << 1,
        RAYTRACING = 1 << 2,
        ALL        = GRAPHICS | COMPUTE | RAYTRACING,
    };

    struct RenderPassSettings {
        RENDER_GRAPH_PASS_TYPE type = RENDER_GRAPH_PASS_TYPE::UNDEFINED;
    };

    struct RenderPassContext {
        RHIGraphicsCommandList * cmd_list;
    };

    // class RENDER_API {
    //
    // };

    using GraphicsExecute   = std::function<void(RenderPassContext& context)>;
    using ComputeExecute    = std::function<void(RenderPassContext& context)>;
    using RaytracingExecute = std::function<void(RenderPassContext& context)>;

    class RenderGraphPassBase {
    public:
       // virtual void execute() = 0;
        
        virtual ~RenderGraphPassBase() = default;

        

    public:
        explicit RenderGraphPassBase(GraphicsExecute&& execute) noexcept
            : mExecute(std::move(execute)) {
        }

        void Execute() {
            mExecute(data);
        }

        RenderPassContext & getData() { return data; }
    protected:
        RenderPassContext    data;
        GraphicsExecute mExecute;
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
}