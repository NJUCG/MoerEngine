//
// Created by 74535 on 2023/10/10.
//

#include <GLFW/glfw3.h>

#include "rhi/vulkan/VulkanRHI.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/Shader.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResourceManager.h"

RHI* g_rhi = nullptr;

// global shader

#include "rhi/RHICommand.h"
RHIBufferRef CreateBufferFromData(const RHIBufferCreateInfo& info, uint32_t size, void* data) {
    RHIBufferRef buffer     = g_rhi->RHICreateBuffer(info);
    void*        mapped_ptr = g_rhi->RHIMapBuffer(buffer, 0, size);
    memcpy(mapped_ptr, data, size);
    g_rhi->RHIUnmapBuffer(buffer);
    return buffer;
}
BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(UniformStructure)

END_SHADER_CONSTANT_STRUCT_DEFINITION(UniformStructure)
class TestShader : public Shader {
    DEFINE_SHADER_TYPE(TestShader, Global, RENDER_API)
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)

    DEFINE_SHADER_PARAM_UAV(RWTexture2D, write_target)
    DEFINE_SHADER_PARAM_SRV(StructuredBuffer, ubo)
    DEFINE_SHADER_PARAM_CBV(StructuredBuffer, cbv)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

// IMPLEMENT_SHADER_TYPE(TestShader, "TestVert.vert", "main", EShaderType::ST_VERTEX)

void Test() {
    // glfwInit();

    // glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // GLFWwindow* window = glfwCreateWindow(800, 800, "VulkanRHITest", nullptr, nullptr);
    g_rhi = new VulkanRHIImpl();

    g_rhi->Initialize(RHIInitInfo());
    g_rhi->PostInit();

    ShaderCompiler::ShaderCompileTest();

    RHIGraphicsPipelineStateInitializer init;
    init.multi_view_count            = 1;
    init.color_attachment_count      = 1;
    init.color_attachment_formats[0] = EPixelFormat::PF_R8G8B8A8_SRGB;
    init.color_attachment_flags[0]   = ETextureUsageFlags::COLOR_ATTACHMENT;
    init.primitive_topology          = EPrimitiveTopology::TRIANGLE_LIST;

    RHIShaderBoundStateInput& shader_state = init.shader_stage;

    VertexInputStateInitializerList vertex_init_list;
    for (int i = 0; i < 1; ++i) {
        vertex_init_list[i].format          = EPixelFormat::PF_R32G32B32_SFLOAT;
        vertex_init_list[i].offset          = 0;
        vertex_init_list[i].stride          = sizeof(Moer::Vector3f);
        vertex_init_list[i].input_rate      = EVertexInputRate::VIR_VERTEX;
        vertex_init_list[i].attribute_index = 0;
        vertex_init_list[i].binding_index   = 0;
    }

    const uint16_t      indices[] = {1, 2, 3};
    RHIBufferCreateInfo buffer_info;
    buffer_info.SetUsage(EBufferUsageFlags::INDEX_BUFFER)
        .SetStride(sizeof(uint16_t))
        .SetSize(sizeof(indices));

    RHIBufferRef index_buffer = CreateBufferFromData(buffer_info, sizeof(indices), (void*)indices);

    RHIBufferCreateInfo v_info;
    v_info.SetSize(16).SetStride(4).SetUsage(EBufferUsageFlags::VERTEX_BUFFER);

    const float  vertex_data[] = {-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 1};
    RHIBufferRef vertex_buffer = CreateBufferFromData(v_info, sizeof(vertex_data), (void*)vertex_data);

    Moer::Array<RHIBufferRef> vertex_buffers = {vertex_buffer};

    shader_state.p_vertex_input_state = g_rhi->RHICreateVertexInputState(vertex_init_list);

    const Moer::Vector2i attachment_size(4, 4);
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
    pass_info.GeneratePipelineAttachmentInfo();
    command_list->BeginRenderPass(pass_info, "triangle pass");
    command_list->BindVertexBuffers(0, 1, vertex_buffers.data(), 0);

    RHIUnorderedAccessViewRef test_view =
        g_rhi->RHICreateUnorderedAccessView(tex,
                                            RHIViewInfo::CreateTextureUAVInfo()
                                                .SetFormat((PF_R8G8B8A8_SRGB)));
    TestShader*            test_shader_vs = (TestShader*)ShaderResourceManager::GetShader<TestShader>();
    TestShader::Parameters params;

    auto test_buff = g_rhi->RHICreateBuffer(buffer_info);

    params.write_target = test_view;
    RHIBatchedShaderParameters batched_params;
    batched_params.SetParameters(test_shader_vs, params);
    // command_list->SetBatchedShaderParameter(batched_params);

    command_list->DrawIndexedInstanced(1, 1, 0, 0, 0);

    command_list->EndRenderPass();

    RHICommandQueue*                                graphics_queue = g_rhi->CreateCommandQueue(ECommandQueueType::GRAPHICS);
    const Moer::StaticArray<RHICommandListBase*, 1> command_array{command_list};
    // graphics_queue->SubmitCommands(1, command_array.data());

    //global buffer
    // start offset
    //VkSetDescriptorWrite()

    //rootSignature <=> pipelineLayout -> descriptorLayout descriptorLayoutBinding
}

int main() {
    Test();

    return 0;
}