#include "DeferredRenderer.h"
#include "PixelFormat.h"
#include "RendererManager.h"
#include "math/Base.h"
#include "Core.h"
#include "math/Function.h"
#include "math/Matrix.h"
#include "misc/MMemory.h"
#include "resources/AsyncResources.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "rhi/RHICommand.h"
#include "shader/Shader.h"
#include "shader/ShaderResourceManager.h"
#include <algorithm>

class TestDeferredTriangleShaderVert : public Shader {
    DEFINE_SHADER_TYPE(TestDeferredTriangleShaderVert, Global, RENDER_API, ...)
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

class TestDeferredTriangleShaderFrag : public Shader {
    DEFINE_SHADER_TYPE(TestDeferredTriangleShaderFrag, Global, RENDER_API, ...)
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

IMPLEMENT_SHADER_TYPE(TestDeferredTriangleShaderVert, "test/Triangle.vert", "main", ST_VERTEX);
IMPLEMENT_SHADER_TYPE(TestDeferredTriangleShaderFrag, "test/Triangle.frag", "main", ST_FRAGMENT);

namespace Moer {
    class DeferredRenderer::Impl {
    public:
        void Init(const BackendRendererInitInfo& _init_info);
        void ShutDown();
        void DrawFrame();
        void Present();
        void SetOriginResolution(uint32_t _width, uint32_t _height);
        void SetPresentResolution(uint32_t _width, uint32_t _height);

        RHIShaderResourceViewRef GetRendererOutput();

    private:
        VirtualViewport* virtual_viewport;
        uint64_t         frame_counter = 0;

        Moer::Array<RHIGraphicsCommandList*> render_cmd_lists;
        RHICommandQueue*                     render_queue;
        RHIFenceRef                          render_fence;

        //test triangle data
        RHIBufferRef vertex_buffer;
        RHIBufferRef index_buffer;
        RHIBufferRef constant_buffer;

        RHIGraphicsPipelineStateRef pipeline_state;
    };
    void DeferredRenderer::Init(const BackendRendererInitInfo& _init_info) {
        impl = MoerNew(Impl);
        impl->Init(_init_info);
    }

    void DeferredRenderer::ShutDown() {
        impl->ShutDown();
    }

    void DeferredRenderer::DrawFrame() {
        impl->DrawFrame();
    }

    void DeferredRenderer::Present() {
        impl->Present();
    }

    void DeferredRenderer::SetOriginResolution(uint32_t _width, uint32_t _height) {
        impl->SetOriginResolution(_width, _height);
    }

    void DeferredRenderer::SetPresentResolution(uint32_t _width, uint32_t _height) {
        impl->SetPresentResolution(_width, _height);
    }

    void* DeferredRenderer::GetRendererOutput() {
        return impl->GetRendererOutput();
    }

