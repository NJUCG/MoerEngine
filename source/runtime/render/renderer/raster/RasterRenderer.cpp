#include "RasterRenderer.h"

#include "AaPass.h"
#include "AoPass.h"
#include "BilateralFilterDenoiserPass.h"
#include "BloomPass.h"
#include "CameraGizmoPass.h"
#include "CooperativeOpsPass.h"
#include "CsmGizmoPass.h"
#include "DirectionalShadowMaskPass.h"
#include "GeometryPass.h"
#include "HiZBuildPass.h"
#include "LightingPass.h"
#include "ProbeGizmoPass.h"
#include "ProbeUpdatePass.h"
#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"
#include "RtaoDenoiserPass.h"
#include "ShadowDepthPass.h"
#include "SkyboxPass.h"
#include "SsrPass.h"
#include "TonemappingPass.h"
#include "debug/RenderDocApi.h"
#include "misc/ScopedLogTimer.h"
#include "scene/testcase/SceneTestCaseDispatcher.h"
#include "scene/testcase/SceneTestCaseRunner.h"
#include "window/WindowContext.h"

#include <chrono>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

#if WITH_CUDA
#include "CudaPass.h"
#include "TensorRTPass.h"
#endif

namespace Moer::Render::Raster {

namespace {

float GetElapsedTimeSeconds() {
    static const auto s_start_time = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - s_start_time).count();
}

} // namespace

RasterRenderer::RasterRenderer(
    uint2                         _resolution,
    const SharedPtr<EditorConfig> _config
) :
    // Super
    Renderer(_resolution, _config) {
    ScopedLogTimer startup_timer("[Startup][RasterRenderer] RasterRenderer::Constructor() total");

    raster_context_ptr = MakeUnique<RasterContext>(
        device, manager, gfx_queue, bindless_array, cmd_list, *render_scene, resolution
    );
    auto& raster_context = *raster_context_ptr;

    raster_context.CreateFrameBuffers();
    raster_context.UploadExternalFrameBuffers();
    raster_context.AllocateFrameBuffers();

    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    hiz_build_pass               = MakeUnique<HiZBuildPass>(raster_context);
    shadow_depth_pass            = MakeUnique<ShadowDepthPass>(raster_context);
    directional_shadow_mask_pass = MakeUnique<DirectionalShadowMaskPass>(raster_context);
    probe_update_pass            = MakeUnique<ProbeUpdatePass>(raster_context);
    csm_gizmo_pass               = MakeUnique<CsmGizmoPass>(raster_context);
    probe_gizmo_pass             = MakeUnique<ProbeGizmoPass>(raster_context);
    camera_gizmo_pass            = MakeUnique<CameraGizmoPass>(raster_context);
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

    cmd_list.UpdateBindlessArray(bindless_array);
    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    // Other vars

    LOG_INFO(
        "Cooperative Matrix & Vector Extensions is Enabled: {}",
        device.IsExtensionCooperativeEnabled() ? "Yes" : "No"
    );
}

RasterRenderer::~RasterRenderer() {
    LOG_INFO(
        "[Threading] RasterRenderer destruction started on {} Thread.",
        IsCurrentlyRenderThread() ? "Render" : "Game"
    );
    auto& raster_context = *raster_context_ptr;
    raster_context.FreeFrameBuffers(true);

    // 下面这段需要在Renderer子类中执行。否则各种Pass对象被Release的时候，对应的资源还没有被释放
    ReleaseResources();
    LOG_INFO(
        "[Threading] RasterRenderer destruction finished on {} Thread.",
        IsCurrentlyRenderThread() ? "Render" : "Game"
    );
}

