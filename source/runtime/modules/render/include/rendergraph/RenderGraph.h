#ifndef MOER_ENGINE_RENDER_GRAPH
#define MOER_ENGINE_RENDER_GRAPH

#include "DepdencyGraph.h"
#include "PassNode.h"
#include "RenderAPI.h"
#include "RenderGraphHandle.h"
#include "RenderGraphPass.h"
#include "RenderGraphResource.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

#include <cstdint>
#include <limits>
namespace Moer {

    class PassNode;

    class RenderGraph;

    //A BlackBoard is a place to store data that is shared between passes
    class RENDER_API BlackBoard {
    public:
        RenderGraphTexture* GetTexture(const std::string& name) const;
        RenderGraphBuffer*  GetBuffer(const std::string& name) const;
        RenderGraphHandle   GetHandle(const std::string& name) const;
        void                PutHandle(const std::string& name, RenderGraphHandle handle);
        BlackBoard(RenderGraph& renderGraph);
        ~BlackBoard() = default;
        void Reset();

    protected:
        RenderGraph&                                       m_renderGraph;
        Moer::UnorderedMap<std::string, RenderGraphHandle> m_handles;
    };

    struct RenderGraphExecuteConfig {
        RHIGraphicsCommandList* cmd_list{nullptr};
        Extent3D                render_extent{};
    };

    class RENDER_API RenderGraph {

    public:
        class Builder {
        public:
            Builder& ReadTexture(RenderGraphHandle         input,
                                 RenderGraphTexture::Usage usage =
                                     RenderGraphTexture::Usage::INPUT_ATTACHMENT);

            Builder& WriteTexture(RenderGraphHandle         output,
                                  RenderGraphTexture::Usage usage =
                                      RenderGraphTexture::Usage::COLOR_ATTACHMENT);

            Builder& ReadTextures(const std::vector<RenderGraphHandle>& inputs,
                                  RenderGraphTexture::Usage             usage = RenderGraphTexture::Usage::INPUT_ATTACHMENT);

            Builder& WriteTextures(const std::vector<RenderGraphHandle>& output,
                                   RenderGraphTexture::Usage             usage =
                                       RenderGraphTexture::Usage::COLOR_ATTACHMENT);

            Builder& ReadBuffer(RenderGraphHandle        input,
                                RenderGraphBuffer::Usage usage =
                                    RenderGraphBuffer::Usage::SHADER_RESOURCE);

            Builder& WriteBuffer(RenderGraphHandle        output,
                                 RenderGraphBuffer::Usage usage =
                                     RenderGraphBuffer::Usage::STORAGE_BUFFER);

            void DeclareRenderPass(const RenderGraphPassDescriptor& descriptor);
            Builder(PassNode* pass, RenderGraph& renderGraph);

        protected:
            PassNode*    m_pass{nullptr};
            RenderGraph& m_renderGraph;
        };
        RenderGraph();
        void              Reset();
        RenderGraphHandle CreateTexture(const std::string& name, const RenderGraphTexture::Descriptor& descriptor);
        RenderGraphHandle ImportTexture(const std::string& name, RHITextureRef texture);
        RenderGraphHandle CreateBuffer(const std::string& name, const RenderGraphBuffer::Descriptor& descriptor);
        RenderGraphHandle ImportBuffer(const std::string& name, RHIBufferRef buffer);

        using GraphicSetup    = std::function<void(Builder& builder)>;
        using ComputeSetUp    = std::function<void(Builder& builder)>;
        using RayTracingSetup = std::function<void(Builder& builder)>;

        void AddGraphicPass(const std::string& name, const GraphicSetup& setup, GraphicsExecute&& execute);
        void AddComputePass(const std::string& name, const ComputeSetUp& setup, ComputeExecute&& execute);
        void AddRayTracingPass(const std::string& name, const RayTracingSetup& setup, RaytracingExecute&& execute);
        // void AddPass();
        void Execute(const RenderGraphExecuteConfig& config);
        void Compile();

        BlackBoard&         GetBlackBoard();
        bool                IsWriteResource(RenderGraphHandle handle, PassNode* node) const;
        bool                IsReadResource(RenderGraphHandle handle, PassNode* node) const;
        RenderGraphTexture* GetTexture(RenderGraphHandle handle) const;
        RenderGraphBuffer*  GetBuffer(RenderGraphHandle handle) const;
        void                SetGraphOutput(RenderGraphHandle handle);
        Extent3D            GetRenderExtent() const;
        ~RenderGraph();

    protected:
        void                 WriteInternal(PassNode* pass, RenderGraphHandle output, uint32_t usage);
        void                 ReadInternal(PassNode* pass, RenderGraphHandle input, uint32_t usage);
        RenderGraphHandle    AddTextureInternal(RenderGraphTexture* texture);
        RenderGraphHandle    AddBufferInternal(RenderGraphBuffer* buffer);
        RenderGraphResource* GetResource(RenderGraphHandle handle) const;

        Moer::Array<RenderGraphResource*> m_resources;
        Moer::Array<PassNode*>            m_passes;
        DepdencyGraph                     m_dependency_graph;
        BlackBoard                        m_black_board;
        //Extent3D                         m_render_extent;
        friend class Builder;
    };
}// namespace Moer

#endif// !MOER_ENGINE_RENDER_GRAPH