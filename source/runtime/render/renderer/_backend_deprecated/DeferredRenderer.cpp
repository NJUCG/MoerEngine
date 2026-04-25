// #include "DeferredRenderer.h"
// #include "Core.h"
// #include "PixelFormat.h"
// #include "RenderThread.h"
// #include "log/LogSystem.h"
// #include "math/Base.h"
// #include "math/Function.h"
// #include "math/Matrix.h"
// #include "misc/MMemory.h"
// #include "misc/STL.h"
// #include "misc/Timer.h"
// #include "renderer/BackendRenderer.h"
// #include "rendergraph/RenderGraphPass.h"
// #include "resources/AsyncResources.h"
// #include "rhi/RHI.h"
// #include "rhi/RHICommon.h"
// #include "rhi/RHIResource.h"
// #include "rhi/RHIResourceInitilizer.h"
// #include "rhi/RHICommand.h"
// #include "scene/CameraManager.h"
// #include "scene/RenderableManager.h"
// #include "scene/TransformManager.h"
// #include "shader/Shader.h"
// #include "shader/ShaderMutation.h"
// #include "shader/ShaderParameterMacros.h"
// #include "shader/ShaderResourceManager.h"
// #include "scene/Scene.h"
// #include "scene/light/LightComponentManager.h"
// #include "deferred/BasePass.h"
// #include "utils/HiZBuilder.h"
// #include "utils/CopyDispatchArgs.h"
// #include "Common.h"

// #include <algorithm>
// #include "Cull.h"
// #include "imgui.h"
// #include "rendergraph/RenderGraph.h"
// #include "resources/GpuScene.h"
// #include "scene/Material.h"
// #include "deferred/RenderResourceDeferred.h"
// #include "deferred/BasePass.h"
// IMPLEMENT_SHADER_TYPE(CullInstancePrePassShader, "features/debug/CullInstance.hlsl", "prepass", ST_COMPUTE)
// IMPLEMENT_SHADER_TYPE(CullMeshletPrepassShader, "features/debug/CullMeshlet.hlsl", "prepass", ST_COMPUTE)

// IMPLEMENT_SHADER_TYPE(CullInstanceRecheckShader, "features/debug/CullInstance.hlsl", "recheck_pass", ST_COMPUTE)
// IMPLEMENT_SHADER_TYPE(CullMeshletRecheckShader, "features/debug/CullMeshlet.hlsl", "recheck_pass", ST_COMPUTE)

// IMPLEMENT_SHADER_TYPE(TestDeferredTriangleShaderVert, "test/TriangleDeferredVert.hlsl", "main", ST_VERTEX);
// IMPLEMENT_SHADER_TYPE(TestDeferredTriangleShaderFrag, "test/TriangleDeferredFrag.hlsl", "main", ST_FRAGMENT);

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(LightingData)
// DEFINE_SHADER_PARAM(Moer::Matrix4x4f, inv_view_proj)
// DEFINE_SHADER_PARAM(uint32_t, light_count)
// DEFINE_SHADER_PARAM(Moer::Vector3ui, padding)
// DEFINE_SHADER_PARAM(Moer::Vector3f, camera_position)
// END_SHADER_CONSTANT_STRUCT_DEFINITION()

// BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(LightData)
// DEFINE_SHADER_PARAM(Moer::Vector4f, color)
// DEFINE_SHADER_PARAM(Moer::Vector4f, position)
// DEFINE_SHADER_PARAM(Moer::Vector4f, direction)
// DEFINE_SHADER_PARAM(Moer::Vector4f, info)
// END_SHADER_CONSTANT_STRUCT_DEFINITION()

// IMPLEMENT_SHADER_TYPE(TestGBufferShaderVert, "deprecated/pipelines/raster/GBufferVert.hlsl", "main", ST_VERTEX);
// IMPLEMENT_SHADER_TYPE(TestGBufferShaderFrag, "deprecated/pipelines/raster/GBufferFrag.hlsl", "main", ST_FRAGMENT);

// class LightingShaderVert : public Shader {
//     DEFINE_SHADER_TYPE(LightingShaderVert, Global, RENDER_API, ...)
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// class LightingShaderFrag : public Shader {
//     DEFINE_SHADER_TYPE(LightingShaderFrag, Global, RENDER_API, ...)
// public:
//     BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//     DEFINE_SHADER_PARAM_STRUCT(LightingData, lighting_data)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<MaterialData>, material_data)
//     DEFINE_SHADER_PARAM_SRV(StructuredBuffer<Light>, light_data)
//     DEFINE_SHADER_PARAM_SRV_ARRAY(Texture2D, scene_textures, 25)
//     DEFINE_SHADER_PARAM_SRV(Texture2D, mat_attach)
//     DEFINE_SHADER_PARAM_SRV(Texture2D, normal_attach)
//     DEFINE_SHADER_PARAM_SRV(Texture2D, gbuffer_uv)
//     DEFINE_SHADER_PARAM_SRV(Texture2D, depth_attach)
//     DEFINE_SHADER_PARAM_SAMPLER(SamplerState, default_sampler)
//     END_ROOT_PARAMETER_DEFINITION(Parameters)
// };

// IMPLEMENT_SHADER_TYPE(LightingShaderVert, "deprecated/pipelines/raster/LightingVert.hlsl", "main", ST_VERTEX);
// IMPLEMENT_SHADER_TYPE(LightingShaderFrag, "deprecated/pipelines/raster/LightingFrag.hlsl", "main", ST_FRAGMENT);

// constexpr uint32_t uniform_buffer_size = sizeof(VirtualView);
// namespace Moer {
//     class DeferredRenderer::Impl {
//     public:
//         void Init(const BackendRendererInitInfo& _init_info);
//         void InitSceneResources();
//         void ShutDown();
//         void DrawFrame();
//         void Present();
//         void FallBackDraw();
//         void SetOriginResolution(uint32_t _width, uint32_t _height);
//         void SetPresentResolution(uint32_t _width, uint32_t _height);
//         void UpdateGui();

//         RHISRVRef GetRendererOutput();

//     private:
//         void CreateDepthBuffer();
//         void OnResizeVSwapChain();

//         void PrePass();
//         void RecheckPass();
//         void LightingPass();

//         void DispatchDrawScene(const CameraData& _camera_data, bool _first_pass = true);

//     private:
//         VirtualViewport* virtual_viewport;

//         RHIGfxPsoRef gbuffer_pipeline_state;
//         RHIGfxPsoRef lighting_pipeline_state;

//         RHIBufferRef           light_buffer;
//         RHISRVRef              light_buffer_view;
//         Moer::Array<LightData> lights;

//         RHIComputePsoRef cull_instance_recheck_pso;
//         RHIComputePsoRef cull_meshlet_recheck_pso;

//         RHIShaderRef cull_instance_recheck_shader;
//         RHIShaderRef cull_meshlet_recheck_shader;

//         RHIComputePsoRef cull_instance_prepass_pso;
//         RHIComputePsoRef cull_meshlet_prepass_pso;

//         RHIShaderRef cull_instance_prepass_shader;
//         RHIShaderRef cull_meshlet_prepass_shader;

//         HiZBuffer hiz_buffer;

//         // RHIBufferRef draw_indirect_buffer;
//         // RHIBufferRef draw_count_buffer;
//         // RHIBufferRef zero_buffer;
//         // RHIBufferRef instance_meshlet_cull_info_buffer;
//         // RHIBufferRef recheck_cull_info_buffer;
//         // RHIBufferRef recheck_instance_id_buffer;
//         RHIBufferRef uniform_buffer;
//         RHIBufferRef view_buffer;

//         Array<RHICBVRef> uniform_buffer_view;

