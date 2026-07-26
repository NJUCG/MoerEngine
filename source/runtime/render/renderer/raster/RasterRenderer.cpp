// 负责光栅帧编排，不在此处实现各 Pass 的具体渲染算法。
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
#include "TessellatedSurfacePass.h"
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
#include "config/ConfigManager.h"
#include "debug/RenderDocApi.h"
#include "misc/ScopedLogTimer.h"
#include "rendergraph/RenderGraph.h"
#include "rhi/RHIExecutor.h"
#include "scene/testcase/SceneTestCaseDispatcher.h"
#include "scene/testcase/SceneTestCaseRunner.h"
#include "window/WindowContext.h"

#include <algorithm>
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
    uint2                   initial_resolution,
    SharedPtr<EditorConfig> config
) :
    Renderer(initial_resolution, config) {
    ScopedLogTimer startup_timer("[Startup][RasterRenderer] RasterRenderer::Constructor() total");

    const auto& engine_config = ConfigManager::GetInstance().GetConfig().engine;
    const auto& graph_config  = engine_config.render.raster;
    render_graph_enabled    = graph_config.render_graph;
    render_graph_debug_dump = graph_config.render_graph_debug_dump;
    parallel_recording_enabled = graph_config.render_graph_parallel_recording;
    LOG_INFO(
        "[RenderGraph] Raster execution mode: {}, upper recording: {}",
        render_graph_enabled ? "graph" : "linear",
        parallel_recording_enabled ? "parallel-eligible" : "serial-fallback"
    );
    if (!config->validation_selected_frame_buffer_name.empty()) {
        LOG_INFO(
            "[ThreadingValidation][RasterFramebuffer] selection={}",
            config->validation_selected_frame_buffer_name
        );
    }

    raster_context_ptr = MakeUnique<RasterContext>(
        device, manager, gfx_queue, bindless_array, cmd_list, *render_scene, resolution
    );
    auto& raster_context = *raster_context_ptr;

    raster_context.CreateFrameBuffers();
    raster_context.UploadExternalFrameBuffers();
    raster_context.AllocateFrameBuffers();

    RHIExecutor::Get().Submit(
        EQueueType::Graphics, cmd_list.Submit(), ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    hiz_build_pass               = MakeUnique<HiZBuildPass>(raster_context);
    shadow_depth_pass            = MakeUnique<ShadowDepthPass>(raster_context);
    directional_shadow_mask_pass = MakeUnique<DirectionalShadowMaskPass>(raster_context);
    probe_update_pass            = MakeUnique<ProbeUpdatePass>(raster_context);
    csm_gizmo_pass               = MakeUnique<CsmGizmoPass>(raster_context);
    probe_gizmo_pass             = MakeUnique<ProbeGizmoPass>(raster_context);
    camera_gizmo_pass            = MakeUnique<CameraGizmoPass>(raster_context);
    geometry_pass                = MakeUnique<GeometryPass>(raster_context);
    tessellated_surface_pass     = MakeUnique<TessellatedSurfacePass>(raster_context);
    lighting_pass                = MakeUnique<LightingPass>(raster_context);
    skybox_pass                  = MakeUnique<SkyboxPass>(raster_context);
    ao_pass                      = MakeUnique<AoPass>(raster_context);
    rtao_denoiser_pass           = MakeUnique<RtaoDenoiserPass>(raster_context);
    bilateral_filter_denoiser_pass =
        MakeUnique<BilateralFilterDenoiserPass>(raster_context);
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
    RHIExecutor::Get().Submit(
        EQueueType::Graphics, cmd_list.Submit(), ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

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

    // 必须在派生 Pass 对象仍然存活时释放其 Pass 资源。
    ReleaseResources();
    LOG_INFO(
        "[Threading] RasterRenderer destruction finished on {} Thread.",
        IsCurrentlyRenderThread() ? "Render" : "Game"
    );
}

void RasterRenderer::Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    while (!WindowContext::ShouldClose(WindowContext::GetMainWindow())) {
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
    const uint    cascade_count = ui_config.shadow_csm_num_of_cascades;
    LightingData* lighting_data = &context.lighting_data;

    lighting_data->clip2world      = Transpose(camera.GetViewProjectionMatrixInv());
    lighting_data->light_count     = context.GetSceneUpdates().light_count;
    lighting_data->camera_position = camera.GetPosition();

    // Shadow Parameters
    lighting_data->shadow_map_mode              = static_cast<int>(ui_config.shadow_map_mode);
    lighting_data->shadow_sampling_mode         = ui_config.shadow_sampling_mode;
    lighting_data->shadow_csm_num_of_cascades   = cascade_count;
    lighting_data->shadow_csm_sm_size           = ui_config.shadow_csm_sm_size;
    lighting_data->shadow_csm_visualize_cascade = ui_config.shadow_csm_visualize_cascade;

    // Shadow Map
    for (uint cascade_index = 0; cascade_index < cascade_count; ++cascade_index) {
        lighting_data->cascade_shadow_map[cascade_index] =
            context.csm_data.shadow_map_textures[cascade_index].hdl;
    }
    lighting_data->point_shadow_map = context.point_shadow_data.shadow_cubes[0].handle;
    lighting_data->light_pos        = context.point_shadow_data.shadow_cubes[0].light_pos;
    lighting_data->light_radius     = context.point_shadow_data.shadow_cubes[0].far_plane;

    // Shadow Transform
    for (uint cascade_index = 0; cascade_index < cascade_count; ++cascade_index) {
        lighting_data->world2shadow_clip[cascade_index] =
            Transpose(lighting_data->world2shadow_clip[cascade_index]);
    }
    lighting_data->world2view = Transpose(camera.GetViewMatrix());
    lighting_data->near_clip  = camera.GetNearClip();
    lighting_data->far_clip   = camera.GetFarClip();

    lighting_data->is_csm_blend_enabled = ui_config.shadow_csm_blend_option ? 1 : 0;
    // Shader 仅使用有效前缀，未启用的级联槽位保持不变。

    // PCSS
    lighting_data->light_size_world = ui_config.shadow_pcss_light_size_world;
    lighting_data->pcss_enabled     = ui_config.shadow_pcss_enabled ? 1 : 0;

    // BRDF
    lighting_data->lut_ggx_emu_handle  = context.textures.lut_ggx_emu.hdl;
    lighting_data->lut_ggx_eavg_handle = context.textures.lut_ggx_eavg.hdl;

    lighting_data->brdf_enable_multi_scatter = ui_config.shading_brdf_enable_multi_scatter ? 1 : 0;
    lighting_data->brdf_NDF_mode             = static_cast<uint>(ui_config.shading_brdf_NDF_mode);
    lighting_data->brdf_G_mode               = static_cast<uint>(ui_config.shading_brdf_G_mode);
    lighting_data->brdf_G_is_ibl             = ui_config.shading_brdf_G_is_ibl ? 1 : 0;

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
        std::span<byte>(reinterpret_cast<byte*>(lighting_data), sizeof(LightingData)),
        context.lighting_data_buffer.buf->GetView()
    );
}

RasterFramePacket
RasterRenderer::PrepareFrame(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyGameThread());
    assert(editor_config);

    const bool           profile_logging = IsFramePrepareProfilingEnabled();
    const auto           prepare_started = BeginFramePrepareProfile();
    FramePrepareProfile  prepare_profile{};
    FramePrepareWorkload prepare_workload{};
    RasterFramePacket    frame_packet{};
    frame_packet.frame_id = next_frame_id++;
    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.window_ms);
        LogSceneLoadStatus(*editor_config);
        frame_packet.window = TickWindowContext(editor_config->GetResolution());
        editor_config->SetResolution(frame_packet.window.resolution);
    }

    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.scripting_ms);
        if (hooks.on_tick_scripting) {
            hooks.on_tick_scripting(scene);
        }
    }

    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.test_ms);
        if (hooks.on_tick_test) {
            hooks.on_tick_test(scene);
        }
    }

    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.ui_tick_ms);
        if (hooks.on_tick_ui) {
            hooks.on_tick_ui(scene);
        }
    }

    CameraFrameInput camera_input{};
    bool             is_run_scene_test_case = false;
    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.camera_and_test_ms);
        camera_input                            = CameraFrameInput::Capture(*editor_config);
        frame_packet.camera_viewport_resolution = camera_input.viewport_resolution;

        if (scene.IsReady()) {
            ProcessSceneTestCaseRequests(
                editor_config->scene_test_case_config, scene, GetElapsedTimeSeconds()
            );
        }

        auto& scene_test_case_runner = SceneTestCaseRunner::Get();
        is_run_scene_test_case =
            scene_test_case_runner.HasActiveCase() || scene_test_case_runner.HasPendingCase();
    }

    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.scene_update_ms);
        frame_packet.scene_updates =
            scene.PrepareUpdateBatch(is_run_scene_test_case, capture_scene_geometry_snapshot);
    }
    if (profile_logging) {
        const auto& updates                        = frame_packet.scene_updates;
        prepare_workload.scene_ready_frames        = updates.scene_ready ? 1u : 0u;
        prepare_workload.scene_dirty_frames        = static_cast<bool>(updates.tick_state) ? 1u : 0u;
        prepare_workload.initial_gpu_update_frames = updates.initial_gpu_update ? 1u : 0u;
        prepare_workload.update_gpu_update_frames  = updates.update_gpu_update ? 1u : 0u;
        prepare_workload.geometry_snapshot_frames  = updates.geometry ? 1u : 0u;
    }

    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.camera_and_test_ms);
        if (frame_packet.scene_updates.scene_ready) {
            if (frame_packet.scene_updates.geometry) {
                capture_scene_geometry_snapshot = false;
            }

            Camera main_camera = frame_packet.scene_updates.main_camera;
            if (!scene_view_camera_initialized) {
                scene_view_camera             = main_camera;
                scene_view_camera_initialized = true;
            }

            Camera& render_camera = editor_config->active_viewport_mode == EEditorViewportMode::Scene ?
                                        scene_view_camera :
                                        main_camera;
            render_camera.Tick(camera_input);
            frame_packet.render_camera = render_camera;

            if (editor_config->active_viewport_mode == EEditorViewportMode::Game) {
                frame_packet.scene_updates.main_camera = render_camera;
                scene.GetMainCamera().camera           = render_camera;
            }
        } else {
            capture_scene_geometry_snapshot = true;
        }
    }

    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.config_snapshot_ms);
        frame_packet.raster_config        = editor_config->raster_config;
        frame_packet.validation_selected_frame_buffer_name =
            editor_config->validation_selected_frame_buffer_name;
        frame_packet.active_viewport_mode = editor_config->active_viewport_mode;
        frame_packet.scene_view_gizmos    = editor_config->scene_view_gizmos;
    }

    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.ui_composition_ms);
        if (hooks.on_capture_ui_composition) {
            frame_packet.ui_composition = hooks.on_capture_ui_composition();
        }
    }
    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.ui_draw_packet_ms);
        if (hooks.on_capture_ui_draw_frame) {
            frame_packet.ui_draw_frame = hooks.on_capture_ui_draw_frame();
        }
        CaptureFramePrepareUiWorkload(prepare_workload, frame_packet.ui_draw_frame);
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

    RecordFramePrepareProfile("Raster", prepare_started, prepare_profile, prepare_workload);
    return frame_packet;
}