    void DeferredRenderer::Impl::Init(const BackendRendererInitInfo& _init_info) {
        VirtualViewportCreateInfo create_info;
        create_info.name              = "DeferredRendererViewport";
        create_info.extent            = Moer::Vector2i(_init_info.width, _init_info.height);
        create_info.format            = _init_info.format;
        create_info.back_buffer_count = 3;
        virtual_viewport              = MoerNew(VirtualViewport)(create_info);
        render_queue                  = g_rhi->RHICreateCommandQueue(ECommandQueueType::GRAPHICS);

        render_cmd_lists.resize(create_info.back_buffer_count);
        for (uint32_t i = 0; i < create_info.back_buffer_count; ++i) {
            render_cmd_lists[i] = g_rhi->RHICreateGraphicsCommandList(g_rhi->RHIGetCurrentCommandAllocator());
        }
        render_fence = g_rhi->RHICreateFence({.usage = EFenceUsageFlags::TIMELINE});

        //test draw triangle
        float vertices[] = {
            -0.5f,
            -0.5f,
            0.0f,
            1.0f,
            0.0f,
            0.5f,
            -0.5f,
            0.0f,
            0.0f,
            1.0f,
            0.0f,
            0.5f,
            0.0f,
            1.0f,
            1.0f,
        };

        uint32_t indexes[] = {0, 1, 2};
        vertex_buffer      = g_rhi->RHICreateBuffer(
            RHIBufferCreateInfo::Create()
                .SetSize(sizeof(vertices))
                .SetStride(sizeof(float) * 5)
                .SetUsage(EBufferUsageFlags::VERTEX_BUFFER |
                          EBufferUsageFlags::CPU_VISIBLE));
        void* data = g_rhi->RHIMapBuffer(vertex_buffer, 0, sizeof(vertices));
        memcpy(data, vertices, sizeof(vertices));
        g_rhi->RHIUnmapBuffer(vertex_buffer);

        index_buffer = g_rhi->RHICreateBuffer(
            RHIBufferCreateInfo::Create()
                .SetSize(sizeof(indexes))
                .SetStride(sizeof(uint32_t) * 3)
                .SetUsage(EBufferUsageFlags::INDEX_BUFFER |
                          EBufferUsageFlags::CPU_VISIBLE));
        data = g_rhi->RHIMapBuffer(index_buffer, 0, sizeof(indexes));
        memcpy(data, indexes, sizeof(indexes));
        g_rhi->RHIUnmapBuffer(index_buffer);

        RHIBlendStateInitializer blend_init;
        blend_init.attachments[0].color_blend_op         = BO_ADD;
        blend_init.attachments[0].color_src_blend_factor = BF_SRC_ALPHA;
        blend_init.attachments[0].color_dst_blend_factor = BF_ONE_MINUS_SRC_ALPHA;
        blend_init.attachments[0].alpha_blend_op         = BO_ADD;
        blend_init.attachments[0].alpha_src_blend_factor = BF_ONE;
        blend_init.attachments[0].alpha_dst_blend_factor = BF_ONE_MINUS_SRC_ALPHA;
        blend_init.attachments[0].color_write_mask       = CW_RGBA;

        RHIBlendStateRef blend_state = g_rhi->RHICreateBlendState(blend_init);

        RHIRasterizationStateInitializer rasterization_init{};
        rasterization_init.cull_mode            = RCM_BACK;
        rasterization_init.fill_mode            = FM_FILL;
        rasterization_init.b_depth_clamp_enable = false;
        rasterization_init.b_depth_bias         = false;
        rasterization_init.b_enable_msaa        = false;

        RHIRasterizationStateRef rasterization_state = g_rhi->RHICreateRasterizationState(rasterization_init);

        RHIMultisampleStateInitializer multisample_init{};
        multisample_init.sample_count            = 1;
        RHIMultisampleStateRef multisample_state = g_rhi->RHICreateMultiSampleState(multisample_init);

        RHIDepthStencilStateInitializer depth_stencil_init{};
        depth_stencil_init.b_enable_depth_write     = false;
        depth_stencil_init.depth_test_op            = CO_NEVER;
        RHIDepthStencilStateRef depth_stencil_state = g_rhi->RHICreateDepthStencilState(depth_stencil_init);

        RHIGraphicsPipelineStateInitializer::TAttachmentFormats color_attachment_formats{};
        color_attachment_formats[0] = EPixelFormat::PF_R8G8B8A8_SRGB;
        RHIGraphicsPipelineStateInitializer::TAttachmentFlags color_attachment_flags{};
        color_attachment_flags[0] = ETextureUsageFlags::COLOR_ATTACHMENT;

        RHIGraphicsPipelineStateInitializer
            init(blend_state,
                 rasterization_state,
                 multisample_state,
                 depth_stencil_state,
                 EPrimitiveTopology::TRIANGLE_LIST,
                 1,
                 color_attachment_formats,
                 color_attachment_flags,
                 PF_UNDEFINED,
                 ETextureUsageFlags::UNDEFINED,
                 {},
                 0,
                 1,
                 false,
                 VSR_NONE);

        VertexInputStateInitializerList vertex_input_state_init_list{};
        vertex_input_state_init_list[0] = VertexElement(0, 0, PF_R32G32B32_SFLOAT, 0, sizeof(float) * 5, EVertexInputRate::VIR_VERTEX);
        vertex_input_state_init_list[1] = VertexElement(0, 3 * sizeof(float), PF_R32G32_SFLOAT, 1, sizeof(float) * 5, EVertexInputRate::VIR_VERTEX);

        RHIVertexInputStateRef vertex_input_state = g_rhi->RHICreateVertexInputState(vertex_input_state_init_list);

        RHIVertexShaderRef   vertex_shader = g_rhi->RHICreateVertexShader(ShaderResourceManager::GetShader<TestDeferredTriangleShaderVert>());
        RHIFragmentShaderRef fragment_shader =
            g_rhi->RHICreateFragmentShader(ShaderResourceManager::GetShader<TestDeferredTriangleShaderFrag>());
        RHIShaderBoundStateInput& shader_stage_input = init.shader_stage;

        shader_stage_input.p_vertex_input_state = vertex_input_state;
        shader_stage_input.p_vertex_shader      = vertex_shader;
        shader_stage_input.p_fragment_shader    = fragment_shader;

        pipeline_state = g_rhi->RHICreateGraphicsPipelineState(init);
    }