//         RHISRVRef meshlet_descs_buffer_view;
//         RHISRVRef meshlet_bounds_buffer_view;
//         RHISRVRef instance_buffer_view;
//         RHISRVRef instance_meshlet_info_view;
//         RHISRVRef instance_meshlet_cull_info_view;
//         RHISRVRef recheck_cull_info_view;
//         RHISRVRef recheck_instance_id_srv;

//         RHIUAVRef instance_meshlet_cull_info_uav;
//         RHIUAVRef recheck_cull_info_uav;
//         RHIUAVRef recheck_instance_id_uav;

//         RHIUAVRef draw_indirect_view;
//         RHIUAVRef draw_count_view;

//         static constexpr uint32_t meshlet_count_offset              = 0;
//         static constexpr uint32_t draw_count_offset                 = 4;
//         static constexpr uint32_t recheck_draw_count_offset         = 8;
//         static constexpr uint32_t check_instance_count_offset       = 12;
//         static constexpr uint32_t check_meshlet_count_offset        = 16;
//         static constexpr uint32_t instance_dispatch_indirect_offset = 20;
//         static constexpr uint32_t meshlet_dispatch_offset           = 24;
//         static constexpr uint32_t recheck_meshlet_dispatch_offset   = 36;
//         static constexpr uint32_t max_meshlet_count                 = 1024 * 1024 * 16;
//         static constexpr uint32_t thread_group_count                = 64;

//         Vector2i source_resolution;

//         RenderContext render_context;

//         UniquePtr<BasePass> base_pass;

//         bool b_need_update = true;

//         std::string                   m_present_texture = "swapchain_output";
//         std::vector<std::string_view> m_current_textures{"swapchain_output"};

//         RHISamplerRef sampler;
//     };
//     void DeferredRenderer::Init(const BackendRendererInitInfo& _init_info) {
//         impl = MoerNew(Impl);
//         impl->Init(_init_info);
//     }

//     void DeferredRenderer::ShutDown() {
//         impl->ShutDown();
//     }

//     void DeferredRenderer::DrawFrame() {
//         impl->DrawFrame();
//     }

//     void DeferredRenderer::Present() {
//         impl->Present();
//     }

//     void DeferredRenderer::SetOriginResolution(uint32_t _width, uint32_t _height) {
//         impl->SetOriginResolution(_width, _height);
//     }

//     void DeferredRenderer::SetPresentResolution(uint32_t _width, uint32_t _height) {
//         impl->SetPresentResolution(_width, _height);
//     }
//     void DeferredRenderer::UpdateGUI() {
//         BackendRenderer::UpdateGUI();
//         impl->UpdateGui();
//     }

//     RHISRVRef DeferredRenderer::GetRendererOutput() {
//         return impl->GetRendererOutput();
//     }

//     void DeferredRenderer::Impl::CreateDepthBuffer() {
//     }

//     void DeferredRenderer::Impl::OnResizeVSwapChain() {
//         assert(IsCurrentlyRenderThread());
//         virtual_viewport->OnResize(source_resolution);
//         hiz_buffer.InitFromDepthExtent(virtual_viewport->GetDepthSRV()->GetTexture()->GetExtent2D());
//     }

//     void DeferredRenderer::Impl::Init(const BackendRendererInitInfo& _init_info) {
//         VirtualViewportCreateInfo create_info;
//         source_resolution             = Vector2i(_init_info.width, _init_info.height);
//         create_info.name              = "DeferredRendererViewport";
//         create_info.extent            = source_resolution;
//         create_info.format            = _init_info.format;
//         create_info.back_buffer_count = 2;
//         virtual_viewport              = MoerNew(VirtualViewport)(create_info);

//         render_context.Init({.back_buffer_cnt = create_info.back_buffer_count,
//                              .main_viewport   = *virtual_viewport});

//         base_pass = std::move(UniquePtr<BasePass>(MoerNew(BasePass)));

//         auto& shader_resource_manager = ShaderResourceManager::GetInstance();
//         // cull_instance_recheck_shader  = shader_resource_manager.GetShader<CullInstanceRecheckShader>();
//         // cull_meshlet_recheck_shader   = shader_resource_manager.GetShader<CullMeshletRecheckShader>();

//         // cull_instance_recheck_pso = g_rhi->RHICreateComputePipelineState(cull_instance_recheck_shader);
//         // cull_meshlet_recheck_pso  = g_rhi->RHICreateComputePipelineState(cull_meshlet_recheck_shader);

//         // cull_instance_prepass_shader = shader_resource_manager.GetShader<CullInstancePrePassShader>();
//         // cull_meshlet_prepass_shader  = shader_resource_manager.GetShader<CullMeshletPrepassShader>();
//         // cull_instance_prepass_pso    = g_rhi->RHICreateComputePipelineState(cull_instance_prepass_shader);
//         // cull_meshlet_prepass_pso     = g_rhi->RHICreateComputePipelineState(cull_meshlet_prepass_shader);
//         //why not implement a counter buffer?
//         {
//             RHIBufferCreateInfo buffer_create_info;
//             uniform_buffer = g_rhi->RHICreateBuffer<float>(uniform_buffer_size * 3, EBufferUsageFlags::CONSTANT_BUFFER | EBufferUsageFlags::CPU_VISIBLE);
//             view_buffer    = g_rhi->RHICreateBuffer<float>(uniform_buffer_size, EBufferUsageFlags::CONSTANT_BUFFER | EBufferUsageFlags::TRANSFER_DST);
//             // for (int i = 0; i < 3; i++) {
//             //     uniform_buffer_view.push_back(
//             //         g_rhi->RHICreateCBV(uniform_buffer, uniform_buffer_size, uniform_buffer_size * i));
//             // }

//             // draw_indirect_buffer = g_rhi->RHICreateBuffer<DrawInstanceCmd>(
//             //     1024 * 1024,
//             //     EBufferUsageFlags::INDIRECT_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::UNORDERED_ACCESS);

//             // draw_count_buffer = g_rhi->RHICreateBuffer<uint32_t>(64 * sizeof(int),
//             //                                                      EBufferUsageFlags::TRANSFER_DST |
//             //                                                          EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDIRECT_BUFFER);

//             // zero_buffer = g_rhi->RHICreateBuffer<uint32_t>(64 * sizeof(int),
//             //                                                EBufferUsageFlags::CPU_VISIBLE | EBufferUsageFlags::TRANSFER_SRC);

//             // void* mapped = g_rhi->RHIMapBuffer(zero_buffer, 0, sizeof(uint32_t));

//             // std::array<uint32_t, 32> zero_data{};
//             // std::copy(zero_data.begin(), zero_data.end(), static_cast<uint32_t*>(mapped));

//             // g_rhi->RHIUnmapBuffer(zero_buffer);

//             // draw_indirect_view =
//             //     g_rhi->RHICreateBufferUAV(draw_indirect_buffer);

//             // draw_count_view =
//             //     g_rhi->RHICreateBufferUAV(draw_count_buffer);
//         }
//         {
//             CreateDepthBuffer();
//         }

//         {
//             RHIShaderRef gbuffer_vert_shader = ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderVert>();
//             RHIShaderRef gbuffer_frag_shader = ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderFrag>();

//             RHIVertexInputInfo vertex_input_info(

//                 VertexElement(0, 0, PF_R32G32B32_SFLOAT, 0, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
//                 VertexElement(0, 3 * sizeof(float), PF_R32G32B32_SFLOAT, 1, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
//                 VertexElement(0, 6 * sizeof(float), PF_R32G32B32_SFLOAT, 2, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
//                 VertexElement(0, 9 * sizeof(float), PF_R32G32_SFLOAT, 3, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
//                 VertexElement(1, 11 * sizeof(float), PF_R32_UINT, 4, sizeof(uint32_t), EVertexInputRate::VIR_INSTANCE));
//             // RHIVertexInputStateRef vertex_input_state = g_rhi->RHICreateVertexInputState(vertex_input_state_init_list);

