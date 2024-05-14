#include "rendergraph/RenderGraph.h"

#include "log/LogSystem.h"
#include "misc/Timer.h"
#include "rendergraph/DepdencyGraph.h"
#include "rendergraph/PassNode.h"
namespace Moer {
    RenderGraphTexture* BlackBoard::GetTexture(const std::string& name) const {
        return m_renderGraph.GetTexture(GetHandle(name));
    }
    RenderGraphHandle BlackBoard::GetHandle(const std::string& name) const {
        if (m_handles.find(name) == m_handles.end()) {
            return RenderGraphHandle();
        }
        return m_handles.at(name);
    }
    Moer::Array<RenderGraphHandle> BlackBoard::GetHandles(const Moer::Array<std::string>& names) const {
        Moer::Array<RenderGraphHandle> handles;
        for (const auto& name : names) {
            handles.emplace_back(GetHandle(name));
        }
        return handles;
    }
    void BlackBoard::PutHandle(const std::string& name, RenderGraphHandle handle) {
        m_handles.emplace(name, handle);
    }
    BlackBoard::BlackBoard(RenderGraph& renderGraph) : m_renderGraph(renderGraph) {
    }
    RenderGraphBuffer* BlackBoard::GetBuffer(const std::string& name) const {
        return m_renderGraph.GetBuffer(GetHandle(name));
    }
    RenderGraph::Builder& RenderGraph::Builder::ReadTexture(RenderGraphHandle _input, RenderGraphTexture::Usage _usage, uint32_t _mip_level, uint32_t _mip_cnt) {
        DepdencyGraph::ResourceDesc desc = DepdencyGraph::TextureSubDesc{.mip_level = _mip_level, .num_mips = _mip_cnt, .array_index = 0, .array_count = 1, .usage = _usage};
        m_renderGraph.ReadInternal(m_pass, _input, std::move(desc));
        return *this;
    }
    RenderGraph::Builder& RenderGraph::Builder::WriteTexture(RenderGraphHandle _output, RenderGraphTexture::Usage _usage, uint32_t _mip_level) {

        DepdencyGraph::ResourceDesc desc = DepdencyGraph::TextureSubDesc{.mip_level = _mip_level, .num_mips = 1, .array_index = 0, .array_count = 1, .usage = _usage};
        m_renderGraph.WriteInternal(m_pass, _output, std::move(desc));
        return *this;
    }
    RenderGraph::Builder& RenderGraph::Builder::ReadTextures(const Moer::Array<RenderGraphHandle>& inputs, RenderGraphTexture::Usage usage) {
        for (auto input : inputs) {
            DepdencyGraph::ResourceDesc desc = DepdencyGraph::TextureSubDesc{.mip_level = 0, .num_mips = 1, .array_index = 0, .array_count = 1, .usage = usage};
            m_renderGraph.ReadInternal(m_pass, input, std::move(desc));
        }
        return *this;
    }
    RenderGraph::Builder& RenderGraph::Builder::WriteTextures(const Moer::Array<RenderGraphHandle>& output, RenderGraphTexture::Usage usage) {
        for (auto out : output) {
            DepdencyGraph::ResourceDesc desc = DepdencyGraph::TextureSubDesc{.mip_level = 0, .num_mips = 1, .array_index = 0, .array_count = 1, .usage = usage};
            m_renderGraph.WriteInternal(m_pass, out, std::move(desc));
        }
        return *this;
    }
    RenderGraph::Builder& RenderGraph::Builder::ReadBuffer(RenderGraphHandle input, RenderGraphBuffer::Usage usage) {
        DepdencyGraph::ResourceDesc desc = DepdencyGraph::BufferSubDesc{.offset = 0, .size = 0, .layout = usage};
        m_renderGraph.ReadInternal(m_pass, input, std::move(desc));
        return *this;
    }
    RenderGraph::Builder& RenderGraph::Builder::WriteBuffer(RenderGraphHandle output, RenderGraphBuffer::Usage usage) {
        DepdencyGraph::ResourceDesc desc = DepdencyGraph::BufferSubDesc{.offset = 0, .size = 0, .layout = usage};
        m_renderGraph.WriteInternal(m_pass, output, std::move(desc));
        return *this;
    }
    RenderGraph::Builder& RenderGraph::Builder::WriteBuffers(const Moer::Array<RenderGraphHandle>& output, RenderGraphBuffer::Usage usage) {
        for (auto out : output) {
            DepdencyGraph::ResourceDesc desc = DepdencyGraph::BufferSubDesc{.offset = 0, .size = 0, .layout = usage};
            m_renderGraph.WriteInternal(m_pass, out, std::move(desc));
        }
        return *this;
    }
    RenderGraph::Builder& RenderGraph::Builder::ReadBuffers(const Moer::Array<RenderGraphHandle>& _inputs, RenderGraphBuffer::Usage _usage) {
        for (auto input : _inputs) {
            DepdencyGraph::ResourceDesc desc = DepdencyGraph::BufferSubDesc{.offset = 0, .size = 0, .layout = _usage};
            m_renderGraph.ReadInternal(m_pass, input, std::move(desc));
        }
        return *this;
    }
    void RenderGraph::Builder::DeclareRenderPass(const RenderGraphPassDescriptor& descriptor) {
        if (descriptor.depth_stencil_attachment.IsInitialized()) {
            //Depth Attachment may be used in this pass,but not used in later pass
            //to avoid cull depth attachment, we need to add a ref
            m_renderGraph.GetResource(descriptor.depth_stencil_attachment)->AddRef();
        }
        auto pass = static_cast<GraphicsPassNode*>(m_pass);
        pass->DeclareRenderPass(descriptor);
    }
    void RenderGraph::Builder::DeclareComputePass(const ComputePassDescriptor& descriptor) {
        auto pass = static_cast<ComputePassNode*>(m_pass);
        pass->DeclareComputePass(descriptor);
    }
    RenderGraph::Builder::Builder(PassNode* pass, RenderGraph& renderGraph) : m_pass(pass), m_renderGraph(renderGraph) {
    }
    void RenderGraph::SetCutUnUsedResources(bool cut) {
        m_cut = cut;
    }
    RenderGraph::RenderGraph() : m_black_board(*this) {
    }
    void RenderGraph::Reset() {
        m_dependency_graph.Reset();
        m_black_board.Reset();
        for (auto& resource : m_last_resources) {
            MoerDelete(resource);
        }
        for (auto& pass : m_last_passes) {
            MoerDelete(pass);
        }
        m_last_resources = std::move(m_resources);
        m_last_passes    = std::move(m_passes);

        m_resources = {};
        m_passes    = {};
    }
    RenderGraphHandle RenderGraph::CreateTexture(const std::string& name, const RenderGraphTexture::Descriptor& descriptor) {
        RenderGraphTexture* texture = MoerNew(RenderGraphTexture)(name, descriptor);
        return AddTextureInternal(texture);
    }
    RenderGraphHandle RenderGraph::ImportTexture(const std::string& name, RHITextureRef rhi_texture) {
        RenderGraphTexture* texture = MoerNew(RenderGraphTexture)(name, rhi_texture);
        return AddTextureInternal(texture);
    }
    RenderGraphHandle RenderGraph::CreateBuffer(const std::string& name, const RenderGraphBuffer::Descriptor& descriptor) {
        RenderGraphBuffer* buffer = MoerNew(RenderGraphBuffer)(name, descriptor);
        return AddBufferInternal(buffer);
    }
    RenderGraphHandle RenderGraph::ImportBuffer(const std::string& _name, RHIBufferRef _rhi_buffer) {
        RenderGraphBuffer* buffer = MoerNew(RenderGraphBuffer)(_name, _rhi_buffer);
        return AddBufferInternal(buffer);
    }
    RenderGraphHandle RenderGraph::CreateTextureSubResource(RenderGraphHandle parent, const std::string& name, const RHISubresourceRange& sub_resource) {
        if (auto handle = m_black_board.GetHandle(name); handle.IsInitialized()) {
            return handle;
        }
        RenderGraphTexture* parent_texture = GetTexture(parent);
        RenderGraphTexture* texture        = MoerNew(RenderGraphTexture)(name, parent_texture, sub_resource);
        return AddTextureInternal(texture);
    }
    void RenderGraph::AddGraphicPass(const std::string& name, const GraphicSetup& setup, GraphicsExecute&& execute) {
        RenderGraphPass* pass = MoerNew(RenderGraphPass)(std::move(execute));
        auto*            node = MoerNew(GraphicsPassNode)(name, pass);
        m_passes.emplace_back(node);
        Builder builder(node, *this);
        setup(builder);
    }
    void RenderGraph::AddComputePass(const std::string& name, const ComputeSetUp& setup, ComputeExecute&& execute) {
        RenderGraphPass* pass = MoerNew(RenderGraphPass)(std::move(execute));
        auto             node = MoerNew(ComputePassNode)(name, pass);
        m_passes.emplace_back(node);
        Builder builder(node, *this);
        setup(builder);
    }
    void RenderGraph::AddRayTracingPass(const std::string& name, const RayTracingSetup& setup, RaytracingExecute&& execute) {
        //TODO
    }

