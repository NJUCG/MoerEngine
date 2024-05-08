#include "rendergraph/PassNode.h"
#include "rendergraph/DepdencyGraph.h"
#include "rendergraph/RenderGraph.h"
#include "rhi/RHICommand.h"
#include <algorithm>
namespace Moer {
    void PassNode::ResloveResourceUsage(RHIGraphicsCommandList* cmd_list) {
        RHIBarrierDependencyInfo barrier_info;
        for (auto& resource_usages : m_resource_desc) {

            for (auto& desc : resource_usages.second) {
                resource_usages.first->ResloveResourceUsage(desc, barrier_info, m_pass_type);
            }
        }
        cmd_list->SetPipelineBarrier(barrier_info);
        //Todo Handle  resource transition
    }
    void PassNode::AddResourceUsage(RenderGraphResource* _resource, DepdencyGraph::ResourceDesc _desc) {
        m_resource_desc[_resource].emplace_back(_desc);
    }
    void PassNode::AddResourceToCreate(RenderGraphResource* resource) {
        m_resources_to_create.push_back(resource);
    }
    void PassNode::AddResourceToDestroy(RenderGraphResource* resource) {
        m_resources_to_destroy.push_back(resource);
    }
    Moer::Array<RenderGraphResource*>& PassNode::GetResourcesToCreate() {
        return m_resources_to_create;
    }
    Moer::Array<RenderGraphResource*>& PassNode::GetResourcesToDestroy() {
        return m_resources_to_destroy;
    }
    GraphicsPassNode::GraphicsPassNode(const std::string& passName, RenderGraphPass* pass) : PassNode(passName), m_pass(pass) {
        m_pass_type = EPassType::Graphics;
    }
    void GraphicsPassNode::Execute(RenderPassContext& pass_context) {
        RHIGraphicsCommandList* cmd_list     = pass_context.cmd_list;
        auto&                   render_graph = pass_context.graph;

        RHIRenderPassInfo pass_info;

        {
            uint32_t color_attachment_idx = 0;
            // pass_info.color_attachments.resize(m_renderPassData.m_descriptor.color_attachments.size());
            for (auto& pass_attachment : m_renderPassData.m_descriptor.color_attachments) {

                auto& color_attachment = pass_info.color_attachments[color_attachment_idx];
                auto  is_write         = render_graph.IsWriteResource(pass_attachment, this);
                auto  is_read          = render_graph.IsReadResource(pass_attachment, this);

                EAttachmentAction action = EAttachmentAction::AC_LOAD_STORE;
                if (is_write && !is_read) action = AC_CLEAR_STORE;
                pass_info.color_attachments[color_attachment_idx].color_attachment_action = action;

                RenderGraphTexture* texture                         = render_graph.GetTexture(pass_attachment);
                color_attachment.color_attachment_view.texture_view = texture->GetUAV();

                //todo resolve layout
                color_attachment.color_attachment_view.required_layout  = m_resource_layout.contains(texture) ? static_cast<ETextureLayout>(m_resource_layout[texture]) : TEXTURE_LAYOUT_COLOR_ATTACHMENT;
                color_attachment.color_attachment_view.clear_attachment = RHIClearAttachment(EClearAttachment::COLOR);

                color_attachment_idx++;
            }
        }

        if (m_renderPassData.m_descriptor.depth_stencil_attachment.IsInitialized()) {
            auto  depth_attachment                 = m_renderPassData.m_descriptor.depth_stencil_attachment;
            auto& depth_attachment_view            = pass_info.depth_stencil_attachment.depth_stencil_attachment_view;
            auto  depth_texture                    = render_graph.GetTexture(m_renderPassData.m_descriptor.depth_stencil_attachment);
            depth_attachment_view.texture_view     = depth_texture->GetUAV();
            depth_attachment_view.required_layout  = TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE;
            depth_attachment_view.clear_attachment = RHIClearAttachment(EClearAttachment::DEPTH_STENCIL);
            auto is_write                          = render_graph.IsWriteResource(depth_attachment, this);
            auto is_read                           = render_graph.IsReadResource(depth_attachment, this);
            depth_attachment_view.required_layout  = m_resource_layout.contains(depth_texture) ? static_cast<ETextureLayout>(m_resource_layout[depth_texture]) : TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE;
            depth_attachment_view.clear_attachment = RHIClearAttachment(EClearAttachment::DEPTH_STENCIL);
            EAttachmentAction action               = EAttachmentAction::AC_LOAD_STORE;
            if (is_write && !is_read) action = AC_CLEAR_STORE;
            pass_info.depth_stencil_attachment.depth_stencil_action = action;
        }

        Extent3D extent              = pass_context.render_extent;
        pass_info.render_area.extent = {extent.width, extent.height};
        pass_info.render_area.offset = {0, 0};

        cmd_list->BeginRenderPass(pass_info, GetName().data());

        ViewPort viewport{0, 0, float(extent.x), float(extent.y), 0, 1};
        cmd_list->SetViewPort(viewport);
        cmd_list->SetScissor({0, 0, uint32_t(extent.x), uint32_t(extent.y)});

        m_pass->Execute(pass_context);
        cmd_list->EndRenderPass();
        // auto psss_context =  m_pass->getData();
    }