//             RHIGraphicsShaderInputInfo gbuffer_shader_input_info = RHIGraphicsShaderInputInfo::Create().SetVertexWorkFlow(vertex_input_info, gbuffer_vert_shader, gbuffer_frag_shader);
//             RHIGraphicsPSOCreateInfo   pso_create_info =
//                 RHIGraphicsPSOCreateInfo::Create()
//                     .SetShaderStage(
//                         std::move(gbuffer_shader_input_info))
//                     .SetDepthStencilInfo(RHIDepthStencilStateInfo::Preset<Moer::Render::DepthStencil::DEPTH_WRITE_GREATER>())
//                     .SetColorAttachmentInfo(
//                         {std::move(RHIColorAttachmentInfo::Preset<Moer::Render::Blend::ALPHA_BLEND>(EPixelFormat::PF_R8G8B8A8_SRGB))})
//                     .SetDepthStencilFormat(PF_D32_SFLOAT_S8_UINT)
//                     .Finalize();

//             //ToDo: this may be can read and cached from render pass
//             pso_create_info.color_attachment_count = 3;
//             auto& color_attachments_info           = pso_create_info.color_attachments_info;
//             color_attachments_info[0]              = RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R32_UINT);
//             // color_attachments_info[1]              = RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R8G8B8A8_UNORM);
//             color_attachments_info[1] = RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R8G8B8A8_UNORM);
//             color_attachments_info[2] = RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R16G16_SFLOAT);

//             gbuffer_pipeline_state = g_rhi->RHICreateGraphicsPSO(std::move(pso_create_info));

//             RHIShaderRef               lighting_vert_shader       = ShaderResourceManager::GetInstance().GetShader<LightingShaderVert>();
//             RHIShaderRef               lighting_frag_shader       = ShaderResourceManager::GetInstance().GetShader<LightingShaderFrag>();
//             RHIVertexInputInfo         lighting_vertex_input_info = {};
//             RHIGraphicsShaderInputInfo lighting_shader_input_info = RHIGraphicsShaderInputInfo::Create().SetVertexWorkFlow(lighting_vertex_input_info, lighting_vert_shader, lighting_frag_shader);
//             RHIGraphicsPSOCreateInfo   lighting_pso_create_info =
//                 RHIGraphicsPSOCreateInfo::Create()
//                     .SetShaderStage(
//                         std::move(lighting_shader_input_info))
//                     .SetRasterizerInfo({
//                         .cull_mode = RCM_NONE,
//                     })
//                     .Finalize();
//             lighting_pipeline_state = g_rhi->RHICreateGraphicsPSO(std::move(lighting_pso_create_info));
//         }
//         {
//             // render_graphs.resize(render_cmd_lists.size());
//         }
//         CopyDispatchArgs::Init(render_context);
//         base_pass->InitResources(render_context);

//         if (sampler == nullptr) {
//             RHISamplerCreateInfo create_info(SF_NEAREST, TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
//             create_info.SetCompareOp(SCF_NEVER)
//                 .SetAddressMode(ESamplerAddressMode::SAM_CLAMP_TO_EDGE);
//             sampler = g_rhi->RHICreateSampler(create_info);
//         }
//     }

//     void DeferredRenderer::Impl::InitSceneResources() {
//         // FIXME: If this function is called multiple times (> 1000), the engine will crash and stuck OS.
//         //        According to the variable `b_need_update`, this function should be called multiple times.

//         auto meshlet_descs = g_scene->GetBuffer("meshlet_descs");

//         meshlet_descs_buffer_view = g_rhi->RHICreateBufferSRV(g_scene->GetBuffer("meshlet_descs"));

//         meshlet_bounds_buffer_view = g_rhi->RHICreateBufferSRV(g_scene->GetBuffer("meshlet_bounds"));

//         instance_buffer_view = g_rhi->RHICreateBufferSRV(g_scene->GetBuffer("instance_data"));

//         instance_meshlet_info_view = g_rhi->RHICreateBufferSRV(g_scene->GetBuffer("instance_meshlet_info_buffer"));

//         // instance_meshlet_cull_info_buffer = g_rhi->RHICreateBuffer<uint64_t>(
//         //     1024 * 512 * sizeof(uint64_t),
//         //     EBufferUsageFlags::UNORDERED_ACCESS);

//         // recheck_cull_info_buffer = g_rhi->RHICreateBuffer<uint64_t>(
//         //     1024 * 512 * sizeof(uint64_t),
//         //     EBufferUsageFlags::UNORDERED_ACCESS);
//         // recheck_instance_id_buffer = g_rhi->RHICreateBuffer<uint32_t>(
//         //     64 * 512 * sizeof(uint32_t),
//         //     EBufferUsageFlags::UNORDERED_ACCESS);
//         // instance_meshlet_cull_info_view = g_rhi->RHICreateBufferSRV(instance_meshlet_cull_info_buffer);

//         // instance_meshlet_cull_info_uav = g_rhi->RHICreateBufferUAV(instance_meshlet_cull_info_buffer);

//         // recheck_cull_info_view = g_rhi->RHICreateBufferSRV(recheck_cull_info_buffer);
//         // recheck_cull_info_uav  = g_rhi->RHICreateBufferUAV(recheck_cull_info_buffer);

//         // recheck_instance_id_srv = g_rhi->RHICreateBufferSRV(recheck_instance_id_buffer);
//         // recheck_instance_id_uav = g_rhi->RHICreateBufferUAV(recheck_instance_id_buffer);

//         lights.clear();
//         for (auto& light_entity : g_scene->GetLights()) {
//             auto      light_component = LightComponentManager::Get().Get(light_entity);
//             LightData light_data;

//             if (auto light = dynamic_cast<DirectionalLightComponent*>(light_component.Get())) {
//                 // lightcycle control, prevent accidently misuse directional_light in other branches
//                 auto directional_light = light;

//                 light_data.color     = Vector4f(directional_light->GetColor(), directional_light->GetIntensity());
//                 light_data.position  = Vector4f(0.0f, 0.0f, 0.0f, 1.0f);
//                 light_data.direction = Vector4f(directional_light->GetDirection(), 1);// 1 means directional light

//             } else if (auto light = dynamic_cast<PointLightComponent*>(light_component.Get())) {
//                 auto point_light = light;

//                 light_data.color     = Vector4f(point_light->GetColor(), point_light->GetIntensity());
//                 light_data.position  = Vector4f(point_light->GetPosition(), 1.0f);
//                 light_data.direction = Vector4f(0.0f, 0.0f, 0.0f, 2);// 2 means point light

//             } else if (auto light = dynamic_cast<SpotLightComponent*>(light_component.Get())) {
//                 auto spot_light = light;

//                 // TODO: implement spot light (need test case)
//                 // Be careful, I don't test any part of spot light (including data storage, etc)
//                 LOG_WARNING(MOER_TEXT("Spot light not implemented yet"));

//             } else {
//                 LOG_WARNING(MOER_TEXT("Unknown light type"));
//             }

//             lights.push_back(light_data);
//         }
//         if (lights.size() == 0) {
//             LOG_WARNING(MOER_TEXT("No light in scene! Please make sure the scene cache is latest! And you have at least one light in scene."));
//             LOG_WARNING(MOER_TEXT("For ply and json scene, the loader hasn't implement default lights. So you can implement it by yourself refer to LightComponent.cpp:CreateDefaultLightComponents() and loader/gltf/Parser.cpp:LoadLights()"));
//         }

