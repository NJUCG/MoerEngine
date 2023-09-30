#include "rhi/RHI.h"
RHI* g_rhi = nullptr;

#include "RHICommandList.h"
void Test(){
    g_rhi->Initialize();

    g_rhi->PostInit();

    RHIGraphicsPipelineStateInitializer init;
    init.num_samples = 1;
    init.multi_view_count = 1;
    init.stencil_attachment_store_op = EAttachmentStoreOp::NONE;
    init.stencil_attachment_load_op = EAttachmentLoadOp::NONE;
    init.depth_attachment_store_op = EAttachmentStoreOp::NONE;
    init.depth_attachment_load_op = EAttachmentLoadOp::NONE;
    init.color_attachment_count = 1;
    init.color_attachment_formats[0] = EPixelFormat::PF_R8G8B8A8_SRGB;
    init.color_attachment_flags[0] = ETextureUsageFlags::COLOR_ATTACHMENT;

    RHIShaderBoundState& state = init.shader_stage;
    VertexInputStateInitializerList vertex_init_list;

    state.p_vertex_description;

//    state.p_vertex_shader =

    RHIGraphicsPipelineStateRef pso = g_rhi->RHICreateGraphicsPipelineState(init);

    RHIGraphicsCommandList* command_list = g_rhi->CreateGraphicsCommandList(pso);

    RHIBufferCreateInfo v_info;
    v_info.SetSize(16);
    v_info.SetStride(4);
    v_info.SetUsage(EBufferUsageFlags::VERTEX_BUFFER);
    RHIBufferRef vertex_buffer = g_rhi->RHICreateBuffer(v_info);

    command_list->BindVertexBuffers(0, 1, vertex_buffer);


}