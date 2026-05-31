#include "RasterRenderer.h"

#include "../../../../editor/raster_ui/RasterUI.h"
#include "AaPass.h"
#include "AoPass.h"
#include "BilateralFilterDenoiserPass.h"
#include "BloomPass.h"
#include "CooperativeOpsPass.h"
#include "DirectionalShadowMaskPass.h"
#include "GeometryPass.h"
#include "LightingPass.h"
#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"
#include "RtaoDenoiserPass.h"
#include "ShadowDepthPass.h"
#include "SkyboxPass.h"
#include "SsrPass.h"
#include "TonemappingPass.h"
#include "debug/RenderDocApi.h"
#include "misc/Timer.h"
#include "scene/LogicalComponents.h"
#include "trace/Trace.h"
#include "window/WindowContext.h"

#if WITH_CUDA
#include "CudaPass.h"
#include "TensorRTPass.h"
#endif

namespace Moer::Render::Raster {

RasterRenderer::RasterRenderer(
    uint2&                        _resolution,
    const SharedPtr<EditorConfig> _config,
    const EngineHooks&            _hooks,
    ::Moer::RuntimeAssets&        _runtime_assets
) :
    // Super
    Renderer(_resolution, _config, _hooks, _runtime_assets) {

    raster_context_ptr =
        MakeUnique<RasterContext>(device, manager, gfx_queue, bindless_array, cmd_list, scene, resolution);
    auto& raster_context = *raster_context_ptr;

    raster_context.CreateFrameBuffers();
    raster_context.UploadExternalFrameBuffers();
    raster_context.AllocateFrameBuffers();

    {
        Array<CommandList> init_cmd_lists{};
        init_cmd_lists.emplace_back(std::move(cmd_list));
        RHIExecutor::Get().Submit(std::move(init_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
        cmd_list = CommandList(EQueueType::Graphics);
    }

    shadow_depth_pass            = MakeUnique<ShadowDepthPass>(raster_context);
    directional_shadow_mask_pass = MakeUnique<DirectionalShadowMaskPass>(raster_context);
    geometry_pass                = MakeUnique<GeometryPass>(raster_context);
    lighting_pass                = MakeUnique<LightingPass>(raster_context);
    skybox_pass                  = MakeUnique<SkyboxPass>(raster_context);
    ao_pass                      = MakeUnique<AoPass>(raster_context);
    rtao_denoiser_pass           = MakeUnique<RtaoDenoiserPass>(raster_context);
    bfd_pass                     = MakeUnique<BilateralFilterDenoiserPass>(raster_context);
    ssr_pass                     = MakeUnique<SsrPass>(raster_context);
    cooperative_ops_pass         = MakeUnique<CooperativeOpsPass>(raster_context);
    aa_pass                      = MakeUnique<AaPass>(raster_context);
    bloom_pass                   = MakeUnique<BloomPass>(raster_context);
    tonemapping_pass             = MakeUnique<TonemappingPass>(raster_context);

#if WITH_CUDA
    // 固定CudaPass位于AoPass之后（需要保证AoPass必定往 ao_output 中写入数据
    cuda_pass      = MakeUnique<CudaPass>(raster_context, raster_context.textures.ao_output.tex);
    tensor_rt_pass = MakeUnique<TensorRTPass>(
        raster_context,
        raster_context.textures.ao_output_ambient_only.tex,
        raster_context.textures.depth_linear_sampler.tex->CastToTextureRef(),
        // 下面这个color，需要传入lighting_output，而非ao_output，因为模型的输入需要不带ao的color
        raster_context.textures.lighting_output.tex,
        raster_context.textures.camera_motion_vector.tex,
        raster_context.textures.ao_output_ambient_only_1.tex
    );
#endif

    cmd_list.Barriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Graphics,
        ETrackedStateUpdateMode::Update,
        WriteBindlessArray{bindless_array, EBufferState::UNORDERED_ACCESS}
    );
    cmd_list.UpdateBindlessArray(bindless_array);
    cmd_list.Barriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Graphics,
        ETrackedStateUpdateMode::Update,
        ReadBindlessArray{bindless_array, EBufferState::SHADER_RESOURCE}
    );
    {
        Array<CommandList> init_cmd_lists{};
        init_cmd_lists.emplace_back(std::move(cmd_list));
        RHIExecutor::Get().Submit(std::move(init_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
        cmd_list = CommandList(EQueueType::Graphics);
    }

    // Other vars

    LOG_INFO(
        MOER_TEXT("Cooperative Matrix & Vector Extensions is Enabled: {}"),
        device.IsExtensionCooperativeEnabled() ? "Yes" : "No"
    );
}

RasterRenderer::~RasterRenderer() {
    ReleaseRasterResources();
    ReleaseResources();
}

void RasterRenderer::ReleaseRasterResources() {
    if (raster_resources_released) {
        return;
    }
    raster_resources_released = true;

    if (time > 0) {
        timeline->Wait(time);
        gfx_queue.Sync();
    }

    raster_context_ptr->FreeFrameBuffers(true);

#if WITH_CUDA
    tensor_rt_pass.reset();
    cuda_pass.reset();
#endif
    tonemapping_pass.reset();
    bloom_pass.reset();
    aa_pass.reset();
    cooperative_ops_pass.reset();
    ssr_pass.reset();
    bfd_pass.reset();
    rtao_denoiser_pass.reset();
    ao_pass.reset();
    skybox_pass.reset();
    lighting_pass.reset();
    geometry_pass.reset();
    directional_shadow_mask_pass.reset();
    shadow_depth_pass.reset();
}

void RasterRenderer::Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    ::Moer::RasterUI config_ui(editor_config->raster_config);
    if (hooks.on_register_renderer_config_section) {
        hooks.on_register_renderer_config_section(
            "Raster",
            "Settings",
            [&config_ui](Synapse::Context& ui) {
                config_ui.ShowConfig(ui);
            }
        );
    }

    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        if (!RunSingle(editor_config, hooks, config_ui)) {
            break;
        }
    }

    ReleaseRasterResources();
    ReleaseResources();

    if (hooks.on_unregister_renderer_config_section) {
        hooks.on_unregister_renderer_config_section("Raster", "Settings");
    }
}

void RasterRenderer::UpdateGlobalLightingData(
    RasterContext&      context,
    const RasterConfig& ui_config,
    const Camera&       camera
) {
    uint          csm_layers    = ui_config.shadow_csm_num_of_cascades;
    LightingData* lighting_data = &context.lighting_data;

    lighting_data->clip2world      = Transpose(camera.GetViewProjectionMatrixInv());
    lighting_data->light_count     = context.scene.cpu_scene().GetLightCount();
    lighting_data->camera_position = camera.GetPosition();

    // Shadow Parameters
    lighting_data->shadow_map_mode              = static_cast<int>(ui_config.shadow_map_mode);
    lighting_data->shadow_sampling_mode         = ui_config.shadow_sampling_mode;
    lighting_data->shadow_csm_num_of_cascades   = csm_layers;
    lighting_data->shadow_csm_sm_size           = ui_config.shadow_csm_sm_size;
    lighting_data->shadow_csm_visualize_cascade = ui_config.shadow_csm_visualize_cascade;

    // Shadow Map
    for (uint i = 0; i < csm_layers; i++) {
        lighting_data->cascade_shadow_map[i] = context.csm_data.shadow_map_textures[i].hdl;
    }
    lighting_data->point_shadow_map = context.point_shadow_data.shadow_cubes[0].handle;
    lighting_data->light_pos        = context.point_shadow_data.shadow_cubes[0].light_pos;
    lighting_data->light_radius     = context.point_shadow_data.shadow_cubes[0].far_plane;

    // Shadow Transform
    for (uint i = 0; i < csm_layers; i++) {
        lighting_data->world2shadow_clip[i] = Transpose(lighting_data->world2shadow_clip[i]);
    }
    lighting_data->world2view = Transpose(camera.GetViewMatrix());
    lighting_data->near_clip  = camera.GetNearClip();
    lighting_data->far_clip   = camera.GetFarClip();

    lighting_data->is_csm_blend_enabled = ui_config.shadow_csm_blend_option ? 1 : 0;
    // 注：此处不一定使用所有CSM，Shader中具体根据shadow_csm_num_of_cascades来决定

    // PCSS
    lighting_data->light_size_world = ui_config.shadow_pcss_light_size_world; //假定的光源大小，用于软阴影计算
    lighting_data->pcss_enabled     = ui_config.shadow_pcss_enabled ? 1 : 0;

    // BRDF
    {
        lighting_data->lut_ggx_emu_handle  = context.textures.lut_ggx_emu.hdl;
        lighting_data->lut_ggx_eavg_handle = context.textures.lut_ggx_eavg.hdl;

        lighting_data->brdf_enable_multi_scatter = ui_config.shading_brdf_enable_multi_scatter ? 1 : 0;
        lighting_data->brdf_NDF_mode             = static_cast<uint>(ui_config.shading_brdf_NDF_mode);
        lighting_data->brdf_G_mode               = static_cast<uint>(ui_config.shading_brdf_G_mode);
        lighting_data->brdf_G_is_ibl             = ui_config.shading_brdf_G_is_ibl ? 1 : 0;
    }

    context.cmd_list.CopyFrom(
        std::span<byte>((byte*)lighting_data, sizeof(LightingData)),
        context.lighting_data_buffer.buf->GetView()
    );
}

bool RasterRenderer::RunSingle(
    const SharedPtr<EditorConfig> editor_config,
    const EngineHooks& hooks,
    ::Moer::RasterUI& config_ui
) {
    TRACE_SCOPE_CAT("Raster.Frame", "Frame");
    auto& raster_context = *raster_context_ptr;

    PumpAsyncLoads();

    LogSceneLoadStatus(*editor_config);

    // MARK: 1. Tick Window
    auto window_state = TickWindowContext(hooks);

    bool skip_present = false;
    if (window_state == EWindowState::Hiding) {
        std::this_thread::yield(); // FIXME: 这个东西有用吗？
        skip_present = true;

    } else if (window_state == EWindowState::SizeChanged) {
        LOG_INFO(MOER_TEXT("Size Changed."));

        raster_context.FreeFrameBuffers(false);
        raster_context.CreateFrameBuffers();
        // raster_context.UploadExternalFrameBuffers(); // 窗口大小变化时，外部纹理不会变化，所以不需要上传
        raster_context.AllocateFrameBuffers();

#if WITH_CUDA
        tensor_rt_pass->RecreateResource(
            raster_context,
            raster_context.textures.ao_output_ambient_only.tex,
            raster_context.textures.depth_linear_sampler.tex->CastToTextureRef(),
            // 下面这个color，需要传入lighting_output，而非ao_output，因为模型的输入需要不带ao的color
            raster_context.textures.lighting_output.tex,
            raster_context.textures.camera_motion_vector.tex,
            raster_context.textures.ao_output_ambient_only_1.tex
        );
#endif

        config_ui.RegisterFrameBuffers(raster_context.GetDisplayableFrameBuffersView());

    } else if (window_state == EWindowState::Default) {
        // do nothing

    } else {
        assert(false);
    }

    // MARK: 2. Tick UI
    if (hooks.on_tick_ui) {
        hooks.on_tick_ui();
    }

    config_ui.RegisterFrameBuffers(raster_context.GetDisplayableFrameBuffersView());

    TextureRef default_output_texture = raster_context.textures.output.tex;
    bool       gui_rendered           = false;
    Array<CommandList> pre_frame_cmd_lists{};

    // MARK: 3. Run Render Passes

    if (scene.IsReady()) {

        // Submit scene-loading uploads before frame graph execution.
        auto&& scene_cmd_list = scene.PopPendingCommandList();
        if (!scene_cmd_list.gfx_queue_cmd_list.IsEmpty()) {
            pre_frame_cmd_lists.emplace_back(std::move(scene_cmd_list.gfx_queue_cmd_list));
        }

        if (first_load) {
            first_load = false;

            cmd_list.Barriers(
                EQueueType::Graphics,
                EQueueType::Graphics,
                EPassType::Graphics,
                ETrackedStateUpdateMode::Update,
                WriteBindlessArray{bindless_array, EBufferState::UNORDERED_ACCESS}
            );
            cmd_list.UpdateBindlessArray(bindless_array);
            cmd_list.Barriers(
                EQueueType::Graphics,
                EQueueType::Graphics,
                EPassType::Graphics,
                ETrackedStateUpdateMode::Update,
                ReadBindlessArray{bindless_array, EBufferState::SHADER_RESOURCE}
            );

            if (!cmd_list.IsEmpty()) {
                pre_frame_cmd_lists.emplace_back(std::move(cmd_list));
                cmd_list = CommandList(EQueueType::Graphics);
            }
        }

        auto& raster_config = editor_config->raster_config;
        auto& camera        = scene.GetMainCamera().camera;

        {
            // Jitter Camera for SMAA T2x
            static uint8_t smaa_current_frame_index = 0;
            if (raster_config.aa_mode == EAaMode::SMAA_T2X) {

                smaa_current_frame_index ^= 1;
                static StaticArray<float2, 2> smaa_jitter = {float2(0.25f, -0.25f), float2(-0.25f, 0.25f)};
                camera.SetJitterMatrix(smaa_jitter[smaa_current_frame_index]);
            }
        }

        camera.Tick(editor_config);

        raster_context.Update(camera.GetDeltaTime());

        // others
        // FIXME: 统一update scene
        // scene.GetGpuScene().UpdateRaytracingScene(cmd_list);

        // 当启用视锥剔除时，先恢复完整的 draw commands，
        // 确保 ShadowDepthPass 使用未被上一帧剔除的完整场景数据
        if (raster_config.enable_frustum_culling) {
            raster_context.scene.RestoreDrawCommands(raster_context.cmd_list);
        }

        // Shadow Depth Pass
        cmd_list.PushScopeWithTimeScope(RasterTool::GetShadowDepthPassProfileScopeName());
        shadow_depth_pass->Process(raster_context, raster_config, camera);
        cmd_list.PopScopeWithTimeScope();

        // Update Global Lighting Data
        UpdateGlobalLightingData(raster_context, raster_config, camera);

        // Geometry Pass
        cmd_list.PushScopeWithTimeScope(RasterTool::GetGeometryPassProfileScopeName());
        geometry_pass->Process(raster_context, raster_config, camera);
        cmd_list.PopScopeWithTimeScope();

        // Directional Shadow Mask Pass
        cmd_list.PushScopeWithTimeScope(MOER_TEXT("Directional Shadow Mask Pass"));
        directional_shadow_mask_pass->Process(raster_context, raster_config, camera);
        cmd_list.PopScopeWithTimeScope();

        // Lighting Pass
        cmd_list.PushScopeWithTimeScope(MOER_TEXT("Lighting Pass"));
        lighting_pass->Process(raster_context, raster_config, camera);
        cmd_list.PopScopeWithTimeScope();

        //Env&Atmo Pass
        cmd_list.PushScopeWithTimeScope(MOER_TEXT("Skybox Pass"));
        skybox_pass->Process(raster_context, raster_config, camera);
        cmd_list.PopScopeWithTimeScope();

        // Post Process Passes
        // - Ambient Occlusion
        cmd_list.PushScopeWithTimeScope(MOER_TEXT("Ambient Occlusion Pass"));
        auto ao_result = ao_pass->Process(raster_context, raster_config, camera, time);
        cmd_list.PopScopeWithTimeScope();

        cmd_list.PushScopeWithTimeScope(MOER_TEXT("RTAO Denoiser Pass"));
        rtao_denoiser_pass->ProcessInPlace(raster_context, raster_config, ao_result.ao_only_idx);
        cmd_list.PopScopeWithTimeScope();
        cmd_list.PushScopeWithTimeScope(MOER_TEXT("Ambient Occlusion Composite Pass"));
        ao_pass->CompositeAo(raster_context, raster_config, ao_result.ao_only);
        cmd_list.PopScopeWithTimeScope();

        TextureWithHandle processing_image = raster_context.textures.ao_output;

        // - CUDA Pass
#if WITH_CUDA
        if (raster_config.ai_is_cuda_enabled) {
        cmd_list.PushScopeWithTimeScope(MOER_TEXT("TensorRT Pass"));
            processing_image = tensor_rt_pass->Process(raster_context, raster_config, ao_result.ao_only_idx);
        cmd_list.PopScopeWithTimeScope();
        }
#endif

        // - Denoiser Pass (Bilateral Filter)
    cmd_list.PushScopeWithTimeScope(MOER_TEXT("Bilateral Denoiser Pass"));
        processing_image = bfd_pass->Process(raster_context, raster_config, processing_image);
    cmd_list.PopScopeWithTimeScope();

        // - Screen Space Reflection
    cmd_list.PushScopeWithTimeScope(MOER_TEXT("Screen Space Reflection Pass"));
        processing_image = ssr_pass->Process(raster_context, raster_config, camera, processing_image);
    cmd_list.PopScopeWithTimeScope();

        // - Cooperative Ops
    cmd_list.PushScopeWithTimeScope(MOER_TEXT("Cooperative Ops Pass"));
        processing_image = cooperative_ops_pass->Process(raster_context, raster_config, processing_image);
    cmd_list.PopScopeWithTimeScope();

        // - Anti-aliasing
    cmd_list.PushScopeWithTimeScope(MOER_TEXT("Anti Aliasing Pass"));
        processing_image = aa_pass->Process(raster_context, raster_config, camera, processing_image);
    cmd_list.PopScopeWithTimeScope();

        // - Bloom Pass
    cmd_list.PushScopeWithTimeScope(MOER_TEXT("Bloom Pass"));
        processing_image = bloom_pass->Process(raster_context, raster_config, processing_image);
    cmd_list.PopScopeWithTimeScope();

        // - Tonemapping Pass
    cmd_list.PushScopeWithTimeScope(MOER_TEXT("Tonemapping Pass"));
        processing_image = tonemapping_pass->Process(raster_context, raster_config, processing_image);
    cmd_list.PopScopeWithTimeScope();

        TextureView scene_output = raster_context.GetSelectedFrameBufferView(raster_config.selected_frame_buffer_index);
        if (editor_config->play_mode_enabled) {
            default_output_texture = scene_output.GetTexture();
        } else {
            if (hooks.on_publish_scene_output) {
                hooks.on_publish_scene_output(scene_output);
            }
            if (hooks.on_render_gui) {
                cmd_list.Barriers(
                    {BarrierCreateInfo::Transition(
                        raster_context.textures.output.tex->GetView(),
                        MakeBarrierState(ETextureState::TRANSFER_SRC, EPassType::Copy),
                        MakeBarrierState(ETextureState::TRANSFER_DST, EPassType::Copy)
                    )},
                    EQueueType::Graphics,
                    EQueueType::Graphics,
                    ETrackedStateUpdateMode::Update
                );
                cmd_list.ClearResource(raster_context.textures.output.tex->GetView(), float4(0.f, 0.f, 0.f, 1.f));
                cmd_list.Barriers(
                    {BarrierCreateInfo::Transition(
                        raster_context.textures.output.tex->GetView(),
                        MakeBarrierState(ETextureState::TRANSFER_DST, EPassType::Copy),
                        MakeBarrierState(ETextureState::RENDER_TARGET, EPassType::Graphics)
                    )},
                    EQueueType::Graphics,
                    EQueueType::Graphics,
                    ETrackedStateUpdateMode::Update
                );
                hooks.on_render_gui(cmd_list, raster_context.textures.output.tex);
                cmd_list.Barriers(
                    {BarrierCreateInfo::Transition(
                        raster_context.textures.output.tex->GetView(),
                        MakeBarrierState(ETextureState::RENDER_TARGET, EPassType::Graphics),
                        MakeBarrierState(ETextureState::TRANSFER_SRC, EPassType::Copy)
                    )},
                    EQueueType::Graphics,
                    EQueueType::Graphics,
                    ETrackedStateUpdateMode::Update
                );
                default_output_texture = raster_context.textures.output.tex;
                gui_rendered = true;
            }
        }

        // without test
        // FIXME: 统一advance frame
        // scene.GetRaytracingScene()->AdvanceFrame();

        // debug
        if (raster_config.debug_fps_limit_enable) {
            // sleep 1.0 / fps seconds
            // 虽然不精确，但简单
            std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / raster_config.debug_fps_limit));
            LOG_DEBUG(MOER_TEXT("FPS Limit Enabled: {}"), raster_config.debug_fps_limit);
        }
    }

    // 因为目前Vulkan的输出信息会聚合后再print，所以我们需要轮询，打印出最后添加的信息
    device.FlushDebugMessages();

    if (!editor_config->play_mode_enabled && hooks.on_render_gui && !gui_rendered) {
        cmd_list.Barriers(
            {BarrierCreateInfo::Transition(
                default_output_texture->GetView(),
                MakeBarrierState(ETextureState::TRANSFER_SRC, EPassType::Copy),
                MakeBarrierState(ETextureState::TRANSFER_DST, EPassType::Copy)
            )},
            EQueueType::Graphics,
            EQueueType::Graphics,
            ETrackedStateUpdateMode::Update
        );
        cmd_list.ClearResource(default_output_texture->GetView(), float4(0.f, 0.f, 0.f, 1.f));
        cmd_list.Barriers(
            {BarrierCreateInfo::Transition(
                default_output_texture->GetView(),
                MakeBarrierState(ETextureState::TRANSFER_DST, EPassType::Copy),
                MakeBarrierState(ETextureState::RENDER_TARGET, EPassType::Graphics)
            )},
            EQueueType::Graphics,
            EQueueType::Graphics,
            ETrackedStateUpdateMode::Update
        );
        hooks.on_render_gui(cmd_list, default_output_texture);
        cmd_list.Barriers(
            {BarrierCreateInfo::Transition(
                default_output_texture->GetView(),
                MakeBarrierState(ETextureState::RENDER_TARGET, EPassType::Graphics),
                MakeBarrierState(ETextureState::TRANSFER_SRC, EPassType::Copy)
            )},
            EQueueType::Graphics,
            EQueueType::Graphics,
            ETrackedStateUpdateMode::Update
        );
    }

    time++;
    RHIPresentRequest present_request = presentation_surface->CreatePresentRequest(default_output_texture);
    cmd_list.Signal(timeline, time).DeleteResources().TickFrame();
    const bool should_close_now = WindowContext::ShouldClose(WindowContext::GetMainWindow());
    if (should_close_now) {
        skip_present = true;
    }

    Array<CommandList> frame_cmd_lists = std::move(pre_frame_cmd_lists);
    frame_cmd_lists.emplace_back(std::move(cmd_list));
    RHIExecutor::Get().Submit(
        std::move(frame_cmd_lists),
        ERHIExecSubmitFlags::FlushGPU,
        skip_present ? nullptr : &present_request
    );
    cmd_list = CommandList(EQueueType::Graphics);

    if (!skip_present && !editor_config->play_mode_enabled && hooks.on_present_windows) {
        hooks.on_present_windows();
    }

    if (should_close_now) {
        return false;
    }

    if (hooks.on_is_need_reload && hooks.on_is_need_reload()) {
        return false; // break
    }

    return true;
}

} // namespace Moer::Render::Raster