//         light_buffer      = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::UNORDERED_ACCESS, lights.data(), lights.size() * sizeof(LightData));
//         light_buffer_view = g_rhi->RHICreateBufferSRV(light_buffer);

//         base_pass->UpdateSceneData(render_context);
//     }

//     void DeferredRenderer::Impl::ShutDown() {
//         RenderThreadFence render_thread_fence;
//         render_thread_fence.BeginFence();
//         render_thread_fence.Wait();
//         MoerDelete(virtual_viewport);
//         CopyDispatchArgs::Dispose();
//     }

//     void DeferredRenderer::Impl::DrawFrame() {
//         if (!Scene::GetCurrentSceneLoadInfo()->IsReady()) {
//             //should draw default instead of return
//             EnqueueRenderTask([this]() {
//                 FallBackDraw();
//             });
//             return;
//         }

//         if (b_need_update) {
//             InitSceneResources();
//             b_need_update = false;
//         }
//         //render and copy to backbuffer
//         auto camera_entity = g_scene->GetCameras()[0];
//         auto camera        = CameraManager::Get().Get(camera_entity);

//         CameraData camera_data;

//         auto vp = camera->GetProjectionMatrix() * camera->GetViewMatrix();

//         camera_data.prev_view_proj = vp;
//         auto prev_view_proj        = vp;

//         camera->Tick();

//         camera_data.view       = camera->GetViewMatrix();
//         camera_data.view_proj  = camera->GetProjectionMatrix() * camera->GetViewMatrix();
//         camera_data.camera_pos = Vector4f(camera->GetPosition(), 1.f);
//         CameraCullData cull_data;
//         cull_data.camera_data      = camera_data;
//         cull_data.proj             = camera->GetProjectionMatrix();
//         cull_data.aspect_ratio     = camera->GetAspectRatio();
//         cull_data.far_plane        = camera->GetFarClip();
//         cull_data.near_plane       = camera->GetNearClip();
//         cull_data.inv_tan_half_fov = 1.f / camera->GetTanHalfFov();

//         VirtualView view_data;
//         view_data.view             = camera->GetViewMatrix();
//         view_data.view_proj        = camera->GetProjectionMatrix() * camera->GetViewMatrix();
//         view_data.prev_view_proj   = prev_view_proj;
//         view_data.proj             = camera->GetProjectionMatrix();
//         view_data.pos              = camera->GetPosition();
//         view_data.aspect_ratio     = camera->GetAspectRatio();
//         view_data.nearz            = camera->GetNearClip();
//         view_data.inv_tan_half_fov = 1.f / camera->GetTanHalfFov();
//         camera->GetPlanes(view_data.planes);
//         Vector3f aabb_min, aabb_max;
//         camera->GetAABB(aabb_min, aabb_max);
//         view_data.bound_center = (aabb_min + aabb_max) * 0.5f;
//         view_data.bound_extent = (aabb_max - aabb_min) * 0.5f;

//         auto& frustum_planes = cull_data.frustum_planes;
//         {
//             auto target_vp    = vp;
//             frustum_planes[0] = target_vp.r3 + target_vp.r0;//left
//             frustum_planes[1] = target_vp.r3 - target_vp.r0;//right
//             frustum_planes[2] = target_vp.r3 + target_vp.r1;//top
//             frustum_planes[3] = target_vp.r3 - target_vp.r1;//bottom
//             frustum_planes[4] = target_vp.r2;               //near
//             frustum_planes[5] = target_vp.r3 - target_vp.r2;//far
//             //normalize
//             for (int i = 0; i < 6; i++) {
//                 auto length       = Length(Vector3f(frustum_planes[i]));
//                 frustum_planes[i] = Vector4f(Normalizef(Vector3f(frustum_planes[i])), frustum_planes[i].w / length);
//             }
//         }

//         {
//             auto fill_uniform_data = [this, data(view_data)]() {
//                 auto frame_offset = render_context.GetFrameOffset();

//                 auto  c_data = data;
//                 auto* ptr    = g_rhi->RHIMapBuffer(uniform_buffer, frame_offset * uniform_buffer_size, uniform_buffer_size);

//                 std::memcpy(ptr, &data, sizeof(VirtualView));
//                 g_rhi->RHIUnmapBuffer(uniform_buffer);
//             };
//             EnqueueRenderTask(std::move(fill_uniform_data));
//         }

//         RHICopyBufferInfo copy_info{};
//         copy_info.regions.push_back({0, 0, sizeof(uint32_t) * 32});

//         auto&& begin_frame = [this, copy_info = std::move(copy_info)]() {
//             render_context.BeginFrame();

//             render_context.GetRenderGraph().AddCopyPass(
//                 "Copy Uniform Buffer",
//                 [this](RenderGraph::Builder& _builder) {
//                     auto& rg          = render_context.GetRenderGraph();
//                     auto  uni_handle  = rg.ImportIfNotExist("Deferred::UniformView", uniform_buffer);
//                     auto  view_handle = rg.ImportIfNotExist(view_buffer_name, view_buffer);
//                     _builder.ReadBuffer(uni_handle, EBufferRuntimeUsageFlags::TRANSFER_READ);
//                     _builder.WriteBuffer(view_handle, EBufferRuntimeUsageFlags::TRANSFER_WRITE);
//                 },
//                 [this](RenderPassContext& _context) {
//                     auto              src_offset = render_context.GetFrameOffset() * uniform_buffer_size;
//                     RHICopyBufferInfo copy_info{
//                         {src_offset, 0, uniform_buffer_size}};

//                     render_context.GetCommandList().CopyBuffer(copy_info, uniform_buffer, view_buffer);
//                 });
//         };

//         EnqueueRenderTask(std::move(begin_frame));
//         EnqueueRenderTask([this]() {
//             base_pass->Draw(render_context);
//         });

//         // PrePass();

//         // DispatchDrawScene(camera_data);

//         // //hzb build
//         // EnqueueRenderTask(
//         //     [this]() {
//         //         render_context.GetRenderGraph().AddComputePass(
//         //             "Build HZB",
//         //             [&](RenderGraph::Builder& _builder) {
//         //                 // auto frame_offset = frame_counter % render_cmd_lists.size();
//         //                 // auto depth        = GetCurrentRenderGraph().ImportTexture("depth", depth_buffer[frame_offset]);
//         //                 // auto hiz_buffer   = GetCurrentRenderGraph().ImportTexture("hiz_buffer", this->hiz_buffer.texture);
//         //                 // _builder.ReadTexture(depth, ETextureUsageFlags::SAMPLED);
//         //                 // _builder.WriteTexture(hiz_buffer, ETextureUsageFlags::UNORDERED_ACCESS);
//         //             },
//         //             [&](RenderPassContext& _context) {
//         //                 auto frame_offset = render_context.GetFrameOffset();
//         //                 auto depth_buffer = virtual_viewport->GetDepthSRV();
//         //                 HiZBuilder::GetInstance().DispatchBuildHiZ(&render_context.GetCommandList(), depth_buffer, hiz_buffer);
//         //             });
//         //     });
//         // RecheckPass();