RasterFrameFeedback RasterRenderer::RenderFrame(RasterFramePacket frame_packet) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyRenderThread());

    auto& raster_context = *raster_context_ptr;
    raster_context.SetResolution(frame_packet.window.resolution);
    raster_context.BeginSceneFrame(frame_packet.scene_updates);
    PrepareRenderFrame(frame_packet.window);

    if (time == 0) {
        const uint32_t frame_thread_id = IsCurrentlyRenderThread() ? GetRenderThreadId() : GetGameThreadId();
        LOG_INFO(
            "[Threading] Raster frames execute on {} thread id = {}",
            IsCurrentlyRenderThread() ? "Render" : "Game",
            frame_thread_id
        );
    }

    bool              skip_present = false;
    bool              split_graph_profiling_frame = false;
    PresentReceiptRef main_present_receipt{};
    if (frame_packet.window.state == EWindowState::Hiding) {
        // 窗口最小化后无法 present，此处主动让出线程，避免高频空转。
        std::this_thread::yield();
        skip_present = true;

    } else if (frame_packet.window.state == EWindowState::SizeChanged) {
        LOG_INFO("Size Changed.");

        raster_context.FreeFrameBuffers(false);
        raster_context.CreateFrameBuffers();
        // 外部纹理与分辨率无关，窗口尺寸变化后仍然有效。
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

    // 窗口资源就绪后再使用已准备好的场景快照。

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

            // 第一个 Raster Pass 执行前，先发布首次场景上传创建的 descriptor。
            cmd_list.UpdateBindlessArray(bindless_array);
            RHIExecutor::Get().Submit(
                EQueueType::Graphics, cmd_list.Submit(), ERHIExecSubmitFlags::FlushGPU
            );
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        }

        auto& raster_config = frame_packet.raster_config;

        const auto& scene_tick_state = scene_updates.tick_state;
        if (scene_updates.update_gpu_update) {
            auto commands = render_scene->ApplyUpdate(std::move(*scene_updates.update_gpu_update));
            RasterTool::ExecuteScenePendingCommands(
                std::move(commands), device, gfx_queue
            );
        }

        ScopedGpuMarker renderer_marker(
            cmd_list, "Raster Renderer", GpuMarkerPalette::Renderer()
        );

        const Camera& main_camera = scene_updates.main_camera;
        Camera        camera      = frame_packet.render_camera;

        {
            // 在不修改已捕获相机的前提下，交替使用两个 SMAA T2x 亚像素偏移。
            static uint8_t s_smaa_frame_index = 0;
            if (raster_config.aa_mode == EAaMode::SMAA_T2X) {
                s_smaa_frame_index ^= 1;
                static const StaticArray<float2, 2> s_smaa_jitter_offsets = {
                    float2(0.25f, -0.25f),
                    float2(-0.25f, 0.25f)
                };
                const uint2 jitter_resolution = frame_packet.camera_viewport_resolution.x > 0 &&
                                                        frame_packet.camera_viewport_resolution.y > 0 ?
                                                    frame_packet.camera_viewport_resolution :
                                                    frame_packet.window.resolution;
                camera.SetJitterMatrix(s_smaa_jitter_offsets[s_smaa_frame_index], jitter_resolution);
            }
        }

        raster_context.Update(camera.GetDeltaTime());

        if (scene_tick_state.updated_transform || scene_tick_state.rebuilt_mesh) {
            raster_context.csm_data.shadow_cache_config_snapshot_valid = false;
            raster_context.InvalidateHiZHistory();
        }

        const auto& scene_gizmos = frame_packet.scene_view_gizmos;
        const bool  draw_scene_gizmos =
            frame_packet.active_viewport_mode == EEditorViewportMode::Scene && scene_gizmos.enabled;
        AoPass::AoPassOutput ao_result{};
        TextureWithHandle   processing_image = raster_context.textures.ao_output;
        TextureView         selected_framebuffer_view{};
        TextureView         window_framebuffer_view{};
        if (frame_packet.ui_composition.enabled) {
            if (frame_packet.validation_selected_frame_buffer_name.empty()) {
                selected_framebuffer_view = raster_context.GetSelectedFrameBufferView(
                    raster_config.selected_frame_buffer_index
                );
            } else {
                const auto displayable_views = raster_context.GetDisplayableFrameBuffersView();
                const auto selected = std::find_if(
                    displayable_views.begin(),
                    displayable_views.end(),
                    [&](const TextureView& view) {
                        return view.GetTexture()->GetName() ==
                               frame_packet.validation_selected_frame_buffer_name;
                    }
                );
                if (selected == displayable_views.end()) {
                    LOG_ERROR(
                        "[ThreadingValidation][RasterFramebuffer] selected framebuffer '{}' no longer "
                        "exists; using the UI-selected framebuffer instead.",
                        frame_packet.validation_selected_frame_buffer_name
                    );
                    selected_framebuffer_view = raster_context.GetSelectedFrameBufferView(
                        raster_config.selected_frame_buffer_index
                    );
                } else {
                    selected_framebuffer_view = *selected;
                }
            }
            if (frame_packet.ui_composition.window_frame_buffer) {
                window_framebuffer_view = frame_packet.ui_composition.window_frame_buffer->GetView();
            }
        }
        const bool ui_writes_external_window =
            frame_packet.ui_composition.enabled && frame_packet.ui_composition.separate_window &&
            window_framebuffer_view.GetTexture() != nullptr;

        struct RasterParallelRecordSnapshots {
            HiZBuildPass::RecordParameters               hiz{};
            DirectionalShadowMaskPass::RecordParameters directional_shadow{};
            LightingPass::RecordParameters               lighting{};
            SkyboxPass::RecordParameters                 skybox{};
            bool                                         prepared{false};
        };
        auto parallel_record_snapshots = MakeShared<RasterParallelRecordSnapshots>();

        struct RasterGraphResources {
            RenderGraph::TokenHandle   scene;
            RenderGraph::TokenHandle   shadow_maps;
            RenderGraph::TokenHandle   probe_volume;
            RenderGraph::BufferHandle  lighting_data;
            RenderGraph::TextureHandle base_color;
            RenderGraph::TextureHandle normal;
            RenderGraph::TextureHandle metal_rough_ao;
            RenderGraph::TextureHandle depth;
            RenderGraph::TextureHandle cubemap;
            RenderGraph::TextureHandle hiz_current;
            RenderGraph::TextureHandle hiz_previous;
            RenderGraph::TextureHandle shadow_mask;
            RenderGraph::TextureHandle lighting_output;
            RenderGraph::TokenHandle   ao_working_set;
            RenderGraph::TokenHandle   motion_vectors;
            RenderGraph::TextureHandle ao_output;
            RenderGraph::TextureHandle denoiser_output;
            RenderGraph::TextureHandle ssr_output;
            RenderGraph::TextureHandle aa_output;
            RenderGraph::TokenHandle   bloom_chain;
            RenderGraph::TokenHandle   tonemapping_state;
            RenderGraph::TextureHandle tonemapping_output;
            RenderGraph::TextureHandle selected_framebuffer;
            RenderGraph::TextureHandle window_framebuffer;
            RenderGraph::TextureHandle output;
            RenderGraph::BufferHandle  scene_lights;
            RenderGraph::TokenHandle   processing_image;
            RenderGraph::TokenHandle   parallel_record_snapshots;
            RenderGraph::TokenHandle   hiz_recorded;
        } graph_resources{};

        auto define_raster_passes = [&](auto&& dispatch) {
            auto schedule = [&](std::string_view name, auto&& setup, auto&& execute) {
                dispatch(
                    name,
                    std::forward<decltype(setup)>(setup),
                    RenderGraph::PassExecutionClass::MainThread,
                    [execute = std::forward<decltype(execute)>(execute)](CommandList&) mutable {
                        execute();
                    }
                );
            };
            auto schedule_external = [&](std::string_view name, auto&& setup, auto&& execute) {
                dispatch(
                    name,
                    [setup = std::forward<decltype(setup)>(setup)](
                        RenderGraph::PassBuilder& builder
                    ) mutable {
                        setup(builder);
                        builder.ExternalControl();
                    },
                    RenderGraph::PassExecutionClass::ExternalControl,
                    [execute = std::forward<decltype(execute)>(execute)](CommandList&) mutable {
                        execute();
                    }
                );
            };
            auto schedule_cpu_prepare = [&](std::string_view name, auto&& setup, auto&& execute) {
                dispatch(
                    name,
                    [setup = std::forward<decltype(setup)>(setup)](
                        RenderGraph::PassBuilder& builder
                    ) mutable {
                        setup(builder);
                        builder.CpuPrepare();
                    },
                    RenderGraph::PassExecutionClass::CpuPrepare,
                    [execute = std::forward<decltype(execute)>(execute)](CommandList&) mutable {
                        execute();
                    }
                );
            };
            auto schedule_parallel_record =
                [&](std::string_view name, auto&& setup, auto&& record) {
                    dispatch(
                        name,
                        std::forward<decltype(setup)>(setup),
                        RenderGraph::PassExecutionClass::ParallelRecordEligible,
                        std::forward<decltype(record)>(record)
                    );
                };
            schedule(
                "ShadowDepth",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.scene)
                        .Write(graph_resources.shadow_maps)
                        .SideEffect();
                },
                [&]() {
                    cmd_list.PushScopeWithTimeScope(RasterTool::GetShadowDepthPassProfileScopeName());
                    shadow_depth_pass->Process(raster_context, raster_config, camera);
                    cmd_list.PopScopeWithTimeScope();
                }
            );
            schedule(
                "ProbeUpdate",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.scene)
                        .ReadWrite(graph_resources.probe_volume)
                        .SideEffect();
                },
                [&]() { probe_update_pass->Process(raster_context, raster_config, camera, time); }
            );
            schedule(
                "UploadLightingData",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.shadow_maps)
                        .Read(graph_resources.probe_volume)
                        .Write(graph_resources.lighting_data)
                        .SideEffect();
                },
                [&]() { UpdateGlobalLightingData(raster_context, raster_config, camera); }
            );
            schedule(
                "Geometry",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.scene)
                        .Read(graph_resources.hiz_previous)
                        .Write(graph_resources.base_color)
                        .Write(graph_resources.normal)
                        .Write(graph_resources.metal_rough_ao)
                        .Write(graph_resources.depth);
                },
                [&]() {
                    cmd_list.PushScopeWithTimeScope(RasterTool::GetGeometryPassProfileScopeName());
                    geometry_pass->Process(raster_context, raster_config, camera);
                    cmd_list.PopScopeWithTimeScope();
                }
            );
            schedule(
                "TessellatedSurface",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.ReadWrite(graph_resources.base_color)
                        .ReadWrite(graph_resources.normal)
                        .ReadWrite(graph_resources.metal_rough_ao)
                        .ReadWrite(graph_resources.depth);
                },
                [&]() {
                    tessellated_surface_pass->Process(raster_context, raster_config, camera);
                }
            );
            schedule_cpu_prepare(
                "PrepareParallelRecording",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Reference(graph_resources.depth)
                        .Reference(graph_resources.normal)
                        .Reference(graph_resources.base_color)
                        .Reference(graph_resources.metal_rough_ao)
                        .Reference(graph_resources.cubemap)
                        .Reference(graph_resources.hiz_current)
                        .Reference(graph_resources.shadow_mask)
                        .Reference(graph_resources.lighting_output)
                        .Reference(graph_resources.lighting_data)
                        .Reference(graph_resources.shadow_maps)
                        .Read(graph_resources.scene)
                        .Write(graph_resources.parallel_record_snapshots)
                        .SideEffect();
                    if (graph_resources.scene_lights.IsValid()) {
                        builder.Reference(graph_resources.scene_lights);
                    }
                },
                [&, parallel_record_snapshots]() {
                    parallel_record_snapshots->hiz = hiz_build_pass->Prepare(raster_context);
                    parallel_record_snapshots->directional_shadow =
                        directional_shadow_mask_pass->Prepare(raster_context);
                    parallel_record_snapshots->lighting =
                        lighting_pass->Prepare(raster_context, raster_config);
                    parallel_record_snapshots->skybox =
                        skybox_pass->Prepare(raster_context, raster_config, camera);
                    parallel_record_snapshots->prepared = true;
                }
            );
            schedule_parallel_record(
                "HiZBuild",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.depth)
                        .Read(graph_resources.parallel_record_snapshots)
                        .Write(graph_resources.hiz_current)
                        .Write(graph_resources.hiz_recorded);
                },
                [&, parallel_record_snapshots](CommandList& recording_cmd_list) {
                    assert(parallel_record_snapshots->prepared);
                    hiz_build_pass->Record(recording_cmd_list, parallel_record_snapshots->hiz);
                }
            );
            schedule_parallel_record(
                "DirectionalShadowMask",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.normal)
                        .Read(graph_resources.depth)
                        .Read(graph_resources.lighting_data)
                        .Read(graph_resources.shadow_maps)
                        .Read(graph_resources.parallel_record_snapshots)
                        .Write(graph_resources.shadow_mask);
                },
                [&, parallel_record_snapshots](CommandList& recording_cmd_list) {
                    assert(parallel_record_snapshots->prepared);
                    directional_shadow_mask_pass->Record(
                        recording_cmd_list, parallel_record_snapshots->directional_shadow
                    );
                }
            );
            schedule_cpu_prepare(
                "CommitHiZHistory",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Reference(graph_resources.hiz_current)
                        .Reference(graph_resources.hiz_previous)
                        .Read(graph_resources.hiz_recorded)
                        .SideEffect();
                },
                [&]() { raster_context.CommitHiZHistory(camera.GetViewProjectionMatrix()); }
            );
            schedule_parallel_record(
                "Lighting",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.base_color)
                        .Read(graph_resources.normal)
                        .Read(graph_resources.metal_rough_ao)
                        .Read(graph_resources.depth)
                        .Read(graph_resources.shadow_mask)
                        .Read(graph_resources.lighting_data)
                        .Read(graph_resources.cubemap)
                        .Read(graph_resources.probe_volume)
                        .Read(graph_resources.parallel_record_snapshots)
                        .Write(graph_resources.lighting_output);
                    if (graph_resources.scene_lights.IsValid()) {
                        builder.Read(graph_resources.scene_lights);
                    }
                },
                [&, parallel_record_snapshots](CommandList& recording_cmd_list) {
                    assert(parallel_record_snapshots->prepared);
                    lighting_pass->Record(
                        recording_cmd_list, parallel_record_snapshots->lighting
                    );
                }
            );
            schedule_parallel_record(
                "Skybox",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.depth)
                        .Read(graph_resources.cubemap)
                        .Read(graph_resources.parallel_record_snapshots)
                        .ReadWrite(graph_resources.lighting_output);
                },
                [&, parallel_record_snapshots](CommandList& recording_cmd_list) {
                    assert(parallel_record_snapshots->prepared);
                    skybox_pass->Record(recording_cmd_list, parallel_record_snapshots->skybox);
                }
            );

            if (draw_scene_gizmos && scene_gizmos.show_probe_gi) {
                schedule(
                    "ProbeGizmo",
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.Read(graph_resources.probe_volume)
                            .ReadWrite(graph_resources.lighting_output)
                            .SideEffect();
                    },
                    [&]() {
                        RasterConfig probe_gizmo_config                    = raster_config;
                        probe_gizmo_config.probe_gi_gizmo_enabled         = scene_gizmos.show_probe_gi_probes;
                        probe_gizmo_config.probe_gi_volume_bounds_enabled =
                            scene_gizmos.show_probe_gi_volume_bounds;
                        if (scene_gizmos.show_probe_gi_adaptive_cells) {
                            probe_gizmo_config.probe_gi_debug_mode = 9;
                        }
                        probe_gizmo_pass->Process(raster_context, probe_gizmo_config, camera);
                    }
                );
            }

            schedule(
                "AmbientOcclusion",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.normal)
                        .Read(graph_resources.depth)
                        .Read(graph_resources.lighting_output)
                        .Read(graph_resources.scene)
                        .Write(graph_resources.ao_working_set)
                        .Write(graph_resources.motion_vectors);
                },
                [&]() { ao_result = ao_pass->Process(raster_context, raster_config, camera, time); }
            );
            schedule(
                "RtaoDenoise",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.normal)
                        .Read(graph_resources.depth)
                        .Read(graph_resources.motion_vectors)
                        .ReadWrite(graph_resources.ao_working_set);
                },
                [&]() {
                    rtao_denoiser_pass->ProcessInPlace(
                        raster_context, raster_config, ao_result.ao_only_idx
                    );
                }
            );
            schedule(
                "AoComposite",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.ao_working_set)
                        .Read(graph_resources.lighting_output)
                        .Read(graph_resources.depth)
                        .Read(graph_resources.normal)
                        .Write(graph_resources.ao_output)
                        .Write(graph_resources.processing_image);
                },
                [&]() {
                    ao_pass->CompositeAo(raster_context, raster_config, ao_result.ao_only);
                    processing_image = raster_context.textures.ao_output;
                }
            );

