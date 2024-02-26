#include "DeferredRenderer.h"
#include "PixelFormat.h"
#include "RenderThread.h"
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
#include "scene/CameraManager.h"
#include "shader/Shader.h"
#include "shader/ShaderResourceManager.h"
#include "scene/RenderableManager.h"
#include "scene/Scene.h"
#include "scene/TransformManager.h"

#include <algorithm>
BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(UBOVertex)
DEFINE_SHADER_PARAM(Moer::Matrix4x4f, model)
DEFINE_SHADER_PARAM(Moer::Matrix4x4f, view)
DEFINE_SHADER_PARAM(Moer::Matrix4x4f, proj)
DEFINE_SHADER_PARAM(Moer::Matrix4x4f, mvp)
END_SHADER_CONSTANT_STRUCT_DEFINITION()

BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(CameraData)
DEFINE_SHADER_PARAM(Moer::Matrix4x4f, view)
DEFINE_SHADER_PARAM(Moer::Matrix4x4f, proj)
DEFINE_SHADER_PARAM(Moer::Matrix4x4f, inv_view)
DEFINE_SHADER_PARAM(Moer::Matrix4x4f, inv_proj)
END_SHADER_CONSTANT_STRUCT_DEFINITION()
class TestDeferredTriangleShaderVert : public Shader {
    DEFINE_SHADER_TYPE(TestDeferredTriangleShaderVert, Global, RENDER_API, ...)
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    DEFINE_SHADER_PARAM_STRUCT(CameraData, camera_data)
    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

class TestDeferredTriangleShaderFrag : public Shader {
    DEFINE_SHADER_TYPE(TestDeferredTriangleShaderFrag, Global, RENDER_API, ...)
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    DEFINE_SHADER_PARAM_SAMPLER(SamplerState, defaultSampler)
    DEFINE_SHADER_PARAM_SRV(Texture2D, baseColorMap)
    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

IMPLEMENT_SHADER_TYPE(TestDeferredTriangleShaderVert, "test/TriangleVert.hlsl", "main", ST_VERTEX);
IMPLEMENT_SHADER_TYPE(TestDeferredTriangleShaderFrag, "test/TriangleFrag.hlsl", "main", ST_FRAGMENT);

class MeshletCullingShader : public Shader {
    DEFINE_SHADER_TYPE(MeshletCullingShader, Global, RENDER_API, ...)
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    DEFINE_SHADER_PARAM_STRUCT(CameraData, camera_data)//push_constant
    DEFINE_SHADER_PARAM_SRV(StructuredBuffer<MeshletDesc>, meshlet_info_buffer)
    DEFINE_SHADER_PARAM_SRV(StructuredBuffer<MeshletBound>, meshlet_bound_buffer)

    DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<DrawCommand>, draw_indirect_buffer)
    DEFINE_SHADER_PARAM_UAV(RWByteAddressBuffer, draw_count_buffer)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

IMPLEMENT_SHADER_TYPE(MeshletCullingShader, "meshdebug/Cull.hlsl", "main", ST_COMPUTE);
namespace Moer {
    class DeferredRenderer::Impl {
    public:
        void Init(const BackendRendererInitInfo& _init_info);
        void ShutDown();
        void DrawFrame();
        void Present();
        void SetOriginResolution(uint32_t _width, uint32_t _height);
        void SetPresentResolution(uint32_t _width, uint32_t _height);

        RHISRVRef GetRendererOutput();

    private:
        VirtualViewport* virtual_viewport;
        uint64_t         frame_counter = 0;

        Moer::Array<RHIGraphicsCommandList*> render_cmd_lists;
        RHICommandQueue*                     render_queue;
        RHIFenceRef                          render_fence;

        //test triangle data
        // RHIBufferRef vertex_buffer;
        // RHIBufferRef index_buffer;

        RHIGraphicsPipelineStateRef pipeline_state;
        RHIComputePipelineStateRef  compute_pipeline_state;

