// #include "RenderGraphTestRender.h"
// #include "PixelFormat.h"
// #include "RenderThread.h"
// #include "RendererManager.h"
// #include "math/Base.h"
// #include "Core.h"
// #include "math/Function.h"
// #include "math/Matrix.h"
// #include "misc/MMemory.h"
// #include "resources/AsyncResources.h"
// #include "rhi/RHI.h"
// #include "rhi/RHICommon.h"
// #include "rhi/RHIResource.h"
// #include "rhi/RHIResourceInitilizer.h"
// #include "rhi/RHICommand.h"
// #include "rhi/RHIResource.h"
// #include "scene/CameraManager.h"
// #include "shader/Shader.h"
// #include "shader/ShaderResourceManager.h"
// #include "scene/RenderableManager.h"
// #include "scene/Scene.h"
// #include "scene/TransformManager.h"
// #include "rendergraph/RenderGraph.h"
// #include "resources/GpuScene.h"
// #include "scene/Material.h"
//
// #include <algorithm>
//
// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(CameraData)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, view)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, view_proj)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, prev_view_proj)
// DEFINE_SHADER_PARAM(Moer::Vector4f, camera_pos)
// END_SHADER_CONSTANT_STRUCT_DEFINITION()
//
// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(LightingData)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, inv_view_proj)
// DEFINE_SHADER_PARAM(uint32_t, light_count)
// END_SHADER_CONSTANT_STRUCT_DEFINITION()
//
// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(LightData)
// DEFINE_SHADER_PARAM(Moer::Vector4f, color)
// DEFINE_SHADER_PARAM(Moer::Vector4f, position)
// DEFINE_SHADER_PARAM(Moer::Vector4f, direction)
// DEFINE_SHADER_PARAM(Moer::Vector4f, info)
// END_SHADER_CONSTANT_STRUCT_DEFINITION()
//
// class TestGBufferShaderVert : public Shader {
//     DEFINE_SHADER_TYPE(TestGBufferShaderVert, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(CameraData, camera_data)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };
//
// class TestGBufferShaderFragg : public Shader {
//     DEFINE_SHADER_TYPE(TestGBufferShaderFragg, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)
//     // DEFINE_SHADER_PARAM_SAMPLER(SamplerState, defaultSampler)
//     // DEFINE_SHADER_PARAM_SRV(Texture2D, baseColorMap)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };
//
// IMPLEMENT_SHADER_TYPE(TestGBufferShaderVert, "deferedshading/GBufferVert.hlsl", "main", ST_VERTEX);
// IMPLEMENT_SHADER_TYPE(TestGBufferShaderFragg, "deferedshading/GBufferFrag.hlsl", "main", ST_FRAGMENT);
//
// class LightingShaderVert : public Shader {
//     DEFINE_SHADER_TYPE(LightingShaderVert, Global, RENDER_API, ...)
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };
//
// class LightingShaderFrag : public Shader {
//     DEFINE_SHADER_TYPE(LightingShaderFrag, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<MaterialData>, material_data)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<Light>, light_data)
//     DEFINE_SHADER_PARAM_STRUCT(LightingData, lighting_data)
//     //  DEFINE_SHADER_PARAM_SRV_ARRAY(Texture2D, scene_textures, 25)
//     DEFINE_SHADER_PARAM_SRV(Texture2D, scene_texture)
//     DEFINE_SHADER_PARAM_SRV(Texture2D, mat_attach)
//     DEFINE_SHADER_PARAM_SRV(Texture2D, normal_attach)
//     DEFINE_SHADER_PARAM_SRV(Texture2D, depth_attach)
//     DEFINE_SHADER_PARAM_SAMPLER(SamplerState, default_sampler)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };
//
// IMPLEMENT_SHADER_TYPE(LightingShaderVert, "deferedshading/LightingVert.hlsl", "main", ST_VERTEX);
// IMPLEMENT_SHADER_TYPE(LightingShaderFrag, "deferedshading/LightingFrag.hlsl", "main", ST_FRAGMENT);
//
// namespace Moer {
//     class RenderGraphRender::Impl {
//     public:
//         void         Init(const BackendRendererInitInfo& _init_info);
//         void         ShutDown();
//         void         DrawFrame();
//         void         Present();
//         void         SetOriginResolution(uint32_t _width, uint32_t _height);
//         void         SetPresentResolution(uint32_t _width, uint32_t _height);
//         virtual void SetUpRenderGraph(RenderGraph& render_graph, RHIGraphicsCommandList* cmd_list) = 0;
//         void         CullMeshLet();
//
//         RHISRVRef GetRendererOutput();
//
//     protected:
//         VirtualViewport* virtual_viewport;
//         uint64_t         frame_counter = 0;
//
//         Moer::Array<RHIGraphicsCommandList*> render_cmd_lists;
//         RHICommandQueue*                     render_queue;
//         RHIFenceRef                          render_fence;
//
//         //test triangle data
//         RHIBufferRef vertex_buffer;
//         RHIBufferRef index_buffer;
//         RHIBufferRef constant_buffer;
//
//         RHISRVRef instance_buffer_view;
//
//         RHIGraphicsPipelineStateRef gbuffer_pipeline_state;
//         RHIGraphicsPipelineStateRef lighting_pipeline_state;
//
//         Moer::Array<LightData> lights;
//         RHISRVRef              light_buffer_view;
//         RHIBufferRef           light_buffer;
//         // RHIGraphicsPipelineStateRef pipeline_state;
//     };
//
//     class DeferedRenderingRenderGraphRender::Impl : public RenderGraphRender::Impl {
//     public:
//         void SetUpRenderGraph(RenderGraph& render_graph, RHIGraphicsCommandList* cmd_list) override;
//
//     protected:
//     };
//
//     void DeferedRenderingRenderGraphRender::Impl::SetUpRenderGraph(RenderGraph& render_graph, RHIGraphicsCommandList* render_cmd_list) {
//         Extent3D extent = virtual_viewport->GetNextBackBufferExtent();
//
//         render_graph.AddGraphicPass(
//             "GBuffer Pass",
//             [&](RenderGraph::Builder& builder) {
//                 auto normal = render_graph.CreateTexture("normal", {.extent2D = Extent2D(extent.x, extent.y), .format = EPixelFormat::PF_R8G8B8A8_UNORM, .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED});
//                 auto mat    = render_graph.CreateTexture("mat", {.extent2D = Extent2D(extent.x, extent.y), .format = EPixelFormat::PF_R32_UINT, .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED});
//                 auto depth  = render_graph.ImportTexture("depth", virtual_viewport->getDepthTexture());
//                 builder.writeTextures({normal, mat}, RenderGraphTexture::Usage::COLOR_ATTACHMENT);
//                 builder.writeTexture(depth, RenderGraphTexture::Usage::DEPTH_STENCIL_ATTACHMENT);
//                 builder.DeclareRenderPass({.color_attachments = {mat, normal}, .depth_stencil_attachment = depth});
//             },
//             [this](RenderPassContext& context) {
//                 auto cmd_list = context.cmd_list;
//                 cmd_list->SetPipelineState(gbuffer_pipeline_state);
//                 // cmd_list->BindVertexBuffers()
//                 auto scene = g_scene;
//                 cmd_list->BindIndexBuffer(scene->GetBuffer("index_buffer"), 0, IET_UINT32);
//                 uint32_t              offset[]           = {0, 0};
//                 const RHIBufferRef    prim_vertex_buffer = scene->GetBuffer("vertex_buffer");
//                 std::vector<uint32_t> data(25);
//                 const RHIBufferRef    instance_id_buffer = scene->GetBuffer("instance_id_buffer");
//                 RHIBufferRef          vbuffers[]         = {prim_vertex_buffer, instance_id_buffer};
//                 cmd_list->BindVertexBuffers(0, 2, vbuffers, offset);
//
//                 auto camera_entity = g_scene->GetMainCamera();
//                 auto camera        = CameraManager::Get().Get(camera_entity);
//
//                 CameraData camera_data;
//                 camera_data.view           = Transpose(camera->GetViewMatrix());
//                 camera_data.view_proj      = Transpose(camera->GetProjectionMatrix() * camera->GetViewMatrix());
//                 camera_data.camera_pos     = Vector4f(camera->GetPosition(), 1.f);
//                 auto vp                    = camera->GetProjectionMatrix() * camera->GetViewMatrix();
//                 camera_data.prev_view_proj = Transpose(vp);
//
//                 TestGBufferShaderVert::Parameters vert_params;
//                 vert_params.camera_data   = camera_data;
//                 vert_params.instance_data = instance_buffer_view;
//
//                 TestGBufferShaderFragg::Parameters frag_params;
//                 frag_params.instance_data = instance_buffer_view;
//
//                 RHIBatchedShaderParameters batched_params;
//                 batched_params.SetParameters(ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderVert>(), vert_params);
//                 batched_params.SetParameters(ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderFragg>(), frag_params);
//
//                 g_rhi->RHISetBatchedShaderParameters(gbuffer_pipeline_state, batched_params);
//
//                 auto instance_idx = 0;
//                 scene->ForEach([cmd_list, &instance_idx](Entity entity) {
//                     auto mesh_info = RenderableManager::Get().GetMeshInfo(entity);
//                     cmd_list->DrawIndexedInstanced(mesh_info.index_count, 1, mesh_info.index_offset, mesh_info.vertex_offset, instance_idx++);
//                 });
//             });
//
//         render_graph.AddGraphicPass(
//             "Lighting Pass", [&](RenderGraph::Builder& builder) {
//             auto normal = render_graph.GetBlackBoard().GetHandle("normal");
//             auto mat = render_graph.GetBlackBoard().GetHandle("mat");
//             // auto position = render_graph.GetBlackBoard().GetHandle("position");
//             auto output = render_graph.ImportTexture("swapchain_output",virtual_viewport->GetNextBackBufferUAV(virtual_viewport->GetNextBackBuffer().backbuffer_index)->GetTexture());
//             builder.readTextures({normal,mat},RenderGraphTexture::Usage::SAMPLED).writeTexture(output);
//             builder.DeclareRenderPass({.color_attachments =  {output}}); }, [&](RenderPassContext& context) {
//             auto cmd_list = context.cmd_list;
//             cmd_list->SetPipelineState(lighting_pipeline_state);
//
//             //First Get all material instances
//             //Material organize this materials
//             //Material bind all resources for it's pass
//             //Draw a full screen quad pass for each material type
//             Moer::UnorderedSet<EMaterialType> material_types = {};
//             Moer::UnorderedMap<EMaterialType,Moer::Array<MaterialInstanceRef>> material_instances;
//             g_scene->ForEach([&](Entity entity) {
//                 if (RenderableManager::Get().Contains(entity)) {
//
//                      auto mi = RenderableManager::Get().GetMaterialInstance(entity);
//                     material_types.insert(mi->GetMaterial()->GetType());
//                     material_instances[mi->GetMaterial()->GetType()].push_back(mi);
//                 }
//             });
//
//              auto mat_srv = render_graph.GetBlackBoard().GetTexture("mat")->GetSRV();
//              auto normal_srv = render_graph.GetBlackBoard().GetTexture("normal")->GetSRV();
//            auto depth_srv = render_graph.GetBlackBoard().GetTexture("depth")->GetSRV();
//
//            Camera * camera = CameraManager::Get().Get(g_scene->GetMainCamera());
//            LightingData lighting_data;
//            lighting_data.inv_view_proj = Transpose(camera->GetProjectionMatrix() * camera->GetViewMatrix());
//            lighting_data.light_count = lights.size();
//
//             for(auto type : material_types){
//                 RHIBatchedShaderParameters parameters;
//                 LightingShaderFrag::Parameters frag_params;
//                 frag_params.lighting_data = lighting_data;
//                 frag_params.light_data = light_buffer_view;
//                 frag_params.depth_attach = depth_srv;
//                 frag_params.normal_attach = normal_srv;
//                 frag_params.mat_attach = mat_srv;
//                 g_rhi->RHISetBatchedShaderParameters(lighting_pipeline_state,parameters);
//
//                 auto& mat_instances = material_instances[type];
//                 MaterialRef material = mat_instances[0]->GetMaterial();
//                 material->OrganizeInstancesAndBind(parameters,mat_instances);
//                 g_rhi->RHISetBatchedShaderParameters(lighting_pipeline_state,parameters);
//
//                 cmd_list->DrawIndexedInstanced(3, 1, 0, 0, 0);
//
//             } });
//
//         render_graph.SetGraphOutput(render_graph.GetBlackBoard().GetHandle("swapchain_output"));
//     }
//     void RenderGraphRender::Init(const BackendRendererInitInfo& _init_info) {
//         impl->Init(_init_info);
//     }
//
//     void RenderGraphRender::ShutDown() {
//         impl->ShutDown();
//     }
//
//     void RenderGraphRender::DrawFrame() {
//         impl->DrawFrame();
//     }
//
//     void RenderGraphRender::Present() {
//         impl->Present();
//     }
//
//     void RenderGraphRender::SetOriginResolution(uint32_t _width, uint32_t _height) {
//         impl->SetOriginResolution(_width, _height);
//     }
//
//     void RenderGraphRender::SetPresentResolution(uint32_t _width, uint32_t _height) {
//         impl->SetPresentResolution(_width, _height);
//     }
//
//     void* RenderGraphRender::GetRendererOutput() {
//         return impl->GetRendererOutput();
//     }
//
//     DeferedRenderingRenderGraphRender::DeferedRenderingRenderGraphRender() {
//         impl = MoerNew(Impl);
//     }
//     DeferedRenderingRenderGraphRender::~DeferedRenderingRenderGraphRender() {
//         MoerDelete(impl);
//     }
//     void DeferedRenderingRenderGraphRender::Init(const BackendRendererInitInfo& _init_info) {
//         impl = MoerNew(Impl);
//         impl->Init(_init_info);
//     }
//
//     void RenderGraphRender::Impl::Init(const BackendRendererInitInfo& _init_info) {
//         VirtualViewportCreateInfo create_info;
//         create_info.name              = "DeferredRendererViewport";
//         create_info.extent            = Moer::Vector2i(_init_info.width, _init_info.height);
//         create_info.format            = _init_info.format;
//         create_info.back_buffer_count = 3;
//         virtual_viewport              = MoerNew(VirtualViewport)(create_info);
//         render_queue                  = g_rhi->RHICreateCommandQueue(ECommandQueueType::GRAPHICS);
//
//         render_cmd_lists.resize(create_info.back_buffer_count);
//         for (uint32_t i = 0; i < create_info.back_buffer_count; ++i) {
//             render_cmd_lists[i] = g_rhi->RHICreateGraphicsCommandList(g_rhi->RHIGetCurrentCommandAllocator());
//         }
//         render_fence = g_rhi->RHICreateFence({.usage = EFenceUsageFlags::TIMELINE});
//
//         RHIShaderRef gbuffer_vert_shader = ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderVert>();
//         RHIShaderRef gbuffer_frag_shader = ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderFragg>();
//
//         RHIVertexInputInfo vertex_input_info(
//
//             VertexElement(0, 0, PF_R32G32B32_SFLOAT, 0, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//             VertexElement(0, 3 * sizeof(float), PF_R32G32B32_SFLOAT, 1, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//             VertexElement(0, 6 * sizeof(float), PF_R32G32B32_SFLOAT, 2, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//             VertexElement(0, 9 * sizeof(float), PF_R32G32B32_SFLOAT, 3, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//             VertexElement(0, 12 * sizeof(float), PF_R32G32_SFLOAT, 4, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//             VertexElement(1, 14 * sizeof(float), PF_R32_UINT, 5, sizeof(uint32_t), EVertexInputRate::VIR_INSTANCE));
//         // RHIVertexInputStateRef vertex_input_state = g_rhi->RHICreateVertexInputState(vertex_input_state_init_list);
//
//         RHIGraphicsShaderInputInfo gbuffer_shader_input_info = RHIGraphicsShaderInputInfo::Create().SetVertexWorkFlow(vertex_input_info, gbuffer_vert_shader, gbuffer_frag_shader);
//         RHIGraphicsPSOCreateInfo   pso_create_info =
//             RHIGraphicsPSOCreateInfo::Create()
//                 .SetShaderStage(
//                     std::move(gbuffer_shader_input_info))
//                 .SetDepthStencilInfo(RHIDepthStencilStateInfo::Preset<RHIConfig::DepthStencil::DEPTH_WRITE_GREATER>())
//                 .SetColorAttachmentInfo(
//                     {std::move(RHIColorAttachmentInfo::Preset<RHIConfig::Blend::ALPHA_BLEND>(EPixelFormat::PF_R8G8B8A8_SRGB))})
//                 .SetDepthStencilFormat(PF_D32_SFLOAT_S8_UINT)
//                 .Finalize();
//
//         //ToDo: this may be can read and cached from render pass
//         pso_create_info.color_attachment_count = 2;
//         auto& color_attachments_info           = pso_create_info.color_attachments_info;
//         color_attachments_info[0]              = RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R32_UINT);
//         // color_attachments_info[1]              = RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R8G8B8A8_UNORM);
//         color_attachments_info[2] = RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R8G8B8A8_UNORM);
//
//         gbuffer_pipeline_state = g_rhi->RHICreateGraphicsPSO(std::move(pso_create_info));
//
//         RHIShaderRef               lighting_vert_shader       = ShaderResourceManager::GetInstance().GetShader<LightingShaderVert>();
//         RHIShaderRef               lighting_frag_shader       = ShaderResourceManager::GetInstance().GetShader<LightingShaderFrag>();
//         RHIVertexInputInfo         lighting_vertex_input_info = {};
//         RHIGraphicsShaderInputInfo lighting_shader_input_info = RHIGraphicsShaderInputInfo::Create().SetVertexWorkFlow(lighting_vertex_input_info, lighting_vert_shader, lighting_frag_shader);
//         RHIGraphicsPSOCreateInfo   lighting_pso_create_info =
//             RHIGraphicsPSOCreateInfo::Create()
//                 .SetShaderStage(
//                     std::move(lighting_shader_input_info))
//                 .Finalize();
//         lighting_pipeline_state = g_rhi->RHICreateGraphicsPSO(std::move(lighting_pso_create_info));
//
//         instance_buffer_view = g_rhi->RHICreateBufferSRV(g_scene->GetBuffer("instance_data"));
//
//         //HarCode lights
//         auto light_pos   = Moer::Vector3f(0.0f, 128.0f, -225.0f);
//         auto light_color = Moer::Vector3f(1.0, 1.0, 1.0);
//
//         // Magic numbers used to offset lights in the Sponza scene
//         for (int i = -4; i < 4; ++i) {
//             for (int j = 0; j < 2; ++j) {
//                 Moer::Vector3f pos = light_pos;
//                 pos.x += i * 400;
//                 pos.z += j * (225 + 140);
//                 pos.y = 8;
//
//                 for (int k = 0; k < 3; ++k) {
//                     pos.y = pos.y + (k * 100);
//
//                     light_color.x = static_cast<float>(rand()) / (RAND_MAX);
//                     light_color.y = static_cast<float>(rand()) / (RAND_MAX);
//                     light_color.z = static_cast<float>(rand()) / (RAND_MAX);
//
//                     LightData light;
//                     light.color     = Moer::Vector4f(light_color, 1.0f);
//                     light.position  = Moer::Vector4f(pos, 1.0f);
//                     light.direction = Moer::Vector4f(0.0f, 0.0f, 0.0f, 2);
//                     lights.push_back(light);
//                 }
//             }
//         }
//
//         light_buffer      = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::STORAGE_BUFFER, lights.data(), lights.size() * sizeof(LightData));
//         light_buffer_view = g_rhi->RHICreateBufferSRV(light_buffer);
//     }
//
//     void RenderGraphRender::Impl::ShutDown() {
//         RenderThreadFence render_thread_fence;
//         render_thread_fence.BeginFence();
//         render_thread_fence.Wait();
//         MoerDelete(virtual_viewport);
//     }
//
//     void RenderGraphRender::Impl::DrawFrame() {
//         //render and copy to backbuffer
//         EnqueueRenderTask([&] {
//             RenderGraph render_graph;
//             auto        cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];
//             cmd_list->Reset();
//
//             cmd_list->BeginRecording();
//
//             SetUpRenderGraph(render_graph, cmd_list);
//
//             render_graph.Execute({cmd_list, virtual_viewport->GetNextBackBufferExtent()});
//             cmd_list->EndRecording();
//
//             RHISubmitInfo submit_info;
//             submit_info.Wait(virtual_viewport->GetNextBackBuffer().backbuffer_ready_fence, frame_counter);
//             submit_info.Wait(render_fence, frame_counter);
//             submit_info.Signal(render_fence, ++frame_counter);
//
//             render_queue->SubmitCommands(1, cmd_list, &submit_info);
//         });
//
//         return;
//
//         EnqueueRenderTask([this]() {
//             RHIGraphicsCommandList* cmd_list   = render_cmd_lists[frame_counter % render_cmd_lists.size()];
//             uint64_t                wait_index = frame_counter > render_cmd_lists.size() ? frame_counter - render_cmd_lists.size() : 0;
//             //render
//             render_fence->Wait(wait_index);
//
//             cmd_list->Reset();
//
//             cmd_list->BeginRecording();
//
//             RenderGraph render_graph;
//             this->SetUpRenderGraph(render_graph, cmd_list);
//
//             //   render_graph.Execute();
//
//             cmd_list->EndRecording();
//
//             RHISubmitInfo submit_info;
//             submit_info.Wait(virtual_viewport->GetNextBackBuffer().backbuffer_ready_fence, frame_counter);
//             submit_info.Wait(render_fence, frame_counter);
//             submit_info.Signal(render_fence, ++frame_counter);
//
//             render_queue->SubmitCommands(1, cmd_list, &submit_info);
//         });
//
//         // EnqueueRenderTask([this]() {
//         //     auto                      info = virtual_viewport->GetNextBackBuffer();
//         //     auto uav  = virtual_viewport->GetNextBackBufferUAV(info.backbuffer_index);
//         //
//         //     RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];
//         //
//         //     uint64_t wait_index = frame_counter > render_cmd_lists.size() ? frame_counter - render_cmd_lists.size() : 0;
//         //     //render
//         //     render_fence->Wait(wait_index);
//         //
//         //     cmd_list->Reset();
//         //
//         //     cmd_list->BeginRecording();
//         //
//         //     RHIRenderPassInfo pass_info{};
//         //
//         //     pass_info.color_attachments[0].color_attachment_action = AC_CLEAR_STORE;
//         //     RenderAttachmentView& render_attachment_view           = pass_info.color_attachments[0].color_attachment_view;
//         //
//         //     render_attachment_view.required_layout  = TEXTURE_LAYOUT_COLOR_ATTACHMENT;
//         //     render_attachment_view.texture_view     = uav;
//         //     render_attachment_view.clear_attachment = RHIClearAttachment(EClearAttachment::COLOR);
//         //
//         //     Extent3D extent              = uav->GetTexture()->GetExtent3D();
//         //     pass_info.render_area.extent = Extent2D(extent.x, extent.y);
//         //     pass_info.render_area.offset = Offset2D(0, 0);
//         //
//         //     RHIBarrierDependencyInfo barrier_dependency_info;
//         //     barrier_dependency_info.texture_barriers.resize(1);
//         //     auto& texture_barrier_info = barrier_dependency_info.texture_barriers[0];
//         //     texture_barrier_info
//         //         .SetTexture(uav->GetTexture())
//         //         .SetDstTextureLayout(TEXTURE_LAYOUT_COLOR_ATTACHMENT)
//         //         .SetSrcTextureLayout(TEXTURE_LAYOUT_TRANSFER_SRC)
//         //         .SetSrcStage(ERHIPipelineStageFlags::PS_TRANSFER)
//         //         .SetDstStage(ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT)
//         //         .SetDstAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);
//         //
//         //     cmd_list->SetPipelineBarrier(barrier_dependency_info);
//         //     cmd_list->BeginRenderPass(pass_info, "Test Triangle");
//         //
//         //     const VirtualViewportInfo& viewport_info = virtual_viewport->GetInfo();
//         //     ViewPort                   viewport{0, 0, float(viewport_info.extent.x), float(viewport_info.extent.y), 0, 1};
//         //     cmd_list->SetViewPort(viewport);
//         //     cmd_list->SetScissor({0, 0, uint32_t(viewport_info.extent.x), uint32_t(viewport_info.extent.y)});
//         //
//         //     cmd_list->SetPipelineState(pipeline_state);
//         //
//         //     auto* const scene = g_scene;
//         //     if (scene) {
//         //         auto camera_entity = scene->GetCameras()[0];
//         //         auto camera        = CameraManager::Get().Get(camera_entity);
//         //         camera->Tick();
//         //         const auto camera_view = camera->GetViewMatrix();
//         //         const auto camera_proj = camera->GetProjectionMatrix();
//         //
//         //         // Shader* vert_shader = ShaderResourceManager::GetShader<TestDeferredTriangleShaderVert>();
//         //         cmd_list->BindIndexBuffer(scene->GetBuffer("index_buffer"), 0, IET_UINT32);
//         //         uint32_t           offset             = 0;
//         //         const RHIBufferRef prim_vertex_buffer = scene->GetBuffer("vertex_buffer");
//         //         cmd_list->BindVertexBuffers(0, 1, &prim_vertex_buffer, &offset);
//         //
//         //         for (auto entity : scene->GetEntities()) {
//         //             if (RenderableManager::Get().Contains(entity)) {
//         //                 const auto                                 prim_model = TransformManager::Get().Get(entity).matrix;
//         //                 TestDeferredTriangleShaderVert::Parameters params;
//         //                 Matrix4x4f                                 ubo[] = {prim_model, camera_view, camera_proj, Transpose(camera_proj * camera_view * prim_model)};
//         //                 memcpy(&params.scene_ubo, &ubo, sizeof(ubo));
//         //                 RHIBatchedShaderParameters batched_params;
//         //                 batched_params.SetParameters(ShaderResourceManager::GetInstance().GetShader<TestDeferredTriangleShaderVert>(), params);
//         //
//         //                 auto mi = RenderableManager::Get().GetMaterialInstance(entity);
//         //                 mi->Use(batched_params);
//         //
//         //                 auto prim_info = RenderableManager::Get().GetMeshInfo(entity);
//         //
//         //                 g_rhi->RHISetBatchedShaderParameters(pipeline_state, batched_params, true);
//         //
//         //                 // cmd_list->BindIndexBuffer(primitive->GetIndexBuffer(), 0, EIndexElementType::IET_UINT32);
//         //                 // const RHIBufferRef prim_vertex_buffer = primitive->GetVertexBuffer();
//         //                 // uint32_t           offset             = 0;
//         //                 // cmd_list->BindVertexBuffers(0, 1, &prim_vertex_buffer, &offset);
//         //                 cmd_list->DrawIndexedInstanced(prim_info.index_count, 1, prim_info.index_offset, prim_info.vertex_offset, 0);
//         //             }
//         //         }
//         //     } else {
//         //         uint32_t offset = 0;
//         //         cmd_list->BindIndexBuffer(index_buffer, 0, EIndexElementType::IET_UINT32);
//         //         cmd_list->BindVertexBuffers(0, 1, &vertex_buffer, &offset);
//         //         cmd_list->DrawIndexedInstanced(3, 1, 0, 0, 0);
//         //     }
//         //
//         //     cmd_list->EndRenderPass();
//         //
//         //     texture_barrier_info
//         //         .SetDstTextureLayout(TEXTURE_LAYOUT_TRANSFER_SRC)
//         //         .SetSrcTextureLayout(TEXTURE_LAYOUT_COLOR_ATTACHMENT)
//         //         .SetSrcStage(ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT)
//         //         .SetDstStage(ERHIPipelineStageFlags::PS_TRANSFER)
//         //         .SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE)
//         //         .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_READ);
//         //
//         //     cmd_list->SetPipelineBarrier(barrier_dependency_info);
//         //
//         //     cmd_list->EndRecording();
//         //
//         //     RHISubmitInfo submit_info;
//         //     submit_info.Wait(info.backbuffer_ready_fence, frame_counter);
//         //     submit_info.Wait(render_fence, frame_counter);
//         //     submit_info.Signal(render_fence, ++frame_counter);
//         //
//         //     render_queue->SubmitCommands(1, cmd_list, &submit_info);
//         // });
//     }
//
//     void RenderGraphRender::Impl::Present() {
//         EnqueueRenderTask([this]() {
//             virtual_viewport->Present(render_fence);
//         });
//     }
//
//     void RenderGraphRender::Impl::SetOriginResolution(uint32_t _width, uint32_t _height) {
//     }
//
//     void RenderGraphRender::Impl::SetPresentResolution(uint32_t _width, uint32_t _height) {
//         EnqueueRenderTask([this, _width, _height]() {
//             virtual_viewport->OnResize(Extent2D(_width, _height));
//         });
//     }
//
//     void RenderGraphRender::Impl::CullMeshLet() {
//     }
//     RHISRVRef RenderGraphRender::Impl::GetRendererOutput() {
//         return virtual_viewport->GetPresentTextureSRV();
//     }
//
// }// namespace Moer