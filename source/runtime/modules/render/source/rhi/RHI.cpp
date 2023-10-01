#include "rhi/RHI.h"
RHI* g_rhi = nullptr;

#include "RHICommandList.h"
#include "RHICommandQueue.h"
RHIBufferRef CreateBufferFromData(const RHIBufferCreateInfo& info, uint32_t size, void* data) {
    RHIBufferRef buffer     = g_rhi->RHICreateBuffer(info);
    void*        mapped_ptr = g_rhi->RHIMapBuffer(buffer, 0, size);
    memcpy(mapped_ptr, data, size);
    g_rhi->RHIUnmapBuffer(buffer);
    return buffer;
}

void Test() {
    g_rhi->Initialize();

    g_rhi->PostInit();

    RHIGraphicsPipelineStateInitializer init;
    init.num_samples                 = 1;
    init.multi_view_count            = 1;
    init.stencil_attachment_store_op = EAttachmentStoreOp::NONE;
    init.stencil_attachment_load_op  = EAttachmentLoadOp::NONE;
    init.depth_attachment_store_op   = EAttachmentStoreOp::NONE;
    init.depth_attachment_load_op    = EAttachmentLoadOp::NONE;
    init.color_attachment_count      = 1;
    init.color_attachment_formats[0] = EPixelFormat::PF_R8G8B8A8_SRGB;
    init.color_attachment_flags[0]   = ETextureUsageFlags::COLOR_ATTACHMENT;
    init.primitive_topology          = EPrimitiveTopology::TRIANGLE_LIST;

    RHIShaderBoundStateInput& shader_state = init.shader_stage;


    VertexInputStateInitializerList vertex_init_list;
    for (int i = 0; i < 1; ++i) {
        vertex_init_list[i].type            = EVertexElementType::VET_FLOAT3;
        vertex_init_list[i].offset          = 0;
        vertex_init_list[i].stride          = sizeof(float3);
        vertex_init_list[i].input_rate      = EVertexInputRate::VIR_VERTEX;
        vertex_init_list[i].attribute_index = 0;
        vertex_init_list[i].binding_index   = 0;
    }

    const uint16_t      indices[] = {1, 2, 3};
    RHIBufferCreateInfo buffer_info;
    buffer_info.SetUsage(EBufferUsageFlags::INDEX_BUFFER)
        .SetStride(sizeof(uint16_t))
        .SetSize(sizeof(indices));

    RHIBufferRef indexBuffer = CreateBufferFromData(buffer_info, sizeof(indices), (void*)indices);

    RHIBufferCreateInfo v_info;
    v_info.SetSize(16).SetStride(4).SetUsage(EBufferUsageFlags::VERTEX_BUFFER);

    const float  vertex_data[] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 1};
    RHIBufferRef vertex_buffer = CreateBufferFromData(v_info, sizeof(vertex_data), (void*)vertex_data);

    shader_state.p_vertex_input_state = g_rhi->RHICreateVertexInputState(vertex_init_list);

    const int2           attachment_size(4, 4);
    RHITextureCreateInfo tex_info;
    tex_info.SetDimension(ETextureDimension::TEX_2D)
        .SetFormat(PF_R8G8B8A8_SRGB)
        .SetInitialLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT)
        .SetExtent(attachment_size)
        .SetClearAttachment(RHIClearAttachment())
        .SetDepth(1)
        .SetArraySize(1)
        .SetNumMips(1)
        .SetNumSamples(1)
        .SetUsageFlags(ETextureUsageFlags::COLOR_ATTACHMENT);

    //out_put texture
    RHITextureRef tex = g_rhi->RHICreateTexture(tex_info);

    RHIGraphicsPipelineStateRef pso = g_rhi->RHICreateGraphicsPipelineState(init);

    RHIGraphicsCommandList* command_list = g_rhi->CreateGraphicsCommandList(pso);

    RHIRenderPassInfo pass_info;
    command_list->BeginRenderPass(pass_info, "triangle pass");
    command_list->BindVertexBuffers(0, 1, vertex_buffer);

    command_list->DrawIndexedInstanced(1, 1, 0, 0);

    command_list->EndRenderPass();

    RHICommandQueue* graphics_queue = g_rhi->CreateCommandQueue(ECommandQueueType::GRAPHICS);
    const std::array<RHICommandListBase*, 1> _command_array{command_list};
    graphics_queue->SubmitCommands(1, _command_array.data());

    //global buffer
    // start offset
}