        RHIBufferRef draw_indirect_buffer;
        RHIBufferRef draw_count_buffer;
        RHIBufferRef zero_buffer;
        RHISRVRef    meshlet_descs_buffer_view;
        RHISRVRef    meshlet_bounds_buffer_view;

        RHIUAVRef draw_indirect_view;
        RHIUAVRef draw_count_view;
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

        RHIVertexInputInfo vertex_input_info(

            VertexElement(0, 0, PF_R32G32B32_SFLOAT, 0, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 3 * sizeof(float), PF_R32G32B32_SFLOAT, 1, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 6 * sizeof(float), PF_R32G32B32_SFLOAT, 2, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 9 * sizeof(float), PF_R32G32B32_SFLOAT, 3, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 12 * sizeof(float), PF_R32G32_SFLOAT, 4, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX));

        // RHIVertexInputStateRef vertex_input_state = g_rhi->RHICreateVertexInputState(vertex_input_state_init_list);

        auto& shader_resource_manager = ShaderResourceManager::GetInstance();

        RHIShaderRef vertex_shader   = shader_resource_manager.GetShader<TestDeferredTriangleShaderVert>();
        RHIShaderRef fragment_shader = shader_resource_manager.GetShader<TestDeferredTriangleShaderFrag>();
        // RHIShaderBoundStateInput& shader_stage_input = init.shader_stage;

        RHIGraphicsShaderInputInfo shader_input_info =
            RHIGraphicsShaderInputInfo::Create()
                .SetVertexWorkFlow(std::move(vertex_input_info),
                                   vertex_shader,
                                   fragment_shader);

        RHIGraphicsPSOCreateInfo pso_create_info =
            RHIGraphicsPSOCreateInfo::Create()
                .SetShaderStage(
                    std::move(shader_input_info))
                .SetDepthStencilInfo(RHIDepthStencilStateInfo::Preset<RHIConfig::DepthStencil::DEPTH_WRITE_LESS>())
                .SetColorAttachmentInfo(
                    {std::move(RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R8G8B8A8_SRGB))});

        pipeline_state = g_rhi->RHICreateGraphicsPSO(std::move(pso_create_info));

        auto compute_shader    = shader_resource_manager.GetShader<MeshletCullingShader>();
        compute_pipeline_state = g_rhi->RHICreateComputePipelineState(compute_shader.Get());

        //why not implement a counter buffer?
        {
            RHIBufferCreateInfo buffer_create_info;
            draw_indirect_buffer = g_rhi->RHICreateBuffer(RHIBufferCreateInfo::Create(
                1024 * 1024 * 64,
                0,
                EBufferUsageFlags::INDIRECT_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::STORAGE_BUFFER));

            draw_count_buffer = g_rhi->RHICreateBuffer(RHIBufferCreateInfo::Create(
                sizeof(uint32_t),
                0,
                EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::STORAGE_BUFFER));

            zero_buffer = g_rhi->RHICreateBuffer(RHIBufferCreateInfo::Create(
                sizeof(uint32_t),
                0,
                EBufferUsageFlags::CPU_VISIBLE | EBufferUsageFlags::TRANSFER_SRC));

            void* mapped = g_rhi->RHIMapBuffer(zero_buffer, 0, sizeof(uint32_t));

            *(uint32_t*)mapped = 0;

            g_rhi->RHIUnmapBuffer(zero_buffer);

            draw_indirect_view =
                g_rhi->RHICreateUAV(draw_indirect_buffer, RHIViewInfo::CreateBufferUAVInfo());

            draw_count_view =
                g_rhi->RHICreateUAV(draw_count_buffer, RHIViewInfo::CreateBufferUAVInfo());

            auto meshlet_descs = g_scene->GetBuffer("meshlet_descs");

            meshlet_descs_buffer_view = g_rhi->RHICreateSRV(g_scene->GetBuffer("meshlet_descs"),
                                                            RHIViewInfo::CreateBufferSRVInfo()
                                                                .SetByteOffset(0)
                                                                .SetNumElements(meshlet_descs->GetSize() / sizeof(MeshletDesc)));

            meshlet_bounds_buffer_view = g_rhi->RHICreateSRV(g_scene->GetBuffer("meshlet_bounds"),
                                                             RHIViewInfo::CreateBufferSRVInfo()
                                                                 .SetByteOffset(0)
                                                                 .SetStride(sizeof(MeshletBound)));
        }
    }