    void GraphicsPassNode::DeclareRenderPass(const RenderGraphPassDescriptor& descriptor) {
        m_renderPassData.m_descriptor = descriptor;
    }
    GraphicsPassNode::~GraphicsPassNode() {
        MoerDelete(m_pass);
    }
    ComputePassNode::~ComputePassNode() {
        MoerDelete(m_pass);
    }
    void ComputePassNode::DeclareComputePass(const ComputePassDescriptor& descriptor) {
        m_descriptor = descriptor;
    }

    ComputePassNode::ComputePassNode(const std::string& passName, RenderGraphPass* pass) : PassNode(passName), m_pass(pass) {
        m_pass_type = EPassType::Compute;
    }
    void ComputePassNode::Execute(RenderPassContext& pass_context) {
        pass_context.cmd_list->BeginLabel(GetName().data());
        if (m_descriptor.compute_pipeline)
            pass_context.cmd_list->SetPipelineState(m_descriptor.compute_pipeline);
        m_pass->Execute(pass_context);
        pass_context.cmd_list->EndLabel();
    }

    void CopyPassNode::Execute(RenderPassContext& _pass_context) {
        if (_pass_context.graph.GetResourceType(src) == RenderGraphResource::Type::Buffer) {
            const auto &buffer_info = std::get<BufferCopy>(copy_info);
            auto              src_rhi_buffer = _pass_context.graph.GetBuffer(src)->GetBuffer();
            auto              dst_rhi_buffer = _pass_context.graph.GetBuffer(dst)->GetBuffer();
            RHICopyBufferInfo copy_info;
            copy_info.regions.resize(1);
            RHIBufferRegion& region = copy_info.regions[0];
            region.src_offset       = buffer_info.src_offset;
            region.dst_offset       = buffer_info.dst_offset;
            region.size             = buffer_info.size == 0 ? src_rhi_buffer->GetByteSize() : buffer_info.size;
            _pass_context.cmd_list->CopyBuffer(copy_info, src_rhi_buffer, dst_rhi_buffer);
        } else {
            auto                src_rhi_texture = _pass_context.graph.GetTexture(src)->GetTexture();
            auto                dst_rhi_texture = _pass_context.graph.GetTexture(dst)->GetTexture();
            RHIBlitTextureInfo  blit_info;
            //todo form imageinfo
            RHISubresourceRange src_range(ETextureAspectFlags::COLOR, 0, 1, 0, 1, 0, 1);
            RHISubresourceRange dst_range(ETextureAspectFlags::COLOR, 0, 1, 0, 1, 0, 1);
            blit_info.src_slice  = RHISubresourceSlice(ETextureAspectFlags::COLOR, 0, 0, 1, 0, 1);
            blit_info.dst_slice  = RHISubresourceSlice(ETextureAspectFlags::COLOR, 0, 0, 1, 0, 1);
            blit_info.src_layout = src_rhi_texture->GetLayout(src_range);
            blit_info.dst_layout = dst_rhi_texture->GetLayout(dst_range);
            auto     dst_extent  = dst_rhi_texture->GetExtent3D();
            auto     src_extent  = src_rhi_texture->GetExtent3D();
            Offset3D zero_offset(0, 0, 0);
            blit_info.src_offsets[0] = zero_offset;
            blit_info.src_offsets[1] = Offset3D(src_extent.x, src_extent.y, 1);
            blit_info.dst_offsets[0] = zero_offset;
            blit_info.dst_offsets[1] = Offset3D(dst_extent.x, dst_extent.y, 1);

            _pass_context.cmd_list->BlitTexture(blit_info, src_rhi_texture, dst_rhi_texture);
        }
    }

    CopyPassNode::CopyPassNode(const std::string& _pass_name, RenderGraphHandle _src, RenderGraphHandle _dst, BufferCopy info):PassNode(_pass_name),src(_src),dst(_dst),copy_info(info) {
    }
    CopyPassNode::CopyPassNode(const std::string& _pass_name, RenderGraphHandle _src, RenderGraphHandle _dst, ImageCopy info):PassNode(_pass_name),src(_src),dst(_dst),copy_info(info) {
    }
}// namespace Moer