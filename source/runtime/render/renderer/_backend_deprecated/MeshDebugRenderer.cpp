// #include "MeshDebugRenderer.h"
// #include "RenderAPI.h"
// #include "RenderThread.h"
// #include "renderer/BackendRenderer.h"
// #include "rhi/RHI.h"
// #include "rhi/RHICommand.h"
// #include "resources/AsyncResources.h"
// #include "rhi/RHICommon.h"
// #include "rhi/RHIResource.h"
// #include "scene/Scene.h"
// #include "shader/Shader.h"
// #include "shader/ShaderParameterMacros.h"
// #include "shader/ShaderResourceManager.h"
// #include "resources/GpuScene.h"
// #include "Common.h"

// #include "scene/CameraManager.h"
// #include "scene/RenderableManager.h"
// #include "scene/TransformManager.h"

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(CameraData)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, view)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, proj)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, inv_view)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, inv_proj)
// END_SHADER_CONSTANT_STRUCT_DEFINITION()

// class MeshDebugRendererVertexShader : public Shader {
//     DEFINE_SHADER_TYPE(MeshDebugRendererVertexShader, Shader, RENDER_API);

// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(CameraData, camera_data)
//     END_ROOT_PARAMETER_DEFINITION(Parameters);
// };

// IMPLEMENT_SHADER_TYPE(MeshDebugRendererVertexShader, "features/debug/MeshDebugVert.hlsl", "main", ST_VERTEX);

// class MeshDebugRendererFragmentShader : public Shader {
//     DEFINE_SHADER_TYPE(MeshDebugRendererFragmentShader, Shader, RENDER_API);

// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     END_ROOT_PARAMETER_DEFINITION(Parameters);
// };

// IMPLEMENT_SHADER_TYPE(MeshDebugRendererFragmentShader, "features/debug/MeshDebugFrag.hlsl", "main", ST_FRAGMENT);

// class MeshletCullShader : public Shader {
//     DEFINE_SHADER_TYPE(MeshletCullShader, Shader, RENDER_API);

// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     END_ROOT_PARAMETER_DEFINITION(Parameters);
// };

// // IMPLEMENT_SHADER_TYPE(MeshletCullShader, "features/debug/Cull.hlsl", "main", ST_COMPUTE);

// namespace Moer {
//     class MeshDebugRenderer::Impl {
//     public:
//         void Init(const BackendRendererInitInfo& _init_info);
//         void ShutDown();
//         void DrawFrame();
//         void Present();
//         void SetOriginResolution(uint32_t _width, uint32_t _height);
//         void SetPresentResolution(uint32_t _width, uint32_t _height);

//         RHISRVRef GetRendererOutput();

//     private:
//         VirtualViewport* virtual_viewport;
//         uint64_t         frame_counter = 0;

//         Moer::Array<RHIGraphicsCommandList*> render_cmd_lists;
//         RHICommandQueue*                     render_queue;
//         RHIFenceRef                          render_fence;

//         //test triangle data
//         RHIBufferRef vertex_buffer;
//         RHIBufferRef index_buffer;
//         RHIBufferRef constant_buffer;

//         RHIGfxPsoRef pipeline_state;

//         Scene* g_scene = nullptr;
//     };
//     void MeshDebugRenderer::Init(const BackendRendererInitInfo& _init_info) {
//         impl = MoerNew(Impl);
//         impl->Init(_init_info);
//     }

//     void MeshDebugRenderer::ShutDown() {
//         impl->ShutDown();
//     }

//     void MeshDebugRenderer::DrawFrame() {
//         impl->DrawFrame();
//     }

//     void MeshDebugRenderer::Present() {
//         impl->Present();
//     }

//     void MeshDebugRenderer::SetOriginResolution(uint32_t _width, uint32_t _height) {
//         impl->SetOriginResolution(_width, _height);
//     }

//     void MeshDebugRenderer::SetPresentResolution(uint32_t _width, uint32_t _height) {
//         impl->SetPresentResolution(_width, _height);
//     }

//     RHISRVRef MeshDebugRenderer::GetRendererOutput() {
//         return impl->GetRendererOutput();
//     }

//     void MeshDebugRenderer::Impl::Init(const BackendRendererInitInfo& _init_info) {
//         VirtualViewportCreateInfo create_info;
//         create_info.name              = "DeferredRendererViewport";
//         create_info.extent            = Moer::Vector2i(_init_info.width, _init_info.height);
//         create_info.format            = _init_info.format;
//         create_info.back_buffer_count = 3;
//         virtual_viewport              = MoerNew(VirtualViewport)(create_info);
//         render_queue                  = g_rhi->RHICreateCommandQueue(ECommandQueueType::GRAPHICS);

