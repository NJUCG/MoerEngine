#include "rendergraph/RenderGraph.h"

#include "log/LogSystem.h"
#include "rendergraph/PassNode.h"
namespace Moer {
    RenderGraph::Builder& RenderGraph::Builder::readTexture(RenderGraphHandle input, RenderGraphTexture::Usage usage) {
        m_renderGraph.ReadInternal(m_pass, input, usage);
    }
    RenderGraph::Builder& RenderGraph::Builder::writeTexture(RenderGraphHandle output, RenderGraphTexture::Usage usage) {
        m_renderGraph.WriteInternal(m_pass, output, usage);
    }
    RenderGraph::Builder& RenderGraph::Builder::readTextures(const std::vector<RenderGraphHandle>& inputs, RenderGraphTexture::Usage usage) {
    }
    RenderGraph::Builder& RenderGraph::Builder::writeTextures(const std::vector<RenderGraphHandle>& output, RenderGraphTexture::Usage usage) {
    }
    RenderGraphHandle RenderGraph::Builder::readBuffer(RenderGraphHandle input, RenderGraphBuffer::Usage usage) {
    }
    RenderGraphHandle RenderGraph::Builder::writeBuffer(RenderGraphHandle output, RenderGraphBuffer::Usage usage) {
    }
    RenderGraphHandle RenderGraph::CreateTexture(const std::string_view& name, const RenderGraphTexture::Descriptor& descriptor) {
    }
    RenderGraphHandle RenderGraph::ImportTexture(const std::string_view& name, RHITextureRef texture) {
    }
    RenderGraphHandle RenderGraph::CreateBuffer(const std::string_view& name, const RenderGraphBuffer::Descriptor& descriptor) {
    }
    RenderGraphHandle RenderGraph::ImportBuffer(const std::string_view& name, RHIBufferRef buffer) {
    }
    void RenderGraph::AddGraphicPass(const std::string_view& name, const GraphicSetup& setup, GraphicsExecute&& execute) {
        GraphicRenderGraphPass* pass = new GraphicRenderGraphPass(std::move(execute));
        auto                    node = new GraphicsPassNode(name, pass);
        m_passes.emplace_back(node);
        Builder builder(node, *this);
        setup(builder, pass->getData());
    }
    void RenderGraph::AddComputePass(const std::string_view& name, const ComputeSetUp& setup, ComputeExecute&& execute) {
    }
    void RenderGraph::AddRayTracingPass(const std::string_view& name, const RayTracingSetup& setup, RaytracingExecute&& execute) {
    }
    void RenderGraph::Execute() {
        for (auto& pass : m_passes) {
            for (auto& resource : pass->GetResourcesToCreate()) {
                resource->Create();
            }
            pass->ResloveResourceUsage();
            pass->Execute();
            for (auto& resource : pass->GetResourcesToDestroy()) {
                resource->Destroy();
            }
        }
    }
    void RenderGraph::Compile() {
        m_dependency_graph.Cull();

        Moer::Array<PassNode*> available_passes;
        for (auto& pass : m_passes) {
            if (!pass->IsCulled()) {
                available_passes.emplace_back(pass);
            }
        }

        auto       first = available_passes.begin();
        const auto last  = available_passes.end();

        while (first != last) {
            PassNode* const passNode = *first;
            first++;
            auto inResources  = m_dependency_graph.GetInComingNodes(passNode);
            auto outResources = m_dependency_graph.GetOutGoingNodes(passNode);

            for (const auto inResource : inResources) {
                const auto resource = dynamic_cast<RenderGraphResource*>(inResource);
                //Currently not suupport pass connect
                assert(resource);

                resource->create_pass  = resource->create_pass ? resource->create_pass : passNode;
                resource->destroy_pass = passNode;
                //   passNode->addTextureUsage(static_cast<const RenderGraphTexture*>(inResource), );
            }
            for (const auto outResource : outResources) {
                const auto resource = dynamic_cast<RenderGraphResource*>(outResource);
                assert(resource);
                resource->create_pass  = resource->create_pass ? resource->create_pass : passNode;
                resource->destroy_pass = passNode;
                //passNode->addTextureUsage(static_cast<const RenderGraphTexture*>(inResource), texture->usage);
            }

            for (const auto edge : m_dependency_graph.getEdges(passNode)) {
                auto resource = dynamic_cast<RenderGraphResource*>(edge->src == passNode ? edge->dst : edge->src);
                passNode->AddResourceUsage(resource, edge->usage);
            }
        }

        for (const auto& resource : m_resources) {
            if (resource->GetRefCount() != 0) {
                if (resource->create_pass)
                    resource->create_pass->AddResourceToCreate(resource);
                if (resource->destroy_pass)
                    resource->destroy_pass->AddResourceToDestroy(resource);
            } else {
                LOG_INFO("Resource {0} is not used", resource->GetName());
            }
        }
    }
    void RenderGraph::WriteInternal(PassNode* pass, RenderGraphHandle output, RenderGraphTexture::Usage usage) {
        GetResource(output)->ConnectForWrite(m_dependency_graph, pass, static_cast<uint32_t>(usage));
    }
    void RenderGraph::ReadInternal(PassNode* pass, RenderGraphHandle input, RenderGraphTexture::Usage usage) {
        GetResource(input)->ConnectForRead(m_dependency_graph, pass, static_cast<uint32_t>(usage));
    }
    RenderGraphHandle RenderGraph::AddTextureInternal(RenderGraphTexture* texture) {
        m_dependency_graph.RegisterNode(texture);
        const RenderGraphHandle handle(m_resources.size());
        m_resources.emplace_back(texture);
        return handle;
    }
    RenderGraphHandle RenderGraph::AddBufferInternal(RenderGraphBuffer* buffer) {
        m_dependency_graph.RegisterNode(buffer);
        const RenderGraphHandle handle(m_resources.size());
        m_resources.emplace_back(buffer);
        return handle;
    }
    RenderGraphResource* RenderGraph::GetResource(RenderGraphHandle handle) const {
        return m_resources[handle.index];
    }
}