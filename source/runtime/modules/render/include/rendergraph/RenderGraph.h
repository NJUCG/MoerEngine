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
    class BlackBoard {
    public:
        RenderGraphTexture GetTexture(const std::string_view& name) const;
        RenderGraphBuffer GetBuffer(const std::string_view& name) const;
        RenderGraphHandle GetHandle(const std::string_view& name) const;
        void PutHandle(const std::string_view& name, RenderGraphHandle handle);
        BlackBoard(RenderGraph& renderGraph);
    protected:
        RenderGraph & m_renderGraph;
        Moer::UnorderedMap<std::string,RenderGraphHandle> m_handles;
    };

    class RENDER_API RenderGraph {

    public:
        class Builder {
        public:
            Builder& readTexture(RenderGraphHandle         input,
                                 RenderGraphTexture::Usage usage =
                                     RenderGraphTexture::Usage::INPUT_ATTACHMENT);

            Builder& writeTexture(RenderGraphHandle         output,
                                  RenderGraphTexture::Usage usage =
                                      RenderGraphTexture::Usage::COLOR_ATTACHMENT);

            Builder& readTextures(const std::vector<RenderGraphHandle>& inputs,
                                  RenderGraphTexture::Usage             usage = RenderGraphTexture::Usage::INPUT_ATTACHMENT);

            Builder& writeTextures(const std::vector<RenderGraphHandle>& output,
                                   RenderGraphTexture::Usage             usage =
                                       RenderGraphTexture::Usage::COLOR_ATTACHMENT);

            RenderGraphHandle readBuffer(RenderGraphHandle        input,
                                         RenderGraphBuffer::Usage usage =
                                             RenderGraphBuffer::Usage::SHADER_RESOURCE);

            RenderGraphHandle writeBuffer(RenderGraphHandle        output,
                                          RenderGraphBuffer::Usage usage =
                                              RenderGraphBuffer::Usage::STORAGE_BUFFER);

            void DeclareRenderPass( const RenderGraphPassDescriptor& descriptor);

        protected:
            PassNode*    m_pass{nullptr};
            RenderGraph& m_renderGraph;
        };

        RenderGraph();
        RenderGraphHandle CreateTexture(const std::string_view& name, const RenderGraphTexture::Descriptor& descriptor);
        RenderGraphHandle ImportTexture(const std::string_view& name, RHITextureRef texture);
        RenderGraphHandle CreateBuffer(const std::string_view& name, const RenderGraphBuffer::Descriptor& descriptor);
        RenderGraphHandle ImportBuffer(const std::string_view& name, RHIBufferRef buffer);

        using GraphicSetup    = std::function<void(Builder& builder, RenderPassSettings&)>;
        using ComputeSetUp    = std::function<void(Builder& builder, RenderPassSettings&)>;
        using RayTracingSetup = std::function<void(Builder& builder, RenderPassSettings&)>;

        void AddGraphicPass(const std::string_view& name, const GraphicSetup& setup, GraphicsExecute&& execute);
        void AddComputePass(const std::string_view& name, const ComputeSetUp& setup, ComputeExecute&& execute);
        void AddRayTracingPass(const std::string_view& name, const RayTracingSetup& setup, RaytracingExecute&& execute);
        // void AddPass();
        void Execute();
        void Compile();

        BlackBoard & GetBlackBoard();
        bool IsWriteResource(RenderGraphHandle handle,PassNode * node) const;
        bool IsReadResource(RenderGraphHandle handle,PassNode * node) const;
        RenderGraphTexture*  GetTexture(RenderGraphHandle handle) const;
        RenderGraphBuffer*   GetBuffer(RenderGraphHandle handle) const;

    protected:
        void                 WriteInternal(PassNode* pass, RenderGraphHandle output, RenderGraphTexture::Usage usage);
        void                 ReadInternal(PassNode* pass, RenderGraphHandle input, RenderGraphTexture::Usage usage);
        RenderGraphHandle    AddTextureInternal(RenderGraphTexture* texture);
        RenderGraphHandle    AddBufferInternal(RenderGraphBuffer* buffer);
        RenderGraphResource* GetResource(RenderGraphHandle handle) const;
        

        Moer::Array<RenderGraphResource*> m_resources;
        Moer::Array<PassNode*>            m_passes;
        DepdencyGraph                     m_dependency_graph;
        BlackBoard                        m_blackBoard;
        friend class Builder;
    };
}// namespace Moer

#endif// !MOER_ENGINE_RENDER_GRAPH