void RasterRenderer::Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        if (!RunSingle(editor_config, hooks)) {
            break;
        }
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
    lighting_data->light_count     = context.GetSceneUpdates().light_count;
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

    context.probe_volume.FillLightingData(*lighting_data);

    if (ui_config.probe_gi_enabled) {
        const uint resident_bricks  = context.probe_volume.GetResidentBrickCount();
        const uint scheduled_bricks = context.probe_volume.GetScheduledBrickCount();
        const uint deferred_bricks  = resident_bricks > scheduled_bricks ? resident_bricks - scheduled_bricks : 0u;
        std::ostringstream stream;
        stream << "[ProbeGI] Runtime lighting params: enabled=" << lighting_data->probe_system_config.x
               << " debug=" << lighting_data->probe_system_config.y
               << " probe_buffer=" << lighting_data->probe_system_config.z
               << " volume_buffer=" << lighting_data->probe_system_config.w
               << " visibility_atlas=" << lighting_data->probe_system_atlas.x
               << " irradiance_atlas=" << lighting_data->probe_system_atlas.y
               << " irradiance_texture=" << lighting_data->probe_system_atlas.z
               << " visibility_texture=" << lighting_data->probe_system_atlas.w
               << " volumes=" << lighting_data->probe_system_counts.x
               << " probes=" << lighting_data->probe_system_counts.y
               << " brick_buffer=" << lighting_data->probe_system_counts.z
               << " page_table=" << lighting_data->probe_system_counts.w
               << " cell_buffer=" << lighting_data->probe_system_hierarchy.x
               << " cells=" << lighting_data->probe_system_hierarchy.y
               << " max_level=" << lighting_data->probe_system_hierarchy.z
               << " layout_generation=" << lighting_data->probe_system_hierarchy.w
               << " base_probes=" << context.probe_volume.GetProbeCount()
               << " hierarchy_probes=" << context.probe_volume.GetHierarchyProbeCount()
               << " requested_bricks=" << context.probe_volume.GetRequestedBrickCount() << "/"
               << context.probe_volume.GetBrickCount()
               << " requested_probes=" << context.probe_volume.GetRequestedProbeCount() << "/"
               << context.probe_volume.GetHierarchyProbeCount()
               << " resident_bricks=" << resident_bricks << "/"
               << context.probe_volume.GetBrickCount()
               << " resident_probes=" << context.probe_volume.GetResidentProbeCount() << "/"
               << context.probe_volume.GetHierarchyProbeCount()
               << " published_bricks=" << context.probe_volume.GetPublishedBrickCount()
               << " pending_load_bricks=" << context.probe_volume.GetPendingLoadBrickCount()
               << " cached_bricks=" << context.probe_volume.GetCachedBrickCount()
               << " physical_allocations=" << context.probe_volume.GetPhysicalAllocationCount()
               << " physical_probes=" << context.probe_volume.GetAllocatedPhysicalProbeCount() << "/"
               << context.probe_volume.GetPhysicalAllocatorCapacity()
               << " target_physical_capacity=" << context.probe_volume.GetPhysicalProbeCapacity()
               << " free_physical_probes=" << context.probe_volume.GetFreePhysicalProbeCount()
               << " retiring_allocations=" << context.probe_volume.GetRetiringAllocationCount()
               << " retiring_probes=" << context.probe_volume.GetRetiringProbeCount()
               << " capacity_evicted_bricks=" << context.probe_volume.GetCapacityEvictedBrickCount()
               << " streaming=" << (ui_config.probe_gi_streaming_enabled ? 1 : 0)
               << " stream_budgets=(" << ui_config.probe_gi_streaming_load_budget << ", "
               << ui_config.probe_gi_streaming_eviction_budget << ")"
               << " loaded_bricks=" << context.probe_volume.GetStreamingLoadedBrickCount()
               << " evicted_bricks=" << context.probe_volume.GetStreamingEvictedBrickCount()
               << " reclaimed_allocations="
               << context.probe_volume.GetStreamingReclaimedAllocationCount()
               << " allocation_stalls=" << context.probe_volume.GetStreamingAllocationStallCount()
               << " clipmap_volumes=" << context.probe_volume.GetClipmapVolumeCount()
               << " clipmap_scrolled=" << context.probe_volume.GetClipmapScrolledVolumeCount()
               << " prefetched_bricks=" << context.probe_volume.GetPrefetchedBrickCount()
               << " reused_l0_bricks=" << context.probe_volume.GetClipmapReusedBrickCount()
               << " prefetch=" << (ui_config.probe_gi_motion_prefetch_enabled ? 1 : 0)
               << " prefetch_threshold=" << ui_config.probe_gi_motion_prefetch_threshold
               << " prefetch_keep_frames=" << ui_config.probe_gi_motion_prefetch_keep_frames
               << " scheduler=" << (ui_config.probe_gi_update_scheduler_enabled ? 1 : 0)
               << " update_budget=" << ui_config.probe_gi_update_brick_budget
               << " scheduled_bricks=" << scheduled_bricks << "/" << resident_bricks
               << " scheduled_probes=" << context.probe_volume.GetScheduledProbeCount() << "/"
               << context.probe_volume.GetResidentProbeCount()
               << " deferred_bricks=" << deferred_bricks
               << " dirty_tracking=" << (ui_config.probe_gi_dirty_tracking_enabled ? 1 : 0)
               << " dirty_regions=" << context.probe_volume.GetDirtyRegionCount()
               << " global_dirty=0x" << std::hex << context.probe_volume.GetGlobalDirtyReasons() << std::dec
               << " dirty_bricks=" << context.probe_volume.GetDirtyBrickCount()
               << " scheduled_dirty=" << context.probe_volume.GetScheduledDirtyBrickCount()
               << " deferred_dirty=" << context.probe_volume.GetDeferredDirtyBrickCount()
               << " dirty_influence_scale=" << ui_config.probe_gi_dirty_influence_scale
               << " intensity=" << ui_config.probe_gi_intensity
               << " hit_lights=" << lighting_data->light_count
               << " trace_distance=" << ui_config.probe_gi_trace_distance
               << " trace_rays=" << ui_config.probe_gi_trace_ray_count
               << " ray_sequence=rotated_fibonacci"
               << " placement=history_iterative(16x3+verify)"
               << " visibility=(" << ui_config.probe_gi_visibility_bias << ", "
               << ui_config.probe_gi_visibility_power << ", "
               << ui_config.probe_gi_visibility_min_weight << ", "
               << ui_config.probe_gi_visibility_strength << ")"
               << " sky=" << ui_config.probe_gi_sky_intensity;
        RasterTool::LogDebugEverySeconds(stream.str(), 2.0);
    }

    context.cmd_list.CopyFrom(
        std::span<byte>((byte*)lighting_data, sizeof(LightingData)),
        context.lighting_data_buffer.buf->GetView()
    );
}