//         render_cmd_lists.resize(create_info.back_buffer_count);
//         for (uint32_t i = 0; i < create_info.back_buffer_count; ++i) {
//             render_cmd_lists[i] = g_rhi->RHICreateGraphicsCommandList();
//         }
//         render_fence = g_rhi->RHICreateFence({.usage = EFenceUsageFlags::TIMELINE});

//         auto& shader_resource_manager = ShaderResourceManager::GetInstance();

//         RHIShaderRef vertex_shader   = shader_resource_manager.GetShader<MeshDebugRendererVertexShader>();
//         RHIShaderRef fragment_shader = shader_resource_manager.GetShader<MeshDebugRendererFragmentShader>();

//         RHIVertexInputInfo vertex_input_info(
//             {
//                 VertexElement(0, 0, PF_R32G32B32_SFLOAT, 0, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//                 VertexElement(0, 3 * sizeof(float), PF_R32G32B32_SFLOAT, 1, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//                 VertexElement(0, 6 * sizeof(float), PF_R32G32B32_SFLOAT, 2, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//                 VertexElement(0, 9 * sizeof(float), PF_R32G32B32_SFLOAT, 3, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//                 VertexElement(0, 12 * sizeof(float), PF_R32G32_SFLOAT, 4, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//             });

//         // RHIVertexInputStateRef vertex_input_state = g_rhi->RHICreateVertexInputState(vertex_input_state_init_list);

//         // RHIShaderBoundStateInput& shader_stage_input = init.shader_stage;

//         RHIGraphicsShaderInputInfo shader_input_info =
//             RHIGraphicsShaderInputInfo::Create()
//                 .SetVertexWorkFlow(std::move(vertex_input_info),
//                                    vertex_shader,
//                                    fragment_shader);

//         RHIGraphicsPSOCreateInfo pso_create_info =
//             RHIGraphicsPSOCreateInfo::Create()
//                 .SetShaderStage(
//                     std::move(shader_input_info))
//                 .SetDepthStencilInfo(RHIDepthStencilStateInfo::Preset<Moer::Render::DepthStencil::DEPTH_WRITE_LESS>())
//                 .SetColorAttachmentInfo(
//                     {std::move(RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R8G8B8A8_SRGB))});

//         pipeline_state = g_rhi->RHICreateGraphicsPSO(std::move(pso_create_info));
//     }

//     void MeshDebugRenderer::Impl::ShutDown() {
//         RenderThreadFence render_thread_fence;
//         render_thread_fence.BeginFence();
//         render_thread_fence.Wait();
//         MoerDelete(virtual_viewport);
//     }

//     void MeshDebugRenderer::Impl::DrawFrame() {
//         //render and copy to backbuffer
//         EnqueueRenderTask([this]() {
//             auto      info = virtual_viewport->GetBackBufferInfo();
//             RHIUAVRef uav  = info.backbuffer_uav;

//             RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];

//             uint64_t wait_index = frame_counter > render_cmd_lists.size() ? frame_counter - render_cmd_lists.size() : 0;
//             //render
//             render_fence->Wait(wait_index);

//             cmd_list->Reset();

//             cmd_list->BeginRecording();

//             RHIRenderPassInfo pass_info{};

//             pass_info.color_attachments[0].color_attachment_action = AC_CLEAR_STORE;
//             RenderAttachmentView& render_attachment_view           = pass_info.color_attachments[0].color_attachment_view;

//             render_attachment_view.required_layout  = TEXTURE_LAYOUT_COLOR_ATTACHMENT;
//             render_attachment_view.texture_view     = uav;
//             render_attachment_view.clear_attachment = RHIClearAttachment(EClearAttachment::COLOR);

//             Extent3D extent              = uav->GetTexture()->GetExtent3D();
//             pass_info.render_area.extent = Extent2D(extent.x, extent.y);
//             pass_info.render_area.offset = Offset2D(0, 0);

//             RHIBarrierDependencyInfo barrier_dependency_info;
//             barrier_dependency_info.texture_barriers.resize(1);
//             auto& texture_barrier_info = barrier_dependency_info.texture_barriers[0];
//             texture_barrier_info
//                 .SetTexture(uav->GetTexture())
//                 .SetDstTextureLayout(TEXTURE_LAYOUT_COLOR_ATTACHMENT)
//                 .SetSrcTextureLayout(TEXTURE_LAYOUT_TRANSFER_SRC)
//                 .SetSrcStage(ERHIPipelineStageFlags::PS_TRANSFER)
//                 .SetDstStage(ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT)
//                 .SetDstAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);

//             cmd_list->SetPipelineBarrier(barrier_dependency_info);
//             cmd_list->BeginRenderPass(pass_info, "Test Triangle");