//         // DispatchDrawScene(camera_data, false);
//         // EnqueueRenderTask(
//         //     [this]() {
//         //         render_context.GetRenderGraph().AddComputePass(
//         //             "Post Build HZB",
//         //             [&](RenderGraph::Builder& _builder) {
//         //                 // auto depth      = GetCurrentRenderGraph().ImportTexture("depth", depth_buffer[frame_offset]);
//         //                 // auto hiz_buffer = GetCurrentRenderGraph().ImportTexture("hiz_buffer", this->hiz_buffer.texture);
//         //                 // _builder.ReadTexture(depth, ETextureUsageFlags::SAMPLED);
//         //                 // _builder.WriteTexture(hiz_buffer, ETextureUsageFlags::UNORDERED_ACCESS);
//         //             },
//         //             [&](RenderPassContext& _context) {
//         //                 auto depth_buffer = virtual_viewport->GetDepthSRV();
//         //                 HiZBuilder::GetInstance().DispatchBuildHiZ(&render_context.GetCommandList(), depth_buffer, hiz_buffer);
//         //             });
//         //     });
//         LightingPass();

//         //   EnqueueRenderTask([this]() {
//         // auto&                    cmd_list = render_context.GetCommandList();
//         // RHIBarrierDependencyInfo barrier_dependency_info{};
//         // barrier_dependency_info.texture_barriers.resize(1);
//         // auto& attachment_info = barrier_dependency_info.texture_barriers[0];
//         // attachment_info
//         //     .SetTexture(virtual_viewport->GetBackBufferInfo().backbuffer_uav->GetTexture())
//         //     .SetDstTextureLayout(TEXTURE_LAYOUT_TRANSFER_SRC)
//         //     .SetSrcTextureLayout(TEXTURE_LAYOUT_COLOR_ATTACHMENT)
//         //     .SetSrcStage(ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT)
//         //     .SetDstStage(ERHIPipelineStageFlags::PS_TRANSFER)
//         //     .SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE)
//         //     .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_READ);
//         //
//         // cmd_list.SetPipelineBarrier(barrier_dependency_info);
//         //  });
//         {
//             auto submit_rendering = [this]() {
//                 render_context.EndFrame(virtual_viewport);
//             };
//             EnqueueRenderTask(std::move(submit_rendering));
//         }
//     }
//     void DeferredRenderer::Impl::FallBackDraw() {
//         render_context.BeginFrame();
//         auto& cmd_list = render_context.GetCommandList();
//         render_context.EndFrame(virtual_viewport);
//     }

//     void DeferredRenderer::Impl::Present() {
//         EnqueueRenderTask([this]() {
//             render_context.Present(virtual_viewport);
//         });
//     }

//     void DeferredRenderer::Impl::SetOriginResolution(uint32_t _width, uint32_t _height) {
//     }

//     void DeferredRenderer::Impl::SetPresentResolution(uint32_t _width, uint32_t _height) {
//         if (_width == source_resolution.x && _height == source_resolution.y) {
//             return;
//         }
//         source_resolution = Vector2i(_width, _height);
//         EnqueueRenderTask([this]() {
//             OnResizeVSwapChain();
//             base_pass->OnResizeViewport(source_resolution);
//         });
//     }
//     void DeferredRenderer::Impl::UpdateGui() {
//         // return;
//         int item_current = -1;
//         for (size_t i = 0; i < m_current_textures.size(); i++) {
//             if (m_current_textures[i] == m_present_texture) {
//                 item_current = i;
//                 break;
//             }
//         }
//         uint32_t                 size = m_current_textures.size();
//         std::vector<const char*> current_texture_cstr(m_current_textures.size());
//         for (size_t i = 0; i < m_current_textures.size(); i++) {
//             current_texture_cstr[i] = m_current_textures[i].data();
//         }
//         ImGui::Combo("RenderGraphTextures", &item_current, current_texture_cstr.data(), current_texture_cstr.size());
//         if (item_current > 0 && item_current < m_current_textures.size())
//             m_present_texture = m_current_textures[item_current];
//     }

//     RHISRVRef DeferredRenderer::Impl::GetRendererOutput() {
//         return virtual_viewport->GetPresentTextureSRV();
//     }

//     // void DeferredRenderer::Impl::PrePass() {
//     //     uint32_t frame_offset   = render_context.GetFrameOffset();
//     //     auto     instance_count = instance_buffer_view->GetBuffer()->GetNumElement() / sizeof(InstanceData);

//     //     CullInstancePrePassShader::Parameters cull_instance_params;
//     //     cull_instance_params.input.meshlet_count_offset          = meshlet_count_offset;
//     //     cull_instance_params.input.instance_count                = instance_count;
//     //     cull_instance_params.input.hiz_factor                    = Vector2f(hiz_buffer.texture->GetExtent2D());
//     //     cull_instance_params.input.hiz_depth                     = hiz_buffer.texture->GetNumMips();
//     //     cull_instance_params.input.recheck_counter_buffer_offset = check_instance_count_offset;
//     //     cull_instance_params.input.instance_dispatch_offset      = instance_dispatch_indirect_offset;
//     //     cull_instance_params.input.meshlet_dispatch_offset       = meshlet_dispatch_offset;

//     //     cull_instance_params.instance_meshlet_info      = instance_meshlet_info_view;
//     //     cull_instance_params.instance_meshlet_cull_info = instance_meshlet_cull_info_uav;
//     //     cull_instance_params.recheck_instance_id        = recheck_instance_id_uav;
//     //     cull_instance_params.counters_buffer            = draw_count_view;
//     //     cull_instance_params.views                      = uniform_buffer_view[frame_offset];
//     //     cull_instance_params.hiz_depth                  = hiz_buffer.srv;
//     //     cull_instance_params.depth_sampler              = hiz_buffer.sampler;

//     //     CullMeshletPrepassShader::Parameters cull_meshlet_params;
//     //     cull_meshlet_params.input.meshlet_count_offset          = meshlet_count_offset;
//     //     cull_meshlet_params.input.draw_count_offset             = draw_count_offset;
//     //     cull_meshlet_params.input.hiz_factor                    = Vector2f(hiz_buffer.texture->GetExtent2D());
//     //     cull_meshlet_params.input.hiz_depth                     = hiz_buffer.texture->GetNumMips();
//     //     cull_meshlet_params.input.recheck_counter_buffer_offset = check_meshlet_count_offset;
//     //     cull_meshlet_params.meshlet_info_buffer                 = meshlet_descs_buffer_view;
//     //     cull_meshlet_params.meshlet_bound_buffer                = meshlet_bounds_buffer_view;
//     //     cull_meshlet_params.instance_data                       = instance_buffer_view;
//     //     cull_meshlet_params.instance_meshlet_info               = instance_meshlet_info_view;
//     //     cull_meshlet_params.instance_meshlet_cull_info          = instance_meshlet_cull_info_view;
//     //     cull_meshlet_params.recheck_cull_info                   = recheck_cull_info_uav;
//     //     cull_meshlet_params.counters_buffer                     = draw_count_view;
//     //     cull_meshlet_params.command_buffer                      = draw_indirect_view;
//     //     cull_meshlet_params.views                               = uniform_buffer_view[frame_offset];
//     //     cull_meshlet_params.hiz_depth                           = hiz_buffer.srv;
//     //     cull_meshlet_params.depth_sampler                       = hiz_buffer.sampler;

//     //     RHIBatchedShaderParameters cull_instance_batched_params;
//     //     cull_instance_batched_params.SetParameters(cull_instance_prepass_shader, cull_instance_params);

//     //     RHIBatchedShaderParameters cull_meshlet_batched_params;
//     //     cull_meshlet_batched_params.SetParameters(cull_meshlet_prepass_shader, cull_meshlet_params);
//     //     auto prepass_cull_task = [this,
//     //                               instance_params(std::move(cull_instance_batched_params)),
//     //                               meshlet_params(std::move(cull_meshlet_batched_params)),
//     //                               instance_count(instance_count)](
//     //                                  RenderPassContext& _context) mutable {
//     //         RHIGraphicsCommandList* cmd_list = _context.cmd_list;

