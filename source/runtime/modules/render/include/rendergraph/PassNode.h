#pragma once
#include "DepdencyGraph.h"
#include "RenderGraphPass.h"
#include "RenderGraphResource.h"
#include "misc/STL.h"

namespace Moer {
    class RenderGraph;

    struct RenderGraphPassDescriptor {
        Moer::Array<RenderGraphHandle> color_attachments;
        RenderGraphHandle             depth_stencil_attachment;
    };

    
    class PassNode : public DepdencyGraph::Node {
    public:
        virtual void Execute(RenderGraph & render_graph) = 0;
        void ResloveResourceUsage();
        void AddResourceUsage(RenderGraphResource* resource, uint32_t usage);
        void AddResourceToCreate(RenderGraphResource* resource);
        void AddResourceToDestroy(RenderGraphResource* resource);
        Moer::Array<RenderGraphResource*> & GetResourcesToCreate();
        Moer::Array<RenderGraphResource*> & GetResourcesToDestroy();
    protected:
        Moer::Map<RenderGraphResource*, uint32_t> m_resourceUsage;
        Moer::Array<RenderGraphResource*>         m_resources_to_create;
        Array<RenderGraphResource*>               m_resources_to_destroy;
    };

    class GraphicsPassNode : public PassNode {
    public:
        GraphicsPassNode(const std::string_view& passName, RenderGraphPassBase* pass);
        void Execute(RenderGraph & render_graph) override;
    protected:
        RenderGraphPassBase * m_pass;
        class RenderPassData {
            RenderGraphPassDescriptor m_descriptor;
            friend  class GraphicsPassNode;    
        };
        RenderPassData m_renderPassData;
    };

    class ComputePassNode : public PassNode {
    };

    class RaytracingPassNode : public PassNode {
    };
}// namespace Moer