#if WITH_CUDA
            if (raster_config.ai_is_cuda_enabled) {
                schedule_external(
                    "TensorRT",
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.Read(graph_resources.ao_working_set)
                            .Read(graph_resources.depth)
                            .Read(graph_resources.lighting_output)
                            .Read(graph_resources.motion_vectors)
                            .ReadWrite(graph_resources.processing_image)
                            .SideEffect();
                    },
                    [&]() {
                        processing_image = tensor_rt_pass->Process(
                            raster_context, raster_config, ao_result.ao_only_idx
                        );
                    }
                );
            }
#endif

            schedule(
                "BilateralDenoise",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.ReadWrite(graph_resources.processing_image)
                        .Write(graph_resources.denoiser_output);
                },
                [&]() {
                    processing_image = bilateral_filter_denoiser_pass->Process(
                        raster_context, raster_config, processing_image
                    );
                }
            );
            schedule(
                "ScreenSpaceReflection",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.ReadWrite(graph_resources.processing_image)
                        .Read(graph_resources.normal)
                        .Read(graph_resources.depth)
                        .Read(graph_resources.metal_rough_ao)
                        .Write(graph_resources.ssr_output);
                },
                [&]() {
                    processing_image =
                        ssr_pass->Process(raster_context, raster_config, camera, processing_image);
                }
            );
            schedule_cpu_prepare(
                "CooperativeOps",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.ReadWrite(graph_resources.processing_image).SideEffect();
                },
                [&]() {
                    processing_image =
                        cooperative_ops_pass->Process(raster_context, raster_config, processing_image);
                }
            );
            schedule(
                "AntiAliasing",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.ReadWrite(graph_resources.processing_image)
                        .Read(graph_resources.depth)
                        .Write(graph_resources.aa_output);
                },
                [&]() {
                    processing_image =
                        aa_pass->Process(raster_context, raster_config, camera, processing_image);
                }
            );
            schedule(
                "Bloom",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.ReadWrite(graph_resources.processing_image)
                        .Write(graph_resources.bloom_chain);
                },
                [&]() {
                    processing_image = bloom_pass->Process(raster_context, raster_config, processing_image);
                }
            );
            schedule(
                "Tonemapping",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.ReadWrite(graph_resources.processing_image)
                        .ReadWrite(graph_resources.tonemapping_state)
                        .Write(graph_resources.tonemapping_output);
                },
                [&]() {
                    processing_image =
                        tonemapping_pass->Process(raster_context, raster_config, processing_image);
                }
            );

            if (draw_scene_gizmos && scene_gizmos.show_main_camera) {
                schedule(
                    "CameraGizmo",
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.Read(graph_resources.scene)
                            .ReadWrite(graph_resources.tonemapping_output)
                            .ReadWrite(graph_resources.processing_image)
                            .SideEffect();
                    },
                    [&]() { camera_gizmo_pass->Process(raster_context, camera, main_camera); }
                );
            }
            if (draw_scene_gizmos && scene_gizmos.show_csm) {
                schedule(
                    "CsmGizmo",
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.Read(graph_resources.shadow_maps)
                            .ReadWrite(graph_resources.tonemapping_output)
                            .ReadWrite(graph_resources.processing_image)
                            .SideEffect();
                    },
                    [&]() {
                        csm_gizmo_pass->Process(
                            raster_context, raster_config, scene_gizmos, camera, main_camera
                        );
                    }
                );
            }

            schedule(
                "UiCombine",
                [&](RenderGraph::PassBuilder& builder) {
                    if (!frame_packet.ui_composition.enabled) {
                        builder.Read(graph_resources.processing_image)
                            .Write(graph_resources.output);
                    } else if (ui_writes_external_window) {
                        builder.Read(graph_resources.selected_framebuffer)
                            .Write(graph_resources.window_framebuffer);
                    } else {
                        builder.Read(graph_resources.selected_framebuffer)
                            .Write(graph_resources.output);
                    }
                    builder.SideEffect();
                },
                [&]() {
                    if (frame_packet.ui_composition.enabled) {
                        const auto& ui_frame = frame_packet.ui_composition;
                        default_output_texture = ui_combine_pass->Process(
                            cmd_list,
                            ui_frame.separate_window,
                            ui_frame.output_resolution,
                            ui_frame.scene_color_position,
                            ui_frame.scene_color_resolution,
                            window_framebuffer_view,
                            selected_framebuffer_view,
                            raster_context.textures.output.tex
                        );
                    } else {
                        // Without editor UI composition, use the copy path directly instead of sampling an
                        // uninitialized UI buffer through the combine shader.
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
                            raster_context.textures.output.tex
                        );
                    }
                }
            );
        };

        auto execute_linear = [&]() {
            ScopedGpuMarker pipeline_marker(
                cmd_list, "Raster Linear Pipeline", GpuMarkerPalette::RenderGraph()
            );
            auto linear_schedule =
                [&](std::string_view                name,
                    auto&&,
                    RenderGraph::PassExecutionClass execution_class,
                    auto&&                          execute) {
                if (execution_class == RenderGraph::PassExecutionClass::CpuPrepare ||
                    execution_class == RenderGraph::PassExecutionClass::ExternalControl) {
                    std::forward<decltype(execute)>(execute)(cmd_list);
                    return;
                }
                const std::string marker_name = std::format("Pass: {}", name);
                ScopedGpuMarker pass_marker(
                    cmd_list, marker_name, GpuMarkerPalette::Pass()
                );
                std::forward<decltype(execute)>(execute)(cmd_list);
            };
            define_raster_passes(linear_schedule);
        };

        if (render_graph_enabled && !render_graph_fallback_latched) {
            RenderGraph graph("RasterFrame");
            auto import_physical_texture = [&](std::string_view name, Texture* physical_texture) {
                assert(physical_texture != nullptr);
                RenderGraph::TextureAspect aspects = RenderGraph::TextureAspect::None;
                const auto rhi_aspects = physical_texture->GetAspectFlags();
                if (uint32_t(rhi_aspects & ETextureAspectFlags::COLOR) != 0) {
                    aspects = aspects | RenderGraph::TextureAspect::Color;
                }
                if (uint32_t(rhi_aspects & ETextureAspectFlags::DEPTH_SLICE) != 0) {
                    aspects = aspects | RenderGraph::TextureAspect::Depth;
                }
                if (uint32_t(rhi_aspects & ETextureAspectFlags::STENCIL_SLICE) != 0) {
                    aspects = aspects | RenderGraph::TextureAspect::Stencil;
                }
                assert(aspects != RenderGraph::TextureAspect::None);
                return graph.ImportTexture(
                    name,
                    physical_texture,
                    RenderGraph::TextureDesc{
                        .mip_count = physical_texture->GetNumMips(),
                        .layer_count = physical_texture->GetNumArray(),
                        .aspects = aspects
                    }
                );
            };
            auto import_texture = [&](std::string_view name, const auto& texture) {
                // DepthBufferRef wraps a TextureRef while ordinary texture handles point at the
                // Texture directly. Canonicalize every texture import to the TextureView's physical
                // Texture so aliases (including the two depth sampler wrappers and the editor view)
                // resolve to one graph resource.
                return import_physical_texture(name, texture->GetView().GetTexture());
            };

            graph_resources.scene = graph.ImportToken("scene", render_scene.get());
            graph_resources.shadow_maps =
                graph.ImportToken("shadow_maps", &raster_context.csm_data);
            graph_resources.probe_volume =
                graph.ImportToken("probe_volume", &raster_context.probe_volume);
            auto* lighting_data_buffer = raster_context.lighting_data_buffer.buf.Get();
            assert(lighting_data_buffer != nullptr);
            graph_resources.lighting_data = graph.ImportBuffer(
                "lighting_data",
                lighting_data_buffer,
                RenderGraph::BufferDesc{.byte_size = lighting_data_buffer->GetByteSize()}
            );
            const auto& scene_light_buffer = raster_context.GetGpuSceneRes().light_buf.buf;
            if (scene_light_buffer) {
                graph_resources.scene_lights = graph.ImportBuffer(
                    "scene_lights",
                    scene_light_buffer.Get(),
                    RenderGraph::BufferDesc{.byte_size = scene_light_buffer->GetByteSize()}
                );
            }
            graph_resources.base_color =
                import_texture("base_color", raster_context.textures.base_color.tex);
            graph_resources.normal = import_texture("normal", raster_context.textures.normal.tex);
            graph_resources.metal_rough_ao =
                import_texture("metal_rough_ao", raster_context.textures.metal_rough_ao.tex);
            graph_resources.depth =
                import_texture("depth", raster_context.textures.depth_linear_sampler.tex);
            graph_resources.cubemap =
                import_texture("cubemap", raster_context.textures.cubemap_tex.tex);
            const auto depth_nearest_alias =
                import_texture("depth_nearest_sampler", raster_context.textures.depth_nearest_sampler.tex);
            assert(depth_nearest_alias == graph_resources.depth);
            graph_resources.hiz_current =
                import_texture("hiz_current", raster_context.textures.hiz_current.tex);
            graph_resources.hiz_previous =
                import_texture("hiz_previous", raster_context.textures.hiz_previous.tex);
            graph_resources.shadow_mask =
                import_texture("shadow_mask", raster_context.textures.shadow_mask.tex);
            graph_resources.lighting_output =
                import_texture("lighting_output", raster_context.textures.lighting_output.tex);
            graph_resources.ao_working_set = graph.ImportToken("ao_working_set", ao_pass.get());
            graph_resources.motion_vectors =
                graph.ImportToken("motion_vectors", rtao_denoiser_pass.get());
            graph_resources.ao_output =
                import_texture("ao_output", raster_context.textures.ao_output.tex);
            graph_resources.denoiser_output =
                import_texture("denoiser_output", raster_context.textures.denoiser_output.tex);
            graph_resources.ssr_output =
                import_texture("ssr_output", raster_context.textures.ssr_output.tex);
            graph_resources.aa_output =
                import_texture("aa_output", raster_context.textures.aa_output.tex);
            graph_resources.bloom_chain = graph.ImportToken("bloom_chain", bloom_pass.get());
            graph_resources.tonemapping_state =
                graph.ImportToken("tonemapping_state", tonemapping_pass.get());
            graph_resources.tonemapping_output =
                import_texture("tonemapping_output", raster_context.textures.tonemapping_output.tex);
            if (frame_packet.ui_composition.enabled) {
                graph_resources.selected_framebuffer = import_physical_texture(
                    "selected_framebuffer", selected_framebuffer_view.GetTexture()
                );

                RenderGraph::TextureHandle expected_validation_resource;
                const auto& validation_name = frame_packet.validation_selected_frame_buffer_name;
                if (validation_name == "base_color") {
                    expected_validation_resource = graph_resources.base_color;
                } else if (validation_name == "normal") {
                    expected_validation_resource = graph_resources.normal;
                } else if (validation_name == "depth_linear_sampler") {
                    expected_validation_resource = graph_resources.depth;
                } else if (validation_name == "tonemapping_output") {
                    expected_validation_resource = graph_resources.tonemapping_output;
                }
                if (expected_validation_resource.IsValid() &&
                    graph_resources.selected_framebuffer != expected_validation_resource) {
                    render_graph_fallback_latched = true;
                    LOG_ERROR(
                        "[RenderGraph][Fallback] Raster framebuffer validation target '{}' did not "
                        "alias its canonical graph resource. Using the linear path for this renderer "
                        "instance.",
                        validation_name
                    );
                }
            }
            if (ui_writes_external_window) {
                graph_resources.window_framebuffer = import_physical_texture(
                    "window_framebuffer", window_framebuffer_view.GetTexture()
                );
            }
            graph_resources.output =
                import_texture("output", raster_context.textures.output.tex);
            graph_resources.processing_image = graph.CreateTransientToken("processing_image");
            graph_resources.parallel_record_snapshots =
                graph.CreateTransientToken("parallel_record_snapshots");
            graph_resources.hiz_recorded = graph.CreateTransientToken("hiz_recorded");

            if (render_graph_fallback_latched) {
                execute_linear();
            } else {
                auto graph_schedule = [&](std::string_view                    name,
                                          auto&&                              setup,
                                          RenderGraph::PassExecutionClass     execution_class,
                                          auto&&                              execute) {
                    const std::string marker_name = std::format("Pass: {}", name);
                    if (execution_class == RenderGraph::PassExecutionClass::MainThread ||
                        execution_class == RenderGraph::PassExecutionClass::CpuPrepare ||
                        execution_class == RenderGraph::PassExecutionClass::ExternalControl) {
                        graph.AddPass(
                            name,
                            std::forward<decltype(setup)>(setup),
                            [&, marker_name, execution_class,
                             execute = std::forward<decltype(execute)>(execute)]() mutable {
                                // CPU-only callbacks record no commands; external-control callbacks own
                                // their explicit submission scope. Neither may inherit a managed marker.
                                if (execution_class == RenderGraph::PassExecutionClass::CpuPrepare ||
                                    execution_class ==
                                        RenderGraph::PassExecutionClass::ExternalControl) {
                                    execute(cmd_list);
                                    return;
                                }
                                ScopedGpuMarker pass_marker(
                                    cmd_list, marker_name, GpuMarkerPalette::Pass()
                                );
                                execute(cmd_list);
                            }
                        );
                        return;
                    }

                    graph.AddRecordPass(
                        name,
                        std::forward<decltype(setup)>(setup),
                        [marker_name,
                         execute = std::forward<decltype(execute)>(execute)](
                            CommandList& recording_cmd_list
                        ) mutable {
                            ScopedGpuMarker pass_marker(
                                recording_cmd_list, marker_name, GpuMarkerPalette::Pass()
                            );
                            execute(recording_cmd_list);
                        },
                        execution_class
                    );
                };
                define_raster_passes(graph_schedule);
                graph.Export(graph_resources.output);
                if (ui_writes_external_window) {
                    graph.Export(graph_resources.window_framebuffer);
                }

                if (graph.Compile()) {
                    if (render_graph_debug_dump) {
                        std::string dump = graph.Dump();
                        if (logged_render_graph_dumps.emplace(dump).second) {
                            LOG_INFO("[RenderGraph][DebugDump]\n{}", dump);
                        }
                    }
                    // A long-lived renderer/graph marker cannot cross the per-pass Submit boundaries.
                    // Every pass still owns its own marker and the immutable source packet carries a
                    // stable frame/pass debug label.
                    renderer_marker.Close();
                    const auto submit_main_thread_pass =
                        [&](const RenderGraph::ExecutedPassInfo& pass) {
                            assert(
                                pass.domain.queue == RenderGraph::QueueRole::Graphics &&
                                "Raster RDG pass recorded into a Graphics CommandList but declared another queue"
                            );
                            if (cmd_list.IsEmpty()) {
                                return;
                            }
                            CmdSubmit pass_submit = cmd_list.Submit();
                            pass_submit.DebugLabel(
                                std::format(
                                    "Raster Frame {}/{}",
                                    frame_packet.frame_id,
                                    pass.name
                                ),
                                GpuMarkerPalette::Pass()
                            );
                            // Preserve the old one-frame timestamp transaction even though RDG
                            // now seals several source submissions.  The first source resets the
                            // frame query range and opens Graphics Exec; intermediate sources only
                            // emit their pass queries; the UI/frame-tail source closes and advances
                            // the profiler below.  This also survives TensorRT's explicit mid-frame
                            // Flush/Sync boundary.
                            pass_submit.SetProfilingPhase(
                                split_graph_profiling_frame ?
                                    ERHIProfilingPhase::Continue :
                                    ERHIProfilingPhase::Begin
                            );
                            split_graph_profiling_frame = true;
                            // Until RDG barrier/ownership metadata is lowered into the runtime
                            // topology, pass packets remain an explicit ordered-control stream.
                            // Their command bodies can still use the lower parallel recorder.
                            pass_submit.SetTranslateExecutionClass(
                                ERHITranslateExecutionClass::SerialControl
                            );
                            RHIExecutor::Get().Submit(
                                EQueueType::Graphics,
                                std::move(pass_submit),
                                ERHIExecSubmitFlags::None
                            );
                        };
                    const auto configure_recording_source =
                        [&](const RenderGraph::ExecutedPassInfo& pass,
                            RHIRecordingSource&                  source) {
                            assert(
                                pass.domain.queue == RenderGraph::QueueRole::Graphics &&
                                "Raster RDG record source declared a non-Graphics queue"
                            );
                            const auto profiling_phase =
                                split_graph_profiling_frame ?
                                    ERHIProfilingPhase::Continue :
                                    ERHIProfilingPhase::Begin;
                            split_graph_profiling_frame = true;
                            const std::string debug_label = std::format(
                                "Raster Frame {}/{}", frame_packet.frame_id, pass.name
                            );
                            source.submit_metadata.debug_label       = debug_label;
                            source.submit_metadata.debug_label_color = GpuMarkerPalette::Pass();
                            source.submit_metadata.profiling_phase   = profiling_phase;
                            // RDG barrier/ownership lowering is still guarded; keep the backend
                            // translator ordered even though CPU recording overlaps.
                            source.submit_metadata.translate_execution_class =
                                ERHITranslateExecutionClass::SerialControl;
                        };
                    if (!graph.ExecuteRecording(
                            submit_main_thread_pass,
                            configure_recording_source,
                            parallel_recording_enabled
                        )) {
                        render_graph_fallback_latched = true;
                        LOG_ERROR(
                            "[RenderGraph][Fallback] Raster graph recording failed after execution began: {}. "
                            "The linear path will be used from the next frame.",
                            graph.GetCompileError()
                        );
                    }
                } else {
                    render_graph_fallback_latched = true;
                    LOG_ERROR(
                        "[RenderGraph][Fallback] Raster graph compile failed before execution: {}. "
                        "Using the linear path for this renderer instance.",
                        graph.GetCompileError()
                    );
                    execute_linear();
                }
            }
        } else {
            execute_linear();
        }

        if (raster_config.debug_fps_limit_enable) {
            // 此调试限帧器有意保持简单，不补偿当前帧耗时。
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
    // Host 同步的 copy 操作已经完成。此处触发 timeline，向 validation layer 传递执行顺序，
    // 无需再额外等待 copy queue。
    CmdSubmit frame_submit =
        cmd_list.Submit()
            .DebugLabel(
                std::format("Raster Frame {}", frame_packet.frame_id),
                GpuMarkerPalette::Frame()
            )
            .Signal(timeline, time)
            .DeleteResources();
    if (split_graph_profiling_frame) {
        frame_submit.SetProfilingPhase(ERHIProfilingPhase::End);
    } else {
        frame_submit.TickProfiling();
    }

    std::optional<RHIPresentRequest> present_request{};

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
            main_present_receipt =
                CreateMainPresentReceipt(frame_packet.scene_updates.scene_ready);
            present_request.emplace(swapchain, output_view, main_present_receipt);
        } else {
            LOG_WARNING(
                "Skipping stale main-window present: source={}x{}, swapchain={}x{}.",
                output_view.extent.x,
                output_view.extent.y,
                swapchain->size.x,
                swapchain->size.y
            );
        }
    }

    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(frame_submit),
        ERHIExecSubmitFlags::FlushGPU,
        present_request ? &*present_request : nullptr
    );

    if (!skip_present) {
        PresentUiDrawFrame(frame_packet.ui_draw_frame, ui_execution_thread);
    }

    RasterFrameFeedback feedback{};
    feedback.frame_id              = frame_packet.frame_id;
    feedback.main_present_receipt  = std::move(main_present_receipt);
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
    return !hooks.should_reload || !hooks.should_reload();
}

void RasterRenderer::ApplyFrameFeedback(
    RasterFrameFeedback feedback,
    RasterConfig&       target_config,
    const EngineHooks&  hooks
) {
    assert(IsCurrentlyGameThread());
    ApplyMainPresentReceipt(feedback.main_present_receipt, hooks);
    target_config.culling_stats          = feedback.culling_stats;
    target_config.cooperative_ops_status = feedback.cooperative_ops_status;
    if (!feedback.displayable_frame_buffer_names.empty() &&
        hooks.on_raster_register_frame_buffer_names) {
        hooks.on_raster_register_frame_buffer_names(feedback.displayable_frame_buffer_names);
    }
}

} // namespace Moer::Render::Raster