    void RenderGraph::AddCopyPass(std::string_view _name, const CopySetup& _setup, CopyExecute&& _execute) {
        RenderGraphPass* pass = MoerNew(RenderGraphPass)(std::move(_execute));
        auto*            node = MoerNew(CopyPassNode)(_name, pass);
        m_passes.emplace_back(node);
        Builder builder(node, *this);
        _setup(builder);
    }

    void RenderGraph::Execute(const RenderGraphExecuteConfig& config) {
        static Timer timer;
        timer.Start();
        Compile();
        timer.Stop();
        // LOG_INFO("Compile Time: {0}ms", timer.ElapsedMilliseconds());
        auto* cmd_list = config.cmd_list;
        timer.Start();
        for (auto& pass : m_passes) {
            for (auto& resource : pass->GetResourcesToCreate()) {
                resource->Create();
            }
            pass->ResloveResourceUsage(cmd_list);
            RenderPassContext pass_context{.graph = *this, .cmd_list = cmd_list, .render_extent = config.render_extent, .pass_type = pass->GetPassType()};
            pass->Execute(pass_context);
            for (auto& resource : pass->GetResourcesToDestroy()) {
                resource->Destroy();
            }
        }
        timer.Stop();
        // LOG_INFO("Execute Time: {0}ms", timer.ElapsedMilliseconds());
    }
    void RenderGraph::Compile() {
        bool need_compile = IsNeedCompile();

        if (!need_compile) {
            std::unordered_map<PassNode*, uint32_t> pass_idxes;
            for (size_t i = 0; i < m_last_passes.size(); i++) {
                pass_idxes.emplace(m_last_passes[i], i);
            }
            for (int i = 0; i < m_resources.size(); i++) {
                auto* resource      = m_resources[i];
                auto* last_resource = m_last_resources[i];
                resource->SetRefCount(last_resource->GetRefCount());
                if (last_resource->create_pass) {
                    uint32_t create_pass_idx = pass_idxes[last_resource->create_pass];
                    resource->create_pass    = m_passes[create_pass_idx];
                }
                if (last_resource->destroy_pass) {
                    uint32_t destroy_pass_idx = pass_idxes[last_resource->destroy_pass];
                    resource->destroy_pass    = m_passes[destroy_pass_idx];
                }
            }
            for (int i = 0; i < m_passes.size(); i++) {
                auto* pass      = m_passes[i];
                auto* last_pass = m_last_passes[i];
                pass->SetBarrierInfo(last_pass->GetBarrierInfo());
            }

        }

        else {
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
                PassNode* const pass_node = *first;
                first++;
                auto in_resources  = m_dependency_graph.GetInComingNodes(pass_node);
                auto out_resources = m_dependency_graph.GetOutGoingNodes(pass_node);

                for (auto* const in_resource : in_resources) {
                    auto* const resource = dynamic_cast<RenderGraphResource*>(in_resource);
                    //Currently not suupport pass connect
                    assert(resource);

                    resource->create_pass  = resource->create_pass ? resource->create_pass : pass_node;
                    resource->destroy_pass = pass_node;
                }
                for (auto* const out_resource : out_resources) {
                    auto* const resource = dynamic_cast<RenderGraphResource*>(out_resource);
                    assert(resource);
                    resource->create_pass  = resource->create_pass ? resource->create_pass : pass_node;
                    resource->destroy_pass = pass_node;
                }

                for (auto* const edge : m_dependency_graph.GetEdges(pass_node)) {
                    auto* resource = dynamic_cast<RenderGraphResource*>(edge->src == pass_node ? edge->dst : edge->src);
                    pass_node->AddResourceUsage(resource, edge->desc);
                }
            }
        }