    void DeferredRenderer::Impl::ShutDown() {
        RenderThreadFence render_thread_fence;
        render_thread_fence.BeginFence();
        render_thread_fence.Wait();
        MoerDelete(virtual_viewport);
    }

    void DeferredRenderer::Impl::DrawFrame() {
        //render and copy to backbuffer
        auto camera_entity = g_scene->GetCameras()[0];
        auto camera        = CameraManager::Get().Get(camera_entity);
        camera->Tick();

        CameraData camera_data;
        camera_data.view = camera->GetViewMatrix();
        camera_data.proj = camera->GetProjectionMatrix();

        camera_data.inv_view = Inverse(camera_data.view.t);
        camera_data.inv_proj = Inverse(camera_data.proj.t);

        {
            MeshletCullingShader::Parameters cull_params;
            cull_params.draw_count_buffer    = draw_count_view;
            cull_params.draw_indirect_buffer = draw_indirect_view;
            cull_params.camera_data          = camera_data;
            cull_params.meshlet_info_buffer  = meshlet_descs_buffer_view;
            cull_params.meshlet_bound_buffer = meshlet_bounds_buffer_view;

            RHIBatchedShaderParameters compute_batched_params;
            compute_batched_params.SetParameters(ShaderResourceManager::GetInstance().GetShader<MeshletCullingShader>(), cull_params);

            auto&& sync_command_list = [this]() {
                int                     i        = render_cmd_lists.size();
                RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];
                cmd_list->Reset();
                cmd_list->BeginRecording();
            };

            auto&& reset_counter_buffer = [this]() {
                RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];

                RHICopyBufferInfo copy_info{};
                copy_info.regions.push_back({0, 0, sizeof(uint32_t)});
                cmd_list->CopyBuffer(copy_info, zero_buffer, draw_count_buffer);
            };

            auto&& cull_task = [this, compute_param = std::move(compute_batched_params)]() {
                RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];

                RHIBarrierDependencyInfo barrier_dependency_info{};
                barrier_dependency_info.buffer_barriers.resize(2);
                auto& buffer_barrier_info = barrier_dependency_info.buffer_barriers[0];
                buffer_barrier_info
                    .SetBuffer(draw_count_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                    .SetSrcAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_TRANSFER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

                auto& buffer_barrier_info_1 = barrier_dependency_info.buffer_barriers[1];
                buffer_barrier_info_1
                    .SetBuffer(draw_indirect_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                    .SetSrcAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_TRANSFER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

                cmd_list->SetPipelineBarrier(barrier_dependency_info);

                cmd_list->SetPipelineState(compute_pipeline_state);
                g_rhi->RHISetBatchedShaderParameters(compute_pipeline_state, compute_param);
                cmd_list->Dispatch(16, 16, 16);
                {
                    RHIBarrierDependencyInfo post_compute_barrier{};
                    post_compute_barrier.buffer_barriers.resize(2);
                    auto& buffer_barrier_info_0 = post_compute_barrier.buffer_barriers[0];
                    buffer_barrier_info_0
                        .SetBuffer(draw_count_buffer)
                        .SetDstAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
                        .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                        .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                        .SetDstStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT);

                    auto& buffer_barrier_info_1 = post_compute_barrier.buffer_barriers[1];
                    buffer_barrier_info_1
                        .SetBuffer(draw_indirect_buffer)
                        .SetDstAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
                        .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                        .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                        .SetDstStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT);

                    cmd_list->SetPipelineBarrier(post_compute_barrier);
                }
            };