//             const VirtualViewportInfo& viewport_info = virtual_viewport->GetInfo();
//             ViewPort                   viewport{0, 0, float(viewport_info.extent.x), float(viewport_info.extent.y), 0, 1};
//             cmd_list->SetViewPort(viewport);
//             cmd_list->SetScissor({0, 0, uint32_t(viewport_info.extent.x), uint32_t(viewport_info.extent.y)});

//             cmd_list->SetPipelineState(pipeline_state);

//             auto* const scene = g_scene;
//             if (scene) {
//                 auto camera_entity = scene->GetCameras()[0];
//                 auto camera        = CameraManager::Get().Get(camera_entity);
//                 camera->Tick();
//                 const auto camera_view = camera->GetViewMatrix();
//                 const auto camera_proj = camera->GetProjectionMatrix();

//                 // Shader* vert_shader = ShaderResourceManager::GetShader<TestDeferredTriangleShaderVert>();
//                 MeshDebugRendererVertexShader::Parameters params;
//                 params.camera_data.view     = camera_view;
//                 params.camera_data.proj     = camera_proj;
//                 params.camera_data.inv_view = Inverse(camera_view);
//                 params.camera_data.inv_proj = Inverse(camera_proj);

//                 for (auto entity : scene->GetEntities()) {
//                     if (auto primitive = RenderableManager::Get().GetRenderPrimitive(entity)) {
//                         const auto prim_model = TransformManager::Get().Get(entity).matrix;

//                         // Matrix4x4f                                 ubo[] = {prim_model, camera_view, camera_proj, Transpose(camera_proj * camera_view * prim_model)};
//                         // memcpy(&params.scene_ubo, &ubo, sizeof(ubo));
//                         RHIBatchedShaderParameters batched_params;
//                         batched_params.SetParameters(ShaderResourceManager::GetInstance().GetShader<MeshDebugRendererVertexShader>(), params);

//                         // auto mi = RenderableManager::Get().GetMaterialInstance(entity);
//                         // mi->Use(batched_params);

//                         // g_rhi->RHISetBatchedShaderParameters(pipeline_state, batched_params, true);

//                         cmd_list->BindIndexBuffer(primitive->GetIndexBuffer(), 0, EIndexElementType::IET_UINT32);
//                         const RHIBufferRef prim_vertex_buffer = primitive->GetVertexBuffer();
//                         uint32_t           offset             = 0;
//                         cmd_list->BindVertexBuffers(0, 1, &prim_vertex_buffer, &offset);
//                         cmd_list->DrawIndexedInstanced(primitive->GetCount(), 1, 0, 0, 0);
//                     }
//                 }
//             } else {
//                 uint32_t offset = 0;
//                 cmd_list->BindIndexBuffer(index_buffer, 0, EIndexElementType::IET_UINT32);
//                 cmd_list->BindVertexBuffers(0, 1, &vertex_buffer, &offset);
//                 cmd_list->DrawIndexedInstanced(3, 1, 0, 0, 0);
//             }

//             cmd_list->EndRenderPass();

//             texture_barrier_info
//                 .SetDstTextureLayout(TEXTURE_LAYOUT_TRANSFER_SRC)
//                 .SetSrcTextureLayout(TEXTURE_LAYOUT_COLOR_ATTACHMENT)
//                 .SetSrcStage(ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT)
//                 .SetDstStage(ERHIPipelineStageFlags::PS_TRANSFER)
//                 .SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE)
//                 .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_READ);

//             cmd_list->SetPipelineBarrier(barrier_dependency_info);

//             cmd_list->EndRecording();

//             RHISubmitInfo submit_info;
//             submit_info.Wait(info.backbuffer_ready_fence, frame_counter);
//             submit_info.Wait(render_fence, frame_counter);
//             submit_info.Signal(render_fence, ++frame_counter);

//             render_queue->SubmitCommands(1, cmd_list, &submit_info);
//         });
//     }

//     void MeshDebugRenderer::Impl::Present() {
//         EnqueueRenderTask([this]() {
//             virtual_viewport->Present(render_fence);
//         });
//     }

//     void MeshDebugRenderer::Impl::SetOriginResolution(uint32_t _width, uint32_t _height) {
//     }

//     void MeshDebugRenderer::Impl::SetPresentResolution(uint32_t _width, uint32_t _height) {
//         EnqueueRenderTask([this, _width, _height]() {
//             virtual_viewport->OnResize(Extent2D(_width, _height));
//         });
//     }

//     RHISRVRef MeshDebugRenderer::Impl::GetRendererOutput() {
//         return virtual_viewport->GetPresentTextureSRV();
//     }

// }// namespace Moer