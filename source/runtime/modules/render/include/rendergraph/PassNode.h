#pragma once
#include "DepdencyGraph.h"
#include "RenderGraphPass.h"
#include "RenderGraphResource.h"
#include "misc/STL.h"

namespace Moer {
    class RenderGraph;

    struct RenderGraphPassDescriptor {
        Moer::Array<RenderGraphHandle> color_attachments;
        RenderGraphHandle              depth_stencil_attachment;
    };

    struct ComputePassDescriptor {
        RHIComputePipelineStateRef compute_pipeline;
    };

    class PassNode : public DepdencyGraph::Node {
    public:
        virtual void                       Execute(RenderPassContext& pass_context) = 0;
        void                               ResloveResourceUsage(RHIGraphicsCommandList* cmd_list);
        void                               AddResourceUsage(RenderGraphResource* resource, uint32_t usage);
        void                               AddResourceToCreate(RenderGraphResource* resource);
        void                               AddResourceToDestroy(RenderGraphResource* resource);
        Moer::Array<RenderGraphResource*>& GetResourcesToCreate();
        Moer::Array<RenderGraphResource*>& GetResourcesToDestroy();
        PassNode(const std::string& name) : Node(name) {}
        EPassType GetPassType() const { return m_pass_type; }

    protected:
        Moer::Map<RenderGraphResource*, uint32_t> m_resource_usage;
        Moer::Map<RenderGraphResource*, uint32_t> m_resource_layout;
        Moer::Array<RenderGraphResource*>         m_resources_to_create;
        Array<RenderGraphResource*>               m_resources_to_destroy;
        EPassType                                 m_pass_type;
    };

    class GraphicsPassNode : public PassNode {
    public:
        GraphicsPassNode(const std::string& passName, RenderGraphPass* pass);
        void Execute(RenderPassContext& pass_context) override;
        void DeclareRenderPass(const RenderGraphPassDescriptor& descriptor);
        ~GraphicsPassNode();

    protected:
        RenderGraphPass* m_pass;
        class RenderPassData {
            RenderGraphPassDescriptor m_descriptor;
            friend class GraphicsPassNode;
        };
        RenderPassData m_renderPassData;
    };

    class ComputePassNode : public PassNode {
    public:
        ComputePassNode(const std::string& passName, RenderGraphPass* pass);
        void Execute(RenderPassContext& pass_context) override;
        ~ComputePassNode();
        void DeclareComputePass(const ComputePassDescriptor& descriptor);

    protected:
        RenderGraphPass*      m_pass;
        ComputePassDescriptor m_descriptor;
    };

    class RaytracingPassNode : public PassNode {
    };
}// namespace Moer