            EnqueueRenderTask(std::move(sync_command_list));
            // EnqueueRenderTask(std::move(reset_counter_buffer));
            // EnqueueRenderTask(std::move(cull_task));
        }

        {
            TestDeferredTriangleShaderVert::Parameters params;
            params.camera_data = camera_data;

            RHIBatchedShaderParameters batched_params;
            batched_params.SetParameters(ShaderResourceManager::GetInstance().GetShader<TestDeferredTriangleShaderVert>(), params);

            EnqueueRenderTask([this, batched_params = std::move(batched_params)]() {
                auto      info = virtual_viewport->GetNextBackBuffer();
                RHIUAVRef uav  = virtual_viewport->GetNextBackBufferUAV(info.backbuffer_index);

                RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];

                RHIRenderPassInfo pass_info{};

                pass_info.color_attachments[0].color_attachment_action = AC_CLEAR_STORE;
                RenderAttachmentView& render_attachment_view           = pass_info.color_attachments[0].color_attachment_view;

                render_attachment_view.required_layout  = TEXTURE_LAYOUT_COLOR_ATTACHMENT;
                render_attachment_view.texture_view     = uav;
                render_attachment_view.clear_attachment = RHIClearAttachment(EClearAttachment::COLOR);

                Extent3D extent              = uav->GetTexture()->GetExtent3D();
                pass_info.render_area.extent = Extent2D(extent.x, extent.y);
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
                // cmd_list->BeginRenderPass(pass_info, "Test Triangle");

                // const VirtualViewportInfo& viewport_info = virtual_viewport->GetInfo();
                // ViewPort                   viewport{0, 0, float(viewport_info.extent.x), float(viewport_info.extent.y), 0, 1};
                // cmd_list->SetViewPort(viewport);
                // cmd_list->SetScissor({0, 0, uint32_t(viewport_info.extent.x), uint32_t(viewport_info.extent.y)});

                // cmd_list->SetPipelineState(pipeline_state);

                // auto* const scene = g_scene;
                // if (scene) {
                //     auto camera_entity = scene->GetCameras()[0];
                //     auto camera        = CameraManager::Get().Get(camera_entity);
                //     camera->Tick();
                //     const auto camera_view = camera->GetViewMatrix();
                //     const auto camera_proj = camera->GetProjectionMatrix();

                //     // Shader* vert_shader = ShaderResourceManager::GetShader<TestDeferredTriangleShaderVert>();
                //     cmd_list->BindIndexBuffer(scene->GetBuffer("index_buffer"), 0, IET_UINT32);
                //     uint32_t           offset             = 0;
                //     const RHIBufferRef prim_vertex_buffer = scene->GetBuffer("vertex_buffer");
                //     cmd_list->BindVertexBuffers(0, 1, &prim_vertex_buffer, &offset);

                //     cmd_list->DrawIndexedIndirect(draw_indirect_buffer, 0, draw_count_buffer, 0, 11451, sizeof(uint32_t) * 4);
                // } else {
                //     assert(false);
                // }

                // cmd_list->EndRenderPass();

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
            });
        }
    }

    void DeferredRenderer::Impl::Present() {
        EnqueueRenderTask([this]() {
            virtual_viewport->Present(render_fence);
        });
    }

    void DeferredRenderer::Impl::SetOriginResolution(uint32_t _width, uint32_t _height) {
    }

    void DeferredRenderer::Impl::SetPresentResolution(uint32_t _width, uint32_t _height) {
        EnqueueRenderTask([this, _width, _height]() {
            virtual_viewport->OnResize(Extent2D(_width, _height));
        });
    }

    RHISRVRef DeferredRenderer::Impl::GetRendererOutput() {
        return virtual_viewport->GetPresentTextureSRV();
    }

}// namespace Moer