        for (const auto& resource : m_resources) {
            if (!m_cut | !resource->IsCulled()) {
                if (resource->create_pass)
                    resource->create_pass->AddResourceToCreate(resource);
                if (resource->destroy_pass)
                    resource->destroy_pass->AddResourceToDestroy(resource);
            } else {
                LOG_INFO("Resource {0} is not used", resource->GetName());
            }
        }
    }
    BlackBoard& RenderGraph::GetBlackBoard() {
        return m_black_board;
    }
    void BlackBoard::Reset() {
        m_handles.clear();
    }
    bool RenderGraph::IsWriteResource(RenderGraphHandle handle, PassNode* node) const {
        auto* resource = GetResource(handle);
        return m_dependency_graph.IsWriteResource(node, resource);
    }
    bool RenderGraph::IsReadResource(RenderGraphHandle handle, PassNode* node) const {
        auto* resource = GetResource(handle);
        return m_dependency_graph.IsReadResource(node, resource);
    }
    RenderGraphTexture* RenderGraph::GetTexture(RenderGraphHandle handle) const {
        return dynamic_cast<RenderGraphTexture*>(GetResource(handle));
    }
    RenderGraphBuffer* RenderGraph::GetBuffer(RenderGraphHandle handle) const {
        return dynamic_cast<RenderGraphBuffer*>(GetResource(handle));
    }
    RenderGraphResource::Type RenderGraph::GetResourceType(RenderGraphHandle handle) const {
        return GetResource(handle)->GetType();
    }
    RenderGraph& RenderGraph::SetGraphOutput(RenderGraphHandle handle) {
        GetResource(handle)->AddRef();
        return *this;
    }
    RenderGraph::~RenderGraph() {
        for (auto& resource : m_resources) {
            MoerDelete(resource);
        }
        for (auto& pass : m_passes) {
            MoerDelete(pass);
        }
    }
    bool RenderGraph::IsNeedCompile() const {
        if (m_last_passes.size() != m_passes.size() || m_last_resources.size() != m_resources.size()) {
            return true;
        }
        static auto check_pass_is_same = [](const PassNode* a, const PassNode* b) {
            if (a->GetName() != b->GetName()) {
                return false;
            }
            if (a->GetPassType() != b->GetPassType()) {
                return false;
            }
            return true;
        };
        for (size_t i = 0; i < m_passes.size(); i++) {
            if (!check_pass_is_same(m_passes[i], m_last_passes[i])) {
                return true;
            }
        }
        for (size_t i = 0; i < m_resources.size(); i++) {
            if (m_resources[i]->GetName() != m_last_resources[i]->GetName()) {
                return true;
            }
        }
        return false;
    }
    void RenderGraph::WriteInternal(PassNode* pass, RenderGraphHandle output, DepdencyGraph::ResourceDesc&& _desc) {
        GetResource(output)->ConnectForWrite(m_dependency_graph, pass, std::move(_desc));
    }
    void RenderGraph::ReadInternal(PassNode* pass, RenderGraphHandle input, DepdencyGraph::ResourceDesc&& _desc) {
        GetResource(input)->ConnectForRead(m_dependency_graph, pass, std::move(_desc));
    }
    RenderGraphHandle RenderGraph::AddTextureInternal(RenderGraphTexture* texture) {
        m_dependency_graph.RegisterNode(texture);
        const RenderGraphHandle handle(static_cast<RenderGraphHandle::Index>(m_resources.size()));
        m_black_board.PutHandle(texture->GetName(), handle);
        m_resources.emplace_back(texture);
        return handle;
    }
    RenderGraphHandle RenderGraph::AddBufferInternal(RenderGraphBuffer* buffer) {
        m_dependency_graph.RegisterNode(buffer);
        const RenderGraphHandle handle(static_cast<RenderGraphHandle::Index>(m_resources.size()));
        m_black_board.PutHandle(buffer->GetName(), handle);
        m_resources.emplace_back(buffer);
        return handle;
    }
    RenderGraphResource* RenderGraph::GetResource(RenderGraphHandle handle) const {
        return m_resources[handle.index];
    }
}// namespace Moer