//     //         RHIBarrierDependencyInfo barrier_dependency_info{};
//     //         auto                     draw_count_buffer_rdg = _context.graph.GetBlackBoard().GetBuffer("count_buffer");
//     //         draw_count_buffer_rdg->ResloveResourceUsage(EBufferLayout::COMMON, barrier_dependency_info, _context.pass_type);
//     //         auto draw_indirect_buffer_rdg = _context.graph.GetBlackBoard().GetBuffer("draw_indirect");
//     //         draw_indirect_buffer_rdg->ResloveResourceUsage(EBufferLayout::COMMON, barrier_dependency_info, _context.pass_type);

//     //         cmd_list->SetPipelineBarrier(barrier_dependency_info);

//     //         cmd_list->SetPipelineState(cull_instance_prepass_pso);
//     //         g_rhi->RHISetBatchedShaderParameters(cull_instance_prepass_pso, instance_params);
//     //         auto dispatch_count = (instance_count + thread_group_count - 1) / thread_group_count;
//     //         cmd_list->Dispatch(dispatch_count, 1, 1);
//     //         {
//     //             RHIBarrierDependencyInfo instance_cull_barrier{};
//     //             auto                     instance_meshlet_cull_info_buffer_rdg = _context.graph.GetBlackBoard().GetBuffer("instance_meshlet_cull_info");
//     //             instance_meshlet_cull_info_buffer_rdg->ResloveResourceUsage(EBufferLayout::COMMON, instance_cull_barrier, _context.pass_type, {});
//     //             auto recheck_instance_id_buffer_rdg = _context.graph.GetBlackBoard().GetBuffer("recheck_instance_id");
//     //             recheck_instance_id_buffer_rdg->ResloveResourceUsage(EBufferLayout::WRITE, instance_cull_barrier, _context.pass_type, {});
//     //             auto draw_count_buffer_rdg = _context.graph.GetBlackBoard().GetBuffer("count_buffer");
//     //             draw_count_buffer_rdg->ResloveResourceUsage(EBufferLayout::INDIRECT_COMMAND_READ, instance_cull_barrier, _context.pass_type, {});
//     //             cmd_list->SetPipelineBarrier(instance_cull_barrier);
//     //         }

//     //         cmd_list->SetPipelineState(cull_meshlet_prepass_pso);
//     //         g_rhi->RHISetBatchedShaderParameters(cull_meshlet_prepass_pso, meshlet_params);
//     //         // cmd_list->Dispatch((max_meshlet_count + thread_group_count) / thread_group_count, 1, 1);
//     //         cmd_list->DispatchIndirect(draw_count_buffer, meshlet_dispatch_offset);
//     //         {
//     //             RHIBarrierDependencyInfo post_compute_barrier{};
//     //             auto                     draw_count_rdg_buffer = _context.graph.GetBlackBoard().GetBuffer("count_buffer");
//     //             draw_count_rdg_buffer->ResloveResourceUsage(EBufferLayout::INDIRECT_COMMAND_READ, post_compute_barrier, _context.pass_type, {});
//     //             auto draw_indirect_rdg_buffer = _context.graph.GetBlackBoard().GetBuffer("draw_indirect");
//     //             draw_indirect_rdg_buffer->ResloveResourceUsage(EBufferLayout::INDIRECT_COMMAND_READ, post_compute_barrier, _context.pass_type, {});
//     //             auto recheck_cull_info_rdg_buffer = _context.graph.GetBlackBoard().GetBuffer("recheck_cull_info");
//     //             recheck_cull_info_rdg_buffer->ResloveResourceUsage(EBufferLayout::READ, post_compute_barrier, _context.pass_type, {});
//     //             cmd_list->SetPipelineBarrier(post_compute_barrier);
//     //         }
//     //     };

//     //     EnqueueRenderTask([&, prepass_cull(std::move(prepass_cull_task))]() {
//     //         render_context.GetRenderGraph().AddComputePass(
//     //             "Prepass Cull Instance",
//     //             [&](RenderGraph::Builder& _builder) {
//     //                 auto counter_buffer      = render_context.GetRenderGraph().GetBlackBoard().GetHandle("count_buffer");
//     //                 auto draw_indirect       = render_context.GetRenderGraph().ImportBuffer("draw_indirect", draw_indirect_buffer);
//     //                 auto recheck_instance_id = render_context.GetRenderGraph().ImportBuffer("recheck_instance_id", recheck_instance_id_buffer);
//     //                 auto instance_meshlet_cull_info =
//     //                     render_context.GetRenderGraph().ImportBuffer("instance_meshlet_cull_info", instance_meshlet_cull_info_buffer);
//     //                 auto recheck_cull_info = render_context.GetRenderGraph().ImportBuffer("recheck_cull_info", recheck_cull_info_buffer);

//     //                 auto zero_buffer_rdg = render_context.GetRenderGraph().GetBlackBoard().GetHandle("zero_buffer");
//     //                 //Avoid cutting these buffers. The logic for reading and writing these buffers in different passes is not set correctly. Currently, it can only be handled in this way.
//     //                 render_context.GetRenderGraph().SetGraphOutput(counter_buffer).SetGraphOutput(draw_indirect).SetGraphOutput(recheck_cull_info).SetGraphOutput(recheck_instance_id).SetGraphOutput(instance_meshlet_cull_info).SetGraphOutput(zero_buffer_rdg);
//     //                 _builder.ReadBuffer(draw_indirect, EBufferLayout::COMMON).ReadBuffer(counter_buffer, EBufferLayout::COMMON).ReadBuffer(recheck_cull_info, READ);
//     //             },
//     //             std::move(prepass_cull));
//     //     });
//     // }

//     // void DeferredRenderer::Impl::RecheckPass() {
//     //     uint32_t frame_offset   = render_context.GetFrameOffset();
//     //     auto     instance_count = instance_buffer_view->GetBuffer()->GetNumElement();

//     //     CullInstanceRecheckShader::Parameters cull_instance_params;
//     //     cull_instance_params.input.meshlet_count_offset          = check_meshlet_count_offset;
//     //     cull_instance_params.input.instance_count                = instance_count;
//     //     cull_instance_params.input.hiz_factor                    = Vector2f(hiz_buffer.texture->GetExtent2D());
//     //     cull_instance_params.input.hiz_depth                     = hiz_buffer.texture->GetNumMips();
//     //     cull_instance_params.input.recheck_counter_buffer_offset = check_instance_count_offset;
//     //     cull_instance_params.input.instance_dispatch_offset      = instance_dispatch_indirect_offset;
//     //     cull_instance_params.input.meshlet_dispatch_offset       = recheck_meshlet_dispatch_offset;
//     //     // cull_instance_params.instance_data                       = instance_buffer_view;
//     //     cull_instance_params.instance_meshlet_info      = instance_meshlet_info_view;
//     //     cull_instance_params.instance_meshlet_cull_info = recheck_cull_info_uav;
//     //     cull_instance_params.recheck_instances          = recheck_instance_id_srv;
//     //     cull_instance_params.counters_buffer            = draw_count_view;
//     //     cull_instance_params.views                      = uniform_buffer_view[frame_offset];
//     //     cull_instance_params.hiz_depth                  = hiz_buffer.srv;
//     //     cull_instance_params.depth_sampler              = hiz_buffer.sampler;

