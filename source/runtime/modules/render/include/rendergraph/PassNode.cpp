#include "PassNode.h"
#include "RenderGraph.h"
#include "rhi/RHICommand.h"
namespace  Moer {
    void PassNode::ResloveResourceUsage() {
    }
    void PassNode::AddResourceUsage(RenderGraphResource* resource, uint32_t usage) {
    }
    void PassNode::AddResourceToCreate(RenderGraphResource* resource) {
    }
    void PassNode::AddResourceToDestroy(RenderGraphResource* resource) {
    }
    Moer::Array<RenderGraphResource*>& PassNode::GetResourcesToCreate() {
    }
    Moer::Array<RenderGraphResource*>& PassNode::GetResourcesToDestroy() {
    }
    GraphicsPassNode::GraphicsPassNode(const std::string_view& passName, RenderGraphPassBase* pass) {
    }
    void GraphicsPassNode::Execute(RenderGraph & render_graph) {
        RHIGraphicsCommandList * cmd_list = nullptr;

        RHIRenderPassInfo pass_info;

        uint32_t color_attachment_idx = 0;
        for(auto & pass_attachment : m_renderPassData.m_descriptor.color_attachments) {
            
            auto & color_attachment =  pass_info.color_attachments[color_attachment_idx];
            auto is_write = render_graph.IsWriteResource(pass_attachment, this);
            auto is_read = render_graph.IsReadResource(pass_attachment, this);

            EAttachmentAction action = EAttachmentAction::AC_LOAD_STORE;
            if(is_write && !is_read) action = AC_CLEAR_STORE;
            pass_info.color_attachments[color_attachment_idx].color_attachment_action = action;

            RenderGraphTexture * texture = render_graph.GetTexture(pass_attachment);
            color_attachment.color_attachment_view.texture_view = texture->GetUAV();
            
            //todo resolve layout
            color_attachment.color_attachment_view.required_layout = TEXTURE_LAYOUT_COLOR_ATTACHMENT;
            color_attachment.color_attachment_view.clear_attachment = RHIClearAttachment(EClearAttachment::COLOR);
            
            color_attachment_idx++;
        }

        {
            auto& depth_attachment_view        = pass_info.depth_stencil_attachment.depth_stencil_attachment_view;
            auto depth_texture = render_graph.GetTexture(m_renderPassData.m_descriptor.depth_stencil_attachment);
            depth_attachment_view.texture_view = depth_texture->GetUAV();
            depth_attachment_view.required_layout = TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE;
            depth_attachment_view.clear_attachment = RHIClearAttachment(EClearAttachment::DEPTH_STENCIL);
        }

        Extent3D extent = {0, 0, 0}  ;
        pass_info.render_area.extent = {extent.width, extent.height};
        pass_info.render_area.offset = {0, 0};

        cmd_list->BeginRenderPass(pass_info,GetName().data());
        
        ViewPort                   viewport{0, 0, float(extent.x), float(extent.y), 0, 1};
        cmd_list->SetViewPort(viewport);
        cmd_list->SetScissor({0, 0, uint32_t(extent.x), uint32_t(extent.y)});
        
        m_pass->Execute();
        cmd_list->EndRenderPass();
        // auto psss_context =  m_pass->getData();
    }
    
}