RasterFramePacket
RasterRenderer::PrepareFrame(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyGameThread());
    assert(editor_config);

    LogSceneLoadStatus(*editor_config);

    RasterFramePacket frame_packet{};
    frame_packet.frame_id = m_next_frame_id++;
    frame_packet.window   = TickWindowContext(editor_config->GetResolution());
    editor_config->SetResolution(frame_packet.window.resolution);

    if (hooks.on_tick_scripting) {
        hooks.on_tick_scripting(scene);
    }

    if (hooks.on_tick_test) {
        hooks.on_tick_test(scene);
    }

    if (hooks.on_tick_ui) {
        hooks.on_tick_ui(scene);
    }

    const CameraFrameInput camera_input = CameraFrameInput::Capture(*editor_config);
    frame_packet.camera_viewport_resolution = camera_input.viewport_resolution;

    if (scene.IsReady()) {
        ProcessSceneTestCaseRequests(
            editor_config->scene_test_case_config,
            scene,
            GetElapsedTimeSeconds()
        );
    }

    auto& scene_test_case_runner = SceneTestCaseRunner::Get();
    const bool is_run_scene_test_case =
        scene_test_case_runner.HasActiveCase() || scene_test_case_runner.HasPendingCase();
    frame_packet.scene_updates =
        scene.PrepareUpdateBatch(is_run_scene_test_case, m_capture_scene_geometry_snapshot);
    if (frame_packet.scene_updates.scene_ready) {
        if (frame_packet.scene_updates.geometry) {
            m_capture_scene_geometry_snapshot = false;
        }

        Camera main_camera = frame_packet.scene_updates.main_camera;
        if (!m_b_scene_view_camera_initialized) {
            m_scene_view_camera               = main_camera;
            m_b_scene_view_camera_initialized = true;
        }

        Camera& render_camera = editor_config->active_viewport_mode == EEditorViewportMode::Scene ?
                                    m_scene_view_camera :
                                    main_camera;
        render_camera.Tick(camera_input);
        frame_packet.render_camera = render_camera;

        if (editor_config->active_viewport_mode == EEditorViewportMode::Game) {
            frame_packet.scene_updates.main_camera = render_camera;
            scene.GetMainCamera().camera            = render_camera;
        }
    } else {
        m_capture_scene_geometry_snapshot = true;
    }

    frame_packet.raster_config        = editor_config->raster_config;
    frame_packet.active_viewport_mode = editor_config->active_viewport_mode;
    frame_packet.scene_view_gizmos    = editor_config->scene_view_gizmos;
    if (hooks.on_capture_ui_composition) {
        frame_packet.ui_composition = hooks.on_capture_ui_composition();
    }
    if (hooks.on_capture_ui_draw_frame) {
        frame_packet.ui_draw_frame = hooks.on_capture_ui_draw_frame();
    }

    if (frame_packet.frame_id == 0) {
        LOG_INFO(
            "[Threading] RasterFramePacket boundary active; resolution={}x{}, UI main vertices={}, "
            "platform viewports={}.",
            frame_packet.window.resolution.x,
            frame_packet.window.resolution.y,
            frame_packet.ui_draw_frame.main_viewport.vertices.size(),
            frame_packet.ui_draw_frame.platform_viewports.size()
        );
    }

    return frame_packet;
}