    void DeferredRenderer::Impl::ShutDown() {
        MoerDelete(virtual_viewport);
    }

    void DeferredRenderer::Impl::DrawFrame() {
        //render and copy to backbuffer
        auto                      info = virtual_viewport->GetNextBackBuffer();
        RHIUnorderedAccessViewRef uav  = virtual_viewport->GetNextBackBufferUAV(info.backbuffer_index);

        RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];

        uint64_t wait_index = frame_counter > render_cmd_lists.size() ? frame_counter - render_cmd_lists.size() : 0;
        //render
        render_fence->Wait(wait_index);

        cmd_list->Reset();

        cmd_list->BeginRecording();

        RHIRenderPassInfo pass_info{};

        pass_info.color_attachments[0].color_attachment_action = AC_CLEAR_STORE;
        RenderAttachmentView& render_attachment_view           = pass_info.color_attachments[0].color_attachment_view;

        render_attachment_view.required_layout  = TEXTURE_LAYOUT_COLOR_ATTACHMENT;
        render_attachment_view.texture_view     = uav;
        render_attachment_view.clear_attachment = RHIClearAttachment(EClearAttachment::COLOR);

        pass_info.render_area.extent = Extent2D(virtual_viewport->GetInfo().extent);
        pass_info.render_area.offset = Offset2D(0, 0);

        RHIBarrierDependencyInfo barrier_dependency_info;
        barrier_dependency_info.texture_barriers.resize(1);
        auto& texture_barrier_info = barrier_dependency_info.texture_barriers[0];
        texture_barrier_info
            .SetTexture(uav->GetTexture())
            .SetDstTextureLayout(TEXTURE_LAYOUT_COLOR_ATTACHMENT)
            .SetSrcTextureLayout(TEXTURE_LAYOUT_TRANSFER_SRC)
            .SetSrcStage(ERHIPipelineStageFlags::PS_TRANSFER)
            .SetDstStage(ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT)
            .SetDstAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);

        cmd_list->SetPipelineBarrier(barrier_dependency_info);
        cmd_list->BeginRenderPass(pass_info, "Test Triangle");

        const VirtualViewportInfo& viewport_info = virtual_viewport->GetInfo();
        ViewPort                   viewport{0, 0, float(viewport_info.extent.x), float(viewport_info.extent.y), 0, 1};
        cmd_list->SetViewPort(viewport);
        cmd_list->SetScissor({0, 0, uint32_t(viewport_info.extent.x), uint32_t(viewport_info.extent.y)});

        cmd_list->SetPipelineState(pipeline_state);
        cmd_list->BindIndexBuffer(index_buffer, 0, EIndexElementType::IET_UINT32);
        uint32_t offset = 0;
        cmd_list->BindVertexBuffers(0, 1, &vertex_buffer, &offset);

        cmd_list->DrawIndexedInstanced(3, 1, 0, 0, 0);

        cmd_list->EndRenderPass();

        texture_barrier_info
            .SetDstTextureLayout(TEXTURE_LAYOUT_TRANSFER_SRC)
            .SetSrcTextureLayout(TEXTURE_LAYOUT_COLOR_ATTACHMENT)
            .SetSrcStage(ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT)
            .SetDstStage(ERHIPipelineStageFlags::PS_TRANSFER)
            .SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE)
            .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_READ);

        cmd_list->SetPipelineBarrier(barrier_dependency_info);

        cmd_list->EndRecording();

        RHISubmitInfo submit_info;
        submit_info.Wait(info.backbuffer_ready_fence, frame_counter);
        submit_info.Wait(render_fence, frame_counter);
        submit_info.Signal(render_fence, ++frame_counter);

        render_queue->SubmitCommands(1, cmd_list, &submit_info);
    }

    void DeferredRenderer::Impl::Present() {
        virtual_viewport->Present(render_fence);
    }

    void DeferredRenderer::Impl::SetOriginResolution(uint32_t _width, uint32_t _height) {
    }

    void DeferredRenderer::Impl::SetPresentResolution(uint32_t _width, uint32_t _height) {
    }

    RHIShaderResourceViewRef DeferredRenderer::Impl::GetRendererOutput() {
        return virtual_viewport->GetPresentTextureSRV();
    }

}// namespace Moer