//     //     CullMeshletRecheckShader::Parameters cull_meshlet_params;
//     //     cull_meshlet_params.input.meshlet_count_offset          = meshlet_count_offset;
//     //     cull_meshlet_params.input.draw_count_offset             = recheck_draw_count_offset;
//     //     cull_meshlet_params.input.hiz_factor                    = Vector2f(hiz_buffer.texture->GetExtent2D());
//     //     cull_meshlet_params.input.hiz_depth                     = hiz_buffer.texture->GetNumMips();
//     //     cull_meshlet_params.input.recheck_counter_buffer_offset = check_meshlet_count_offset;
//     //     cull_meshlet_params.meshlet_info_buffer                 = meshlet_descs_buffer_view;
//     //     cull_meshlet_params.meshlet_bound_buffer                = meshlet_bounds_buffer_view;
//     //     cull_meshlet_params.instance_data                       = instance_buffer_view;
//     //     cull_meshlet_params.instance_meshlet_info               = instance_meshlet_info_view;
//     //     cull_meshlet_params.instance_meshlet_cull_info          = recheck_cull_info_view;
//     //     cull_meshlet_params.counters_buffer                     = draw_count_view;
//     //     cull_meshlet_params.command_buffer                      = draw_indirect_view;
//     //     cull_meshlet_params.views                               = uniform_buffer_view[frame_offset];
//     //     cull_meshlet_params.hiz_depth                           = hiz_buffer.srv;
//     //     cull_meshlet_params.depth_sampler                       = hiz_buffer.sampler;

//     //     RHIBatchedShaderParameters cull_instance_batched_params;
//     //     cull_instance_batched_params.SetParameters(cull_instance_recheck_shader, cull_instance_params);

//     //     RHIBatchedShaderParameters cull_meshlet_batched_params;
//     //     cull_meshlet_batched_params.SetParameters(cull_meshlet_recheck_shader, cull_meshlet_params);

//     //     auto recheck_pass_cull_task = [this,
//     //                                    instance_params(std::move(cull_instance_batched_params)),
//     //                                    meshlet_params(std::move(cull_meshlet_batched_params)),
//     //                                    instance_count(instance_count)](
//     //                                       RenderPassContext& _context) mutable {
//     //         auto& cmd_list = render_context.GetCommandList();

//     //         cmd_list.SetPipelineState(cull_instance_recheck_pso);
//     //         g_rhi->RHISetBatchedShaderParameters(cull_instance_recheck_pso, instance_params);
//     //         auto dispatch_count = (instance_count + thread_group_count - 1) / thread_group_count;
//     //         // cmd_list->Dispatch(dispatch_count, 1, 1);
//     //         cmd_list.DispatchIndirect(draw_count_buffer, instance_dispatch_indirect_offset);
//     //         {
//     //             RHIBarrierDependencyInfo meshlet_cull_barrier{};
//     //             auto                     instance_meshlet_cull_info_buffer_rdg = _context.graph.GetBlackBoard().GetBuffer("instance_meshlet_cull_info");
//     //             instance_meshlet_cull_info_buffer_rdg->ResloveResourceUsage(EBufferLayout::COMMON, meshlet_cull_barrier, _context.pass_type, {});
//     //             auto recheck_cull_info_rdg_buffer = _context.graph.GetBlackBoard().GetBuffer("recheck_cull_info");
//     //             recheck_cull_info_rdg_buffer->ResloveResourceUsage(EBufferLayout::READ, meshlet_cull_barrier, _context.pass_type, {});
//     //             auto draw_count_buffer_rdg = _context.graph.GetBlackBoard().GetBuffer("count_buffer");
//     //             draw_count_buffer_rdg->ResloveResourceUsage(EBufferLayout::INDIRECT_COMMAND_READ, meshlet_cull_barrier, _context.pass_type, {});
//     //             cmd_list.SetPipelineBarrier(meshlet_cull_barrier);
//     //         }

//     //         cmd_list.SetPipelineState(cull_meshlet_recheck_pso);
//     //         g_rhi->RHISetBatchedShaderParameters(cull_meshlet_recheck_pso, meshlet_params);

//     //         // cmd_list->Dispatch((max_meshlet_count + thread_group_count) / thread_group_count, 1, 1);
//     //         cmd_list.DispatchIndirect(draw_count_buffer, recheck_meshlet_dispatch_offset);
//     //         {
//     //             RHIBarrierDependencyInfo post_compute_barrier{};
//     //             auto                     draw_count_rdg_buffer = _context.graph.GetBlackBoard().GetBuffer("count_buffer");
//     //             draw_count_rdg_buffer->ResloveResourceUsage(EBufferLayout::INDIRECT_COMMAND_READ, post_compute_barrier, _context.pass_type, {});
//     //             auto draw_indirect_rdg_buffer = _context.graph.GetBlackBoard().GetBuffer("draw_indirect");
//     //             draw_indirect_rdg_buffer->ResloveResourceUsage(EBufferLayout::INDIRECT_COMMAND_READ, post_compute_barrier, _context.pass_type, {});
//     //             cmd_list.SetPipelineBarrier(post_compute_barrier);
//     //         }
//     //     };

//     //     EnqueueRenderTask([&, recheck_pass(std::move(recheck_pass_cull_task))]() {
//     //         render_context.GetRenderGraph().AddComputePass(
//     //             "Recheck Cull Instance",
//     //             [&](RenderGraph::Builder& _builder) {
//     //                 auto draw_count_rdg_buffer = render_context.GetRenderGraph().GetBlackBoard().GetHandle("count_buffer");
//     //                 _builder.WriteBuffer(draw_count_rdg_buffer, COMMON);
//     //                 auto draw_indirect_rdg_buffer = render_context.GetRenderGraph().GetBlackBoard().GetHandle("draw_indirect");
//     //                 _builder.WriteBuffer(draw_indirect_rdg_buffer, COMMON);
//     //                 auto recheck_instance_id_rdg_buffer = render_context.GetRenderGraph().GetBlackBoard().GetHandle("recheck_instance_id");
//     //                 _builder.ReadBuffer(recheck_instance_id_rdg_buffer, READ);
//     //             },
//     //             std::move(recheck_pass));
//     //     });
//     //     // EnqueueRenderTask(std::move(recheck_pass_cull_task));
//     // }

//     // void DeferredRenderer::Impl::DispatchDrawScene(const CameraData& _camera_data, bool _first_pass) {
//     //     auto dispatch_gbuffer_pass = [&, b_first_pass(_first_pass)]() {
//     //         auto& cmd_list = render_context.GetCommandList();

//     //         Extent3D     extent = virtual_viewport->GetBackBufferExtent();
//     //         RenderGraph& rg     = render_context.GetRenderGraph();
//     //         rg.AddGraphicPass(
//     //             "GBuffer Pass",
//     //             [&](RenderGraph::Builder& _builder) {
//     //                 auto normal = rg.CreateTexture("normal", {.extent2D = Extent2D(extent.x, extent.y), .format = EPixelFormat::PF_R8G8B8A8_UNORM, .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED});
//     //                 auto mat    = rg.CreateTexture("mat", {.extent2D = Extent2D(extent.x, extent.y), .format = EPixelFormat::PF_R32_UINT, .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED});
//     //                 auto uv     = rg.CreateTexture("uv", {.extent2D = Extent2D(extent.x, extent.y), .format = EPixelFormat::PF_R16G16_SFLOAT, .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED});
//     //                 auto depth  = rg.ImportTexture("depth", virtual_viewport->GetDepthSRV()->GetTexture());
//     //                 if (!b_first_pass) {
//     //                     _builder.ReadTextures({normal, uv, mat}, RenderGraphTexture::Usage::COLOR_ATTACHMENT);
//     //                     _builder.ReadTexture(depth, RenderGraphTexture::Usage::DEPTH_STENCIL_ATTACHMENT);
//     //                 }
//     //                 _builder.WriteTextures({normal, uv, mat}, RenderGraphTexture::Usage::COLOR_ATTACHMENT);
//     //                 _builder.WriteTexture(depth, RenderGraphTexture::Usage::DEPTH_STENCIL_ATTACHMENT);
//     //                 _builder.DeclareRenderPass({.color_attachments = {
//     //                                                 mat,
//     //                                                 normal,
//     //                                                 uv},
//     //                                             .depth_stencil_attachment = depth});
//     //             },
//     //             [this, b_first_pass](RenderPassContext& _context) {
//     //                 auto* cmd_list = _context.cmd_list;
//     //                 cmd_list->SetPipelineState(gbuffer_pipeline_state);
//     //                 // cmd_list->BindVertexBuffers()
//     //                 auto* scene = g_scene;
//     //                 cmd_list->BindIndexBuffer(scene->GetBuffer("index_buffer"), 0, IET_UINT32);
//     //                 uint32_t           offset[]           = {0, 0};
//     //                 const RHIBufferRef prim_vertex_buffer = scene->GetBuffer("vertex_buffer");
//     //                 const RHIBufferRef instance_id_buffer = scene->GetBuffer("instance_id_buffer");
//     //                 RHIBufferRef       vbuffers[]         = {prim_vertex_buffer, instance_id_buffer};
//     //                 cmd_list->BindVertexBuffers(0, 2, vbuffers, offset);

