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
#include "DepdencyGraph.h"
#include <cstdint>
#include <limits>
namespace Moer {

    class PassNode;

    class RenderGraph;

    // A BlackBoard is a place to store data that is shared between passes
    class RENDER_API BlackBoard {
    public:
        RenderGraphTexture* GetTexture(const std::string& name) const;
        RenderGraphBuffer*  GetBuffer(const std::string& name) const;
        RenderGraphHandle   GetHandle(const std::string& _name) const;
        Moer::Array<RenderGraphHandle>
             GetHandles(const Moer::Array<std::string>& names) const;
        void PutHandle(const std::string& name, RenderGraphHandle handle);
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
            Builder& ReadTexture(RenderGraphHandle         _input,
                                 RenderGraphTexture::Usage _usage =
                                     RenderGraphTexture::Usage::INPUT_ATTACHMENT,
                                 uint32_t _mip_level = 0,
                                 uint32_t _mip_cnt   = 1);

            Builder& WriteTexture(RenderGraphHandle         _output,
                                  RenderGraphTexture::Usage _usage     = RenderGraphTexture::Usage::COLOR_ATTACHMENT,
                                  uint32_t                  _mip_level = 0,
                                  uint32_t                  _mip_cnt   = 1);

            Builder& ReadTextures(const Moer::Array<RenderGraphHandle>& inputs,
                                  RenderGraphTexture::Usage             usage =
                                      RenderGraphTexture::Usage::INPUT_ATTACHMENT);

            Builder& WriteTextures(const Moer::Array<RenderGraphHandle>& output,
                                   RenderGraphTexture::Usage             usage =
                                       RenderGraphTexture::Usage::COLOR_ATTACHMENT);

            Builder&
            ReadBuffer(RenderGraphHandle        input,
                       RenderGraphBuffer::Usage usage = RenderGraphBuffer::Usage::READ);

            Builder& WriteBuffer(
                RenderGraphHandle        output,
                RenderGraphBuffer::Usage usage = RenderGraphBuffer::Usage::WRITE);

            Builder& ReadBuffers(
                const Moer::Array<RenderGraphHandle>& inputs,
                RenderGraphBuffer::Usage              usage = RenderGraphBuffer::Usage::READ);

            Builder& WriteBuffers(
                const Moer::Array<RenderGraphHandle>& output,
                RenderGraphBuffer::Usage              usage = RenderGraphBuffer::Usage::WRITE);

            void DeclareRenderPass(const RenderGraphPassDescriptor& descriptor);
            void DeclareComputePass(const ComputePassDescriptor& descriptor);
            Builder(PassNode* pass, RenderGraph& renderGraph);

        protected:
            PassNode*    m_pass{nullptr};
            RenderGraph& m_renderGraph;
        };
        RenderGraph();

        RenderGraph& operator=(const RenderGraph& other) = delete;
        void         Reset();
        RenderGraphHandle
                          CreateTexture(const std::string&                    name,
                                        const RenderGraphTexture::Descriptor& descriptor);
        RenderGraphHandle ImportTexture(const std::string& name,
                                        RHITextureRef      texture);
        template<typename TResourceRef>
            requires std::is_same_v<TResourceRef, RHITextureRef> ||
                     std::is_same_v<TResourceRef, RHIBufferRef> ||
                     std::is_constructible_v<RHITextureRef, TResourceRef> ||
                     std::is_constructible_v<RHIBufferRef, TResourceRef>
        RenderGraphHandle ImportIfNotExist(const std::string& _name,
                                           TResourceRef       _resource) {
            if (auto handle = GetBlackBoard().GetHandle(_name);
                handle.IsInitialized()) {
                return handle;
            }
            RenderGraphHandle handle;

            using T = std::decay_t<decltype(_resource)>;
            if constexpr (std::is_constructible_v<RHITextureRef, T>) {
                handle = ImportTexture(_name, RHITextureRef(_resource));
            } else if constexpr (std::is_constructible_v<RHIBufferRef, T>) {
                handle = ImportBuffer(_name, RHIBufferRef(_resource));
            }

            return handle;
        }
        RenderGraphHandle
                          CreateBuffer(const std::string&                   name,
                                       const RenderGraphBuffer::Descriptor& descriptor);
        RenderGraphHandle ImportBuffer(const std::string& name, RHIBufferRef buffer);
        RenderGraphHandle
        CreateTextureSubResource(RenderGraphHandle parent, const std::string& name, const RHISubresourceRange& sub_resource);

        using GraphicSetup    = std::function<void(Builder& builder)>;
        using ComputeSetUp    = std::function<void(Builder& builder)>;
        using RayTracingSetup = std::function<void(Builder& builder)>;
        using CopySetup       = std::function<void(Builder& _builder)>;

        void AddGraphicPass(const std::string& name, const GraphicSetup& setup, GraphicsExecute&& execute);
        void AddComputePass(const std::string& name, const ComputeSetUp& setup, ComputeExecute&& execute);
        void AddRayTracingPass(const std::string& name, const RayTracingSetup& setup, RaytracingExecute&& execute);
        void AddCopyPass(std::string_view _name, const CopySetup& _setup, CopyExecute&& _execute);
        void Execute(const RenderGraphExecuteConfig& config);
        void Compile();
        void SetCutUnUsedResources(bool cut);

        BlackBoard&               GetBlackBoard();
        bool                      IsWriteResource(RenderGraphHandle handle, PassNode* node) const;
        bool                      IsReadResource(RenderGraphHandle handle, PassNode* node) const;
        RenderGraphTexture*       GetTexture(RenderGraphHandle handle) const;
        RenderGraphBuffer*        GetBuffer(RenderGraphHandle handle) const;
        RenderGraphResource::Type GetResourceType(RenderGraphHandle handle) const;
        RenderGraph&              SetGraphOutput(RenderGraphHandle handle);
        Extent3D                  GetRenderExtent() const;
        ~RenderGraph();

    protected:
        bool                 IsNeedCompile() const;
        void                 WriteInternal(PassNode* pass, RenderGraphHandle output, DepdencyGraph::ResourceDesc&& _desc);
        void                 ReadInternal(PassNode* pass, RenderGraphHandle input, DepdencyGraph::ResourceDesc&& _desc);
        RenderGraphHandle    AddTextureInternal(RenderGraphTexture* texture);
        RenderGraphHandle    AddBufferInternal(RenderGraphBuffer* buffer);
        RenderGraphResource* GetResource(RenderGraphHandle handle) const;

        Moer::Array<RenderGraphResource*> m_resources;
        Moer::Array<PassNode*>            m_passes;

        Moer::Array<RenderGraphResource*> m_last_resources;
        Moer::Array<PassNode*>            m_last_passes;

        DepdencyGraph m_dependency_graph;
        BlackBoard    m_black_board;
        // Extent3D                         m_render_extent;
        friend class Builder;
    };
}// namespace Moer

#endif// !MOER_ENGINE_RENDER_GRAPH