RasterFrameFeedback RasterRenderer::RenderFrame(RasterFramePacket frame_packet) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyRenderThread());

    auto& raster_context = *raster_context_ptr;
    raster_context.SetResolution(frame_packet.window.resolution);
    raster_context.BeginSceneFrame(frame_packet.scene_updates);
    PrepareRenderFrame(frame_packet.window);

    if (time == 0) {
        const uint32_t frame_thread_id =
            IsCurrentlyRenderThread() ? GetRenderThreadId() : GetGameThreadId();
        LOG_INFO(
            "[Threading] Raster frames execute on {} thread id = {}",
            IsCurrentlyRenderThread() ? "Render" : "Game",
            frame_thread_id
        );
    }

    bool skip_present = false;
    if (frame_packet.window.state == EWindowState::Hiding) {
        std::this_thread::yield(); // FIXME: 这个东西有用吗？
        skip_present = true;

    } else if (frame_packet.window.state == EWindowState::SizeChanged) {
        LOG_INFO("Size Changed.");

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

    } else {
        assert(frame_packet.window.state == EWindowState::Default);
    }

    TextureRef default_output_texture = raster_context.textures.output.tex;

    // MARK: 3. Run Render Passes

    if (frame_packet.scene_updates.scene_ready) {
        // 处理场景加载过程中遗留的命令
        auto& scene_updates = frame_packet.scene_updates;
        if (scene_updates.initial_gpu_update) {
            auto commands = render_scene->ApplyUpdate(std::move(*scene_updates.initial_gpu_update));
            RasterTool::ExecuteScenePendingCommands(
                std::move(commands), device, gfx_queue
            );
        }

        if (first_load) {
            first_load = false;

            // 随手加一句，避免出错（重构完毕后可以尝试去除）
            cmd_list.UpdateBindlessArray(bindless_array);
            gfx_queue.Execute(cmd_list.Submit());
            gfx_queue.Sync();
        }

        auto& raster_config = frame_packet.raster_config;

        const auto& scene_tick_state = scene_updates.tick_state;
        if (scene_updates.update_gpu_update) {
            auto commands = render_scene->ApplyUpdate(std::move(*scene_updates.update_gpu_update));
            RasterTool::ExecuteScenePendingCommands(
                std::move(commands), device, gfx_queue
            );
        }

        const Camera& main_camera = scene_updates.main_camera;
        Camera        camera      = frame_packet.render_camera;

        {
            // Jitter Camera for SMAA T2x
            static uint8_t smaa_current_frame_index = 0;
            if (raster_config.aa_mode == EAaMode::SMAA_T2X) {

                smaa_current_frame_index ^= 1;
                static StaticArray<float2, 2> smaa_jitter = {float2(0.25f, -0.25f), float2(-0.25f, 0.25f)};
                const uint2 jitter_resolution = frame_packet.camera_viewport_resolution.x > 0 &&
                                                        frame_packet.camera_viewport_resolution.y > 0 ?
                                                    frame_packet.camera_viewport_resolution :
                                                    frame_packet.window.resolution;
                camera.SetJitterMatrix(smaa_jitter[smaa_current_frame_index], jitter_resolution);
            }
        }

        raster_context.Update(camera.GetDeltaTime());

        if (scene_tick_state.updated_transform || scene_tick_state.rebuilt_mesh) {
            raster_context.csm_data.shadow_cache_config_snapshot_valid = false;
            raster_context.InvalidateHiZHistory();
        }

        // others
        // FIXME: 统一update scene
        // scene.GetGpuScene().UpdateRaytracingScene(cmd_list);

        // Shadow Depth Pass
        cmd_list.PushScopeWithTimeScope(RasterTool::GetShadowDepthPassProfileScopeName());
        shadow_depth_pass->Process(raster_context, raster_config, camera);
        cmd_list.PopScopeWithTimeScope();

        probe_update_pass->Process(raster_context, raster_config, camera, time);

        // Update Global Lighting Data
        UpdateGlobalLightingData(raster_context, raster_config, camera);

        // Geometry Pass
        cmd_list.PushScopeWithTimeScope(RasterTool::GetGeometryPassProfileScopeName());
        geometry_pass->Process(raster_context, raster_config, camera);
        cmd_list.PopScopeWithTimeScope();

        hiz_build_pass->Process(raster_context, raster_config);
        raster_context.CommitHiZHistory(camera.GetViewProjectionMatrix());

        // Directional Shadow Mask Pass
        directional_shadow_mask_pass->Process(raster_context, raster_config, camera);

        // Lighting Pass
        lighting_pass->Process(raster_context, raster_config, camera);

        //Env&Atmo Pass
        skybox_pass->Process(raster_context, raster_config, camera);

        const auto& scene_gizmos = frame_packet.scene_view_gizmos;
        const bool  draw_scene_gizmos =
            frame_packet.active_viewport_mode == EEditorViewportMode::Scene && scene_gizmos.enabled;
        if (draw_scene_gizmos && scene_gizmos.show_probe_gi) {
            RasterConfig probe_gizmo_config                    = raster_config;
            probe_gizmo_config.probe_gi_gizmo_enabled         = scene_gizmos.show_probe_gi_probes;
            probe_gizmo_config.probe_gi_volume_bounds_enabled = scene_gizmos.show_probe_gi_volume_bounds;
            if (scene_gizmos.show_probe_gi_adaptive_cells) {
                probe_gizmo_config.probe_gi_debug_mode = 9;
            }
            probe_gizmo_pass->Process(raster_context, probe_gizmo_config, camera);
        }

        // Post Process Passes
        // - Ambient Occlusion
        auto ao_result = ao_pass->Process(raster_context, raster_config, camera, time);

        rtao_denoiser_pass->ProcessInPlace(raster_context, raster_config, ao_result.ao_only_idx);
        ao_pass->CompositeAo(raster_context, raster_config, ao_result.ao_only);

        TextureWithHandle processing_image = raster_context.textures.ao_output;

        // - CUDA Pass
#if WITH_CUDA
        if (raster_config.ai_is_cuda_enabled) {
            processing_image = tensor_rt_pass->Process(raster_context, raster_config, ao_result.ao_only_idx);
        }
#endif

        // - Denoiser Pass (Bilateral Filter)
        processing_image = bfd_pass->Process(raster_context, raster_config, processing_image);

        // - Screen Space Reflection
        processing_image = ssr_pass->Process(raster_context, raster_config, camera, processing_image);

        // - Cooperative Ops
        processing_image = cooperative_ops_pass->Process(raster_context, raster_config, processing_image);

        // - Anti-aliasing
        processing_image = aa_pass->Process(raster_context, raster_config, camera, processing_image);

        // - Bloom Pass
        processing_image = bloom_pass->Process(raster_context, raster_config, processing_image);

        // - Tonemapping Pass
        processing_image = tonemapping_pass->Process(raster_context, raster_config, processing_image);

        if (draw_scene_gizmos) {
            if (scene_gizmos.show_main_camera) {
                camera_gizmo_pass->Process(raster_context, camera, main_camera);
            }

            if (scene_gizmos.show_csm) {
                csm_gizmo_pass->Process(raster_context, raster_config, scene_gizmos, camera, main_camera);
            }
        }

        if (frame_packet.ui_composition.enabled) {
            const auto& ui_frame = frame_packet.ui_composition;
            const auto  window_frame_buffer = ui_frame.window_frame_buffer ?
                                                  ui_frame.window_frame_buffer->GetView() :
                                                  TextureView();
            default_output_texture = ui_combine_pass->Process(
                cmd_list,
                ui_frame.separate_window,
                ui_frame.output_resolution,
                ui_frame.scene_color_position,
                ui_frame.scene_color_resolution,
                window_frame_buffer,
                raster_context.GetSelectedFrameBufferView(raster_config.selected_frame_buffer_index),
                raster_context.textures.ui_frame_buffer.tex,
                raster_context.textures.output.tex
            );
        } else {
            // Without editor UI composition, use the copy path directly instead of sampling an uninitialized
            // UI buffer through the combine shader.
            default_output_texture = ui_combine_pass->Process(
                cmd_list,
                true,
                frame_packet.window.resolution,
                float2(0.f, 0.f),
                float2(
                    static_cast<float>(frame_packet.window.resolution.x),
                    static_cast<float>(frame_packet.window.resolution.y)
                ),
                TextureView(raster_context.textures.output.tex),
                processing_image.tex,
                {},
                raster_context.textures.output.tex
            );
        }

        // without test
        // FIXME: 统一advance frame
        // scene.GetRaytracingScene()->AdvanceFrame();

        // debug
        if (raster_config.debug_fps_limit_enable) {
            // sleep 1.0 / fps seconds
            // 虽然不精确，但简单
            std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / raster_config.debug_fps_limit));
            LOG_DEBUG("FPS Limit Enabled: {}", raster_config.debug_fps_limit);
        }

        scene_updates.main_camera = std::move(main_camera);
    }

    // 因为目前Vulkan的输出信息会聚合后再print，所以我们需要轮询，打印出最后添加的信息
    device.FlushDebugMessages();

    const auto ui_execution_thread =
        IsRenderThreadInitialized() ? EUiDrawExecutionThread::Render : EUiDrawExecutionThread::Game;
    RenderUiDrawFrame(
        cmd_list, default_output_texture->GetView(), frame_packet.ui_draw_frame, ui_execution_thread
    );

    raster_context.probe_volume.TrackFrameSubmission(cmd_list, time);
    time++;
    /***
        currently using a phony timeline (any timeline signaled by copy queue) to remove error message from validation layer caused by host synced copy operations
        we're not waiting for the copy queue to finish, because operations we wanted are synced on host side, we use this timeline just to notifiy the validation layer
        that we've done flushing copy queue resources
        */
    gfx_queue.Execute(cmd_list.Submit().Signal(timeline, time).DeleteResources().TickProfiling());

    if (!skip_present) {
        const auto output_view = default_output_texture->GetView();
        if (frame_packet.window.state == EWindowState::SizeChanged) {
            LOG_INFO(
                "[Threading][Resize] Main present source={}x{}, swapchain={}x{}, UI platform viewports={}.",
                output_view.extent.x,
                output_view.extent.y,
                swapchain->size.x,
                swapchain->size.y,
                frame_packet.ui_draw_frame.platform_viewports.size()
            );
        }
        if (output_view.extent.x == swapchain->size.x && output_view.extent.y == swapchain->size.y) {
            gfx_queue.Present(swapchain, default_output_texture);
        } else {
            LOG_WARNING(
                "Skipping stale main-window present: source={}x{}, swapchain={}x{}.",
                output_view.extent.x,
                output_view.extent.y,
                swapchain->size.x,
                swapchain->size.y
            );
        }
        PresentUiDrawFrame(frame_packet.ui_draw_frame, ui_execution_thread);
    }

    RasterFrameFeedback feedback{};
    feedback.frame_id               = frame_packet.frame_id;
    feedback.culling_stats          = frame_packet.raster_config.culling_stats;
    feedback.cooperative_ops_status = frame_packet.raster_config.cooperative_ops_status;
    if (frame_packet.frame_id == 0) {
        for (const TextureView& view : raster_context.GetDisplayableFrameBuffersView()) {
            feedback.displayable_frame_buffer_names.emplace_back(view.GetTexture()->GetName());
        }
    }
    raster_context.EndSceneFrame();
    return feedback;
}

bool RasterRenderer::RunSingle(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    ApplyFrameFeedback(
        RenderFrame(PrepareFrame(editor_config, hooks)), editor_config->raster_config, hooks
    );
    return !hooks.on_is_need_reload || !hooks.on_is_need_reload();
}

void RasterRenderer::ApplyFrameFeedback(
    RasterFrameFeedback feedback,
    RasterConfig&       target_config,
    const EngineHooks&  hooks
) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyGameThread());
    target_config.culling_stats          = feedback.culling_stats;
    target_config.cooperative_ops_status = feedback.cooperative_ops_status;
    if (!feedback.displayable_frame_buffer_names.empty() &&
        hooks.on_raster_register_frame_buffer_names) {
        hooks.on_raster_register_frame_buffer_names(feedback.displayable_frame_buffer_names);
    }
}

} // namespace Moer::Render::Raster