//     //                 auto camera_entity = g_scene->GetMainCamera();
//     //                 auto camera        = CameraManager::Get().Get(camera_entity);

//     //                 CameraData camera_data;
//     //                 camera_data.view           = camera->GetViewMatrix();
//     //                 camera_data.view_proj      = camera->GetProjectionMatrix() * camera->GetViewMatrix();
//     //                 camera_data.camera_pos     = Vector4f(camera->GetPosition(), 1.f);
//     //                 auto vp                    = camera->GetProjectionMatrix() * camera->GetViewMatrix();
//     //                 camera_data.prev_view_proj = vp;

//     //                 TestGBufferShaderVert::Parameters vert_params;
//     //                 vert_params.camera_data   = camera_data;
//     //                 vert_params.instance_data = instance_buffer_view;

//     //                 TestGBufferShaderFrag::Parameters frag_params;
//     //                 frag_params.instance_data = instance_buffer_view;

//     //                 RHIBatchedShaderParameters batched_params;
//     //                 batched_params.SetParameters(ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderVert>(), vert_params);
//     //                 batched_params.SetParameters(ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderFrag>(), frag_params);

//     //                 g_rhi->RHISetBatchedShaderParameters(gbuffer_pipeline_state, batched_params);

//     //                 auto instance_idx = 0;
//     //                 // scene->ForEach([cmd_list, &instance_idx](Entity entity) {
//     //                 //     auto mesh_info = RenderableManager::Get().GetMeshInfo(entity);
//     //                 //     cmd_list->DrawIndexedInstanced(mesh_info.index_count, 1, mesh_info.index_offset, mesh_info.vertex_offset, instance_idx++);
//     //                 // });
//     //                 auto draw_cnt_offset = b_first_pass ? draw_count_offset : recheck_draw_count_offset;
//     //                 cmd_list->DrawIndexedIndirect(draw_indirect_buffer, 0, draw_count_buffer, draw_cnt_offset, draw_indirect_buffer->GetNumElement(), sizeof(DrawInstanceCmd));
//     //             });
//     //     };
//     //     EnqueueRenderTask(std::move(dispatch_gbuffer_pass));
//     // }
//     void DeferredRenderer::Impl::LightingPass() {
//         auto dispatch_rdg = [&]() {
//             RenderGraph& rg = render_context.GetRenderGraph();
//             rg.AddGraphicPass(
//                 "Lighting Pass", [&rg, this](RenderGraph::Builder& _builder) {
//             auto normal = rg.GetBlackBoard().GetHandle("normal");
//             auto mat    = rg.GetBlackBoard().GetHandle("mat");
//             auto uv     = rg.GetBlackBoard().GetHandle("uv");
//             auto depth  = rg.GetBlackBoard().GetHandle("depth");
//             // auto position = render_graph.GetBlackBoard().GetHandle("position");
//             auto output = rg.ImportTexture("swapchain_output", virtual_viewport->GetBackBufferInfo().backbuffer_uav->GetTexture());
//             _builder.ReadTextures({normal, uv, mat, depth}, TS_SAMPLED).WriteTexture(output);
//             _builder.DeclareRenderPass({.color_attachments =  {output}}); }, [&rg, this](RenderPassContext& context) {
//             auto* cmd_list = context.cmd_list;
//             cmd_list->SetPipelineState(lighting_pipeline_state);

//             //First Get all material instances
//             //Material organize this materials
//             //Material bind all resources for it's pass
//             //Draw a full screen quad pass for each material type
//             Moer::UnorderedSet<EMaterialType> material_types = {};
//             Moer::UnorderedMap<EMaterialType,Moer::Array<MaterialInstanceRef>> material_instances;
//             g_scene->ForEach([&](Entity _entity) {
//                 if (RenderableManager::Get().Contains(_entity)) {

//                     // auto mi = RenderableManager::Get().GetMaterialInstance(_entity);
//                     // material_types.insert(mi->GetMaterial()->GetType());
//                     // material_instances[mi->GetMaterial()->GetType()].push_back(mi);
//                 }
//             });

//             auto mat_srv    = rg.GetBlackBoard().GetTexture("mat")->GetSRV();
//             auto normal_srv = rg.GetBlackBoard().GetTexture("normal")->GetSRV();
//             auto uv_srv     = rg.GetBlackBoard().GetTexture("uv")->GetSRV();
//             auto depth_srv  = rg.GetBlackBoard().GetTexture("depth")->GetSRV();

//             Camera*      camera = CameraManager::Get().Get(g_scene->GetMainCamera());
//             LightingData lighting_data;
//             lighting_data.inv_view_proj = Inverse(camera->GetProjectionMatrix() * camera->GetViewMatrix());
//             lighting_data.light_count   = lights.size();
//             lighting_data.camera_position = camera->GetPosition();

//             for(auto type : material_types){
//                 RHIBatchedShaderParameters parameters;
//                 LightingShaderFrag::Parameters frag_params;
//                 frag_params.lighting_data = lighting_data;
//                 // frag_params.material_data = g_scene->GetB;
//                 frag_params.light_data = light_buffer_view;
//                 frag_params.depth_attach = depth_srv;
//                 frag_params.gbuffer_uv = uv_srv;
//                 frag_params.normal_attach = normal_srv;
//                 frag_params.mat_attach = mat_srv;
//                 frag_params.default_sampler = sampler;
//                 parameters.SetParameters(ShaderResourceManager::GetInstance().GetShader<LightingShaderFrag>(),frag_params);

//                 auto& mat_instances = material_instances[type];
//                 MaterialRef material = mat_instances[0]->GetMaterial();
//                 material->OrganizeInstancesAndBind(parameters,mat_instances);
//                 g_rhi->RHISetBatchedShaderParameters(lighting_pipeline_state,parameters);

//                 cmd_list->Draw(3, 1,0,0);
//             } });

//             rg.SetGraphOutput(rg.GetBlackBoard().GetHandle("swapchain_output"));
//             m_current_textures = rg.GetResourceNames(RenderGraphResource::Type::Texture2D);
//             rg.AddImageCopyPass("Copy to BackBuffer", rg.GetBlackBoard().GetHandle(m_present_texture), rg.GetBlackBoard().GetHandle("swapchain_output"));
//             rg.Execute({&render_context.GetCommandList(), virtual_viewport->GetBackBufferExtent()});
//         };

//         EnqueueRenderTask(std::move(dispatch_rdg));//not ready yet
//     }

// }// namespace Moer