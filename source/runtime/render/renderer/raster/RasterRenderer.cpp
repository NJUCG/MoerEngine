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
#include "debug/RenderDocApi.h"
#include "misc/ScopedLogTimer.h"
#include "profile/ProfileScope.h"
#include "profile/RenderProfileCapture.h"
#include "renderer/common/UiFrameGraphPass.h"
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
    uint2                         initial_resolution,
    SharedPtr<EditorConfig>       config,
    RenderGraphExecutionConfig    graph_config,
    SwapchainSurfaceInfo          main_window_surface,
    RenderProfileCapture*         render_profile_capture
) :
    Renderer(initial_resolution, config, std::move(main_window_surface), render_profile_capture),
    scene_render_extent_tracker(initial_resolution) {
    ScopedLogTimer startup_timer("[Startup][RasterRenderer] RasterRenderer::Constructor() total");

    render_graph_enabled       = graph_config.enabled;
    render_graph_debug_dump    = graph_config.debug_dump;
    parallel_recording_enabled = graph_config.parallel_recording;
    LOG_INFO(
        "[RenderGraph] Raster execution mode: {}, upper recording: {}",
        render_graph_enabled ? "graph" : "linear",
        render_graph_enabled ?
            (parallel_recording_enabled ?
                 "parallel-frontend/merged-submit" :
                 "serial-frontend/merged-submit") :
            "linear"
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

    raster_context.CreateFrameBuffers(resolution, resolution);
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

struct LightingUploadRecordParameters {
    LightingData data{};
    BufferRef    output{};
};

[[nodiscard]] static LightingUploadRecordParameters PrepareGlobalLightingData(
    RasterContext&      context,
    const RasterConfig& ui_config,
    const Camera&       camera
) {
    const uint    cascade_count = ui_config.shadow_csm_num_of_cascades;
    LightingData  prepared_data = context.lighting_data;
    LightingData* lighting_data = &prepared_data;

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

    return LightingUploadRecordParameters{
        .data = std::move(prepared_data),
        .output = context.lighting_data_buffer.buf
    };
}

static void RecordGlobalLightingData(
    CommandList& cmd_list,
    const LightingUploadRecordParameters& parameters
) {
    cmd_list.CopyFrom(
        std::span<const byte>(
            reinterpret_cast<const byte*>(&parameters.data), sizeof(LightingData)
        ),
        parameters.output->GetView()
    );
}

class LightingUploadPass {
public:
    struct GraphResources {
        RenderGraph::TokenHandle  shadow_maps{};
        RenderGraph::TokenHandle  probe_volume{};
        RenderGraph::BufferHandle lighting_data{};
    };

    RenderGraph::PreparedPassHandle AddToGraph(
        RenderGraph& graph,
        RasterContext& context,
        const RasterConfig& config,
        const Camera& camera,
        GraphResources resources,
        std::span<const RenderGraph::SetupPassHandle> setup_dependencies
    ) const {
        auto prepared = graph.AddSetupPass(
            "UploadLightingData.Prepare",
            uint8_t{0},
            [&context, &config, &camera](const uint8_t&) {
                return PrepareGlobalLightingData(context, config, camera);
            },
            setup_dependencies
        );
        if (!prepared.IsValid()) {
            return {};
        }

        auto pass = graph.AddRecordPass(
            "UploadLightingData",
            [resources](RenderGraph::PassBuilder& builder) {
                builder.Read(resources.shadow_maps)
                    .Read(resources.probe_volume)
                    .Write(resources.lighting_data)
                    .SideEffect();
            },
            [prepared](CommandList& cmd_list) {
                ScopedGpuMarker marker(
                    cmd_list, "Pass: UploadLightingData", GpuMarkerPalette::Pass()
                );
                RecordGlobalLightingData(cmd_list, prepared.Get());
            },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
        return {prepared.GetSetupPass(), pass};
    }
};

RasterFramePacket
RasterRenderer::PrepareFrame(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyGameThread());
    assert(editor_config);
    if (hooks.on_tick_engine_control) {
        hooks.on_tick_engine_control();
    }
    MOER_PROFILE_SCOPE("Raster.PrepareFrame");

    const bool           profile_logging = IsFramePrepareProfilingEnabled();
    const auto           prepare_started = BeginFramePrepareProfile();
    FramePrepareProfile  prepare_profile{};
    FramePrepareWorkload prepare_workload{};
    RasterFramePacket    frame_packet{};
    frame_packet.frame_id = next_frame_id++;
    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.window_ms);
        LogSceneLoadStatus(*editor_config);
        frame_packet.window = TickWindowContext();
        const uint2 stable_drawable_resolution =
            frame_packet.window.GetStableDrawableResolution();
        if (stable_drawable_resolution.x != 0 && stable_drawable_resolution.y != 0) {
            editor_config->SetResolution(stable_drawable_resolution);
        }
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

    const WindowInputFrameSnapshot window_input =
        hooks.on_capture_window_input ? hooks.on_capture_window_input() : WindowInputFrameSnapshot{};
    CameraFrameInput camera_input{};
    bool             is_run_scene_test_case = false;
    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.camera_and_test_ms);
        camera_input = CameraFrameInput::Capture(window_input, *editor_config);
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
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.ui_draw_packet_ms);
        if (hooks.on_capture_ui_draw_frame) {
            frame_packet.ui_draw_frame = hooks.on_capture_ui_draw_frame();
        }
        BindUiViewportWindowFrame(
            frame_packet.ui_draw_frame.main_viewport,
            frame_packet.window
        );
        CaptureFramePrepareUiWorkload(prepare_workload, frame_packet.ui_draw_frame);
    }
    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.ui_composition_ms);
        if (hooks.on_capture_ui_composition) {
            frame_packet.ui_composition = hooks.on_capture_ui_composition();
        }
        if (!ResolveUiCompositionDrawableMetrics(
                frame_packet.ui_composition,
                frame_packet.window,
                frame_packet.ui_draw_frame
            )) {
            frame_packet.ui_composition.enabled             = false;
            frame_packet.ui_composition.window_frame_buffer = nullptr;
        }
        frame_packet.scene_render_extent = CaptureSceneRenderExtentRequest(
            frame_packet.ui_composition.scene_extent_resolved,
            frame_packet.ui_composition.scene_color_resolution,
            frame_packet.window.GetStableDrawableResolution()
        );
    }

    if (frame_packet.frame_id == 0) {
        LOG_INFO(
            "[Threading] RasterFramePacket boundary active; resolution={}x{}, UI main vertices={}, "
            "platform viewports={}.",
            frame_packet.window.GetStableDrawableResolution().x,
            frame_packet.window.GetStableDrawableResolution().y,
            frame_packet.ui_draw_frame.main_viewport.vertices.size(),
            frame_packet.ui_draw_frame.platform_viewports.size()
        );
    }

    RecordFramePrepareProfile("Raster", prepare_started, prepare_profile, prepare_workload);
    return frame_packet;
}

RasterFrameFeedback RasterRenderer::RenderFrame(RasterFramePacket frame_packet) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyRenderThread());
    MOER_PROFILE_SCOPE("Raster.RenderFrame");

    RenderProfileFrameToken gpu_profile_frame{};
    if (render_profile_capture != nullptr) {
        static_cast<void>(render_profile_capture->DrainReadyFrames());
        gpu_profile_frame = render_profile_capture->BeginFrame();
    }
    const RHIQueueBinding graphics_profile_binding =
        device.GetQueueTopology().Resolve(EQueueType::Graphics);
    const RHIQueueBinding copy_profile_binding =
        device.GetQueueTopology().Resolve(EQueueType::Copy);
    auto try_bind_profile_source =
        [&](CommandList&    command_list,
            RHIQueueBinding queue_binding,
            uint64          source_order) noexcept {
            if (render_profile_capture == nullptr || !gpu_profile_frame.Valid()) {
                return false;
            }
            return render_profile_capture->BindSource(
                       gpu_profile_frame,
                       command_list,
                       queue_binding,
                       source_order
                   ) == RenderProfileBindResult::Bound;
        };
    auto ensure_profile_source =
        [&](CommandList&    command_list,
            RHIQueueBinding queue_binding,
            uint64          source_order) noexcept {
            if (command_list.HasGpuScopeRecorder()) {
                return true;
            }
            if (!gpu_profile_frame.Valid() ||
                command_list.IsLegacyGpuProfilingSuppressedForGeneration()) {
                return false;
            }
            if (try_bind_profile_source(
                    command_list, queue_binding, source_order
                )) {
                return true;
            }
            try {
                command_list.SuppressLegacyGpuProfilingForGeneration();
            } catch (...) {
                // Profiling degradation must not change the render path.
            }
            return false;
        };
    // Order zero is reserved for work already resident in the persistent
    // Raster CommandList (resize/bindless prefix). Scene-update generations
    // advance this cursor before RDG/linear recording chooses its base.
    uint64 next_profile_source_order = 1;
    uint64 linear_source_order       = 1;
    uint64 tail_source_order         = 1;

    auto& raster_context            = *raster_context_ptr;
    auto& main_presentation_surface = GetMainPresentationSurface();
    bool  main_presentation_current =
        PrepareRenderFrame(frame_packet.window);
    if (main_presentation_current) {
        main_presentation_current = RetargetMainUiPresentation(
            frame_packet.ui_draw_frame.main_viewport,
            frame_packet.ui_composition,
            frame_packet.window,
            Extent2D(resolution.x, resolution.y)
        );
        frame_packet.scene_render_extent = CaptureSceneRenderExtentRequest(
            frame_packet.ui_composition.scene_extent_resolved,
            frame_packet.ui_composition.scene_color_resolution,
            resolution
        );
    }
    auto scene_extent_request = frame_packet.scene_render_extent;
    // An OS-window resize already requires a presentation rebuild. Accept the
    // matching SceneColor layout immediately so the two resource domains move
    // together; dock-only drags retain the two-observation debounce.
    if (frame_packet.window.IsDrawable() &&
        frame_packet.window.transition != EWindowFrameTransition::Stable &&
        frame_packet.window.transition != EWindowFrameTransition::LogicalResized &&
        scene_extent_request.valid) {
        scene_extent_request.immediate = true;
    }
    (void)scene_render_extent_tracker.Observe(scene_extent_request);
    raster_context.BeginSceneFrame(frame_packet.scene_updates);

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
    if (!main_presentation_current ||
        !frame_packet.window.IsDrawable() ||
        !main_presentation_surface.IsCurrent(frame_packet.window)) {
        // 窗口最小化后无法 present，此处主动让出线程，避免高频空转。
        std::this_thread::yield();
        skip_present = true;
    }

    const uint2 active_scene_extent      = scene_render_extent_tracker.GetActiveExtent();
    const uint2 presentation_resolution = resolution;
    const bool  recreate_scene_resources =
        !EqualRenderExtent(raster_context.GetResolution(), active_scene_extent);
    const bool recreate_output_resources =
        !EqualRenderExtent(raster_context.GetOutputResolution(), presentation_resolution);

    const bool recreate_frame_resources =
        !skip_present && (recreate_scene_resources || recreate_output_resources);
    const bool resize_prefix_profile_bound =
        recreate_frame_resources &&
        ensure_profile_source(cmd_list, graphics_profile_binding, 0);
    if (recreate_frame_resources) {
        raster_context.FreeFrameBuffers(
            false, recreate_scene_resources, recreate_output_resources
        );
        raster_context.CreateFrameBuffers(
            active_scene_extent,
            presentation_resolution,
            recreate_scene_resources,
            recreate_output_resources
        );
        // External assets are resolution-independent and retain their bindless
        // handles. Only the selected resource domains are allocated again.
        if (resize_prefix_profile_bound) {
            ScopedGpuMarker resize_marker(
                cmd_list,
                "Raster Resource Resize",
                GpuMarkerPalette::Transfer(),
                EGpuMarkerMode::Timestamp
            );
            raster_context.AllocateFrameBuffers(
                recreate_scene_resources, recreate_output_resources
            );
        } else {
            raster_context.AllocateFrameBuffers(
                recreate_scene_resources, recreate_output_resources
            );
        }

        if (recreate_scene_resources) {
            raster_context.csm_data.shadow_cache_config_snapshot_valid = false;
            aa_pass->ResetHistory();
            rtao_denoiser_pass->ResetHistory();

#if WITH_CUDA
            cuda_pass->RecreateResource(raster_context.textures.ao_output.tex);
            tensor_rt_pass->RecreateResource(
                raster_context,
                raster_context.textures.ao_output_ambient_only.tex,
                raster_context.textures.depth_linear_sampler.tex->CastToTextureRef(),
                raster_context.textures.lighting_output.tex,
                raster_context.textures.camera_motion_vector.tex,
                raster_context.textures.ao_output_ambient_only_1.tex
            );
#endif
        }

        render_extent_generation++;
        LOG_INFO(
            "[RenderExtent][Raster] generation={} requested={}x{} request_valid={} "
            "active_scene={}x{} presentation_output={}x{} swapchain={}x{} "
            "scene_recreated={} output_recreated={} temporal_history_reset={}.",
            render_extent_generation,
            frame_packet.scene_render_extent.extent.x,
            frame_packet.scene_render_extent.extent.y,
            frame_packet.scene_render_extent.valid,
            raster_context.GetResolution().x,
            raster_context.GetResolution().y,
            raster_context.GetOutputResolution().x,
            raster_context.GetOutputResolution().y,
            main_presentation_surface.GetExtent().x,
            main_presentation_surface.GetExtent().y,
            recreate_scene_resources,
            recreate_output_resources,
            recreate_scene_resources
        );
    }

    // Modern RDG/source binding requires an empty caller-thread list. Publish
    // any already-recorded resize prefix before scene-update Copy/Graphics
    // sources so source_order remains consistent with execution order.
    if (gpu_profile_frame.Valid() && !cmd_list.IsEmpty()) {
        CmdSubmit prefix_submit = cmd_list.Submit().DebugLabel(
            std::format("Raster Frame {}/Prefix", frame_packet.frame_id),
            GpuMarkerPalette::Transfer()
        );
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            std::move(prefix_submit),
            ERHIExecSubmitFlags::None
        );
    }

    TextureRef default_output_texture = raster_context.textures.output.tex;
    bool       raster_pipeline_recorded = false;
    Matrix4x4f raster_commit_view_proj  = Matrix4x4f::Identity();
    uint       raster_commit_ao_index   = 0u;
    uint8      raster_commit_aa_phase   = 0u;

    // 窗口资源就绪后再使用已准备好的场景快照。

    if (frame_packet.scene_updates.scene_ready) {
        // 处理场景加载过程中遗留的命令
        auto& scene_updates = frame_packet.scene_updates;
        const auto setup_scene_update_profiling =
            [&](GpuScene::PendingCommandList& commands,
                bool                          full_rebuild) {
                if (full_rebuild) {
                    const uint64 copy_source_order =
                        next_profile_source_order++;
                    static_cast<void>(ensure_profile_source(
                        commands.copy_queue_cmd_list,
                        copy_profile_binding,
                        copy_source_order
                    ));
                }
                const uint64 graphics_source_order =
                    next_profile_source_order++;
                static_cast<void>(ensure_profile_source(
                    commands.gfx_queue_cmd_list,
                    graphics_profile_binding,
                    graphics_source_order
                ));
            };
        if (scene_updates.initial_gpu_update) {
            auto commands = render_scene->ApplyUpdate(
                std::move(*scene_updates.initial_gpu_update),
                setup_scene_update_profiling
            );
            RasterTool::ExecuteScenePendingCommands(
                std::move(commands), device, gfx_queue
            );
        }

        if (first_load) {
            first_load = false;

            // 第一个 Raster Pass 执行前，先发布首次场景上传创建的 descriptor。
            const uint64 descriptor_source_order =
                next_profile_source_order++;
            const bool descriptor_profile_bound = ensure_profile_source(
                cmd_list,
                graphics_profile_binding,
                descriptor_source_order
            );
            if (descriptor_profile_bound) {
                ScopedGpuMarker prefix_marker(
                    cmd_list,
                    "Raster First Load Descriptor",
                    GpuMarkerPalette::Transfer(),
                    EGpuMarkerMode::Timestamp
                );
                cmd_list.UpdateBindlessArray(bindless_array);
            } else {
                cmd_list.UpdateBindlessArray(bindless_array);
            }
            RHIExecutor::Get().Submit(
                EQueueType::Graphics, cmd_list.Submit(), ERHIExecSubmitFlags::FlushGPU
            );
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        }

        auto& raster_config = frame_packet.raster_config;

        const auto& scene_tick_state = scene_updates.tick_state;
        if (scene_updates.update_gpu_update) {
            auto commands = render_scene->ApplyUpdate(
                std::move(*scene_updates.update_gpu_update),
                setup_scene_update_profiling
            );
            RasterTool::ExecuteScenePendingCommands(
                std::move(commands), device, gfx_queue
            );
        }

        // ExecuteFrontendRecordingPlan owns modern per-source binding and
        // requires the caller-thread list to be empty at graph entry. Preserve
        // the legacy renderer label only when no modern frame was admitted.
        std::optional<ScopedGpuMarker> renderer_marker{};
        if (!gpu_profile_frame.Valid()) {
            renderer_marker.emplace(
                cmd_list, "Raster Renderer", GpuMarkerPalette::Renderer()
            );
        }

        const Camera& main_camera = scene_updates.main_camera;
        Camera        camera      = frame_packet.render_camera;
        const uint8 smaa_t2x_phase =
            raster_config.aa_mode == EAaMode::SMAA_T2X ?
                aa_pass->NextSmaaT2xPhase() :
                0u;

        {
            // 在不修改已捕获相机的前提下，交替使用两个 SMAA T2x 亚像素偏移。
            if (raster_config.aa_mode == EAaMode::SMAA_T2X) {
                static const StaticArray<float2, 2> s_smaa_jitter_offsets = {
                    float2(0.25f, -0.25f),
                    float2(-0.25f, 0.25f)
                };
                camera.SetJitterMatrix(
                    s_smaa_jitter_offsets[smaa_t2x_phase],
                    raster_context.GetResolution()
                );
            }
        }

        raster_commit_view_proj = camera.GetViewProjectionMatrix();
        raster_commit_aa_phase  = smaa_t2x_phase;
        raster_context.Update(camera.GetDeltaTime());
        cooperative_ops_pass->Commit(
            raster_config, cooperative_ops_pass->Prepare(raster_config)
        );

        if (scene_tick_state.updated_transform || scene_tick_state.rebuilt_mesh) {
            raster_context.csm_data.shadow_cache_config_snapshot_valid = false;
            raster_context.InvalidateHiZHistory();
        }

        const auto& scene_gizmos = frame_packet.scene_view_gizmos;
        const bool  draw_scene_gizmos =
            frame_packet.active_viewport_mode == EEditorViewportMode::Scene && scene_gizmos.enabled;
        AoPass::AoPassOutput ao_result{};
        TextureWithHandle   processing_image = raster_context.textures.ao_output;
        RenderGraph::TextureHandle processing_image_resource{};
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
            RenderGraph::TextureHandle ao_only;
            RenderGraph::TextureHandle camera_motion_vector;
            RenderGraph::TextureHandle ao_history_read;
            RenderGraph::TextureHandle ao_history_write;
            RenderGraph::BufferHandle  motion_vector_data;
            RenderGraph::TextureHandle ao_output;
            RenderGraph::TextureHandle denoiser_output;
            RenderGraph::TextureHandle ssr_output;
            RenderGraph::TextureHandle aa_output;
            RenderGraph::TextureHandle bloom_downsample_chain;
            RenderGraph::TextureHandle bloom_upsample_chain;
            RenderGraph::BufferHandle  tonemapping_histogram;
            RenderGraph::BufferHandle  tonemapping_exposure;
            RenderGraph::TextureHandle tonemapping_output;
            RenderGraph::TextureHandle selected_framebuffer;
            RenderGraph::TextureHandle window_framebuffer;
            RenderGraph::TextureHandle output;
            RenderGraph::BufferHandle  scene_lights;
        } graph_resources{};

        auto define_raster_passes = [&](auto&& dispatch, auto&& dispatch_prepared) {
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
            auto schedule_prepared =
                [&](std::string_view name,
                    auto&& setup,
                    auto&& prepare,
                    auto&& record,
                    RenderGraph::PrepareSafety prepare_safety =
                        RenderGraph::PrepareSafety::Restricted) {
                    return dispatch_prepared(
                        name,
                        std::forward<decltype(setup)>(setup),
                        RenderGraph::PassExecutionClass::ParallelRecordEligible,
                        std::forward<decltype(prepare)>(prepare),
                        std::forward<decltype(record)>(record),
                        std::span<const RenderGraph::SetupPassHandle>{},
                        prepare_safety
                    );
                };
            auto schedule_prepared_after =
                [&](std::string_view name,
                    RenderGraph::SetupPassHandle dependency,
                    auto&& setup,
                    auto&& prepare,
                    auto&& record,
                    RenderGraph::PrepareSafety prepare_safety =
                        RenderGraph::PrepareSafety::Restricted) {
                    const StaticArray<RenderGraph::SetupPassHandle, 1> dependencies{
                        dependency
                    };
                    return dispatch_prepared(
                        name,
                        std::forward<decltype(setup)>(setup),
                        RenderGraph::PassExecutionClass::ParallelRecordEligible,
                        std::forward<decltype(prepare)>(prepare),
                        std::forward<decltype(record)>(record),
                        std::span<const RenderGraph::SetupPassHandle>(dependencies),
                        prepare_safety
                    );
                };
            auto schedule_prepared_after_two =
                [&](std::string_view name,
                    RenderGraph::SetupPassHandle first_dependency,
                    RenderGraph::SetupPassHandle second_dependency,
                    auto&& setup,
                    auto&& prepare,
                    auto&& record,
                    RenderGraph::PrepareSafety prepare_safety =
                        RenderGraph::PrepareSafety::Restricted) {
                    const StaticArray<RenderGraph::SetupPassHandle, 2> dependencies{
                        first_dependency,
                        second_dependency
                    };
                    return dispatch_prepared(
                        name,
                        std::forward<decltype(setup)>(setup),
                        RenderGraph::PassExecutionClass::ParallelRecordEligible,
                        std::forward<decltype(prepare)>(prepare),
                        std::forward<decltype(record)>(record),
                        std::span<const RenderGraph::SetupPassHandle>(dependencies),
                        prepare_safety
                    );
                };
            const auto shadow_depth_pass_handle = schedule_prepared(
                "ShadowDepth",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.scene)
                        .Write(graph_resources.shadow_maps)
                        .SideEffect();
                },
                [&]() {
                    return shadow_depth_pass->Prepare(raster_context, raster_config, camera);
                },
                [&](CommandList& recording_cmd_list,
                    const ShadowDepthPass::RecordParameters& parameters) {
                    recording_cmd_list.PushScopeWithTimeScope(
                        RasterTool::GetShadowDepthPassProfileScopeName()
                    );
                    shadow_depth_pass->Record(recording_cmd_list, parameters);
                    recording_cmd_list.PopScopeWithTimeScope();
                },
                // Updates the persistent shadow cache and shadow resources.
                RenderGraph::PrepareSafety::Unsafe
            );
            const auto probe_update_pass_handle = schedule_prepared(
                "ProbeUpdate",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.scene)
                        .ReadWrite(graph_resources.probe_volume)
                        .SideEffect();
                },
                [&]() {
                    return probe_update_pass->Prepare(
                        raster_context, raster_config, camera, time
                    );
                },
                [&](CommandList& recording_cmd_list,
                    const ProbeUpdatePass::RecordParameters& parameters) {
                    probe_update_pass->Record(recording_cmd_list, parameters);
                },
                // Advances persistent probe-volume streaming and layout state.
                RenderGraph::PrepareSafety::Unsafe
            );
            schedule_prepared_after_two(
                "UploadLightingData",
                shadow_depth_pass_handle.setup,
                probe_update_pass_handle.setup,
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.shadow_maps)
                        .Read(graph_resources.probe_volume)
                        .Write(graph_resources.lighting_data)
                        .SideEffect();
                },
                [&]() {
                    return PrepareGlobalLightingData(raster_context, raster_config, camera);
                },
                [&](CommandList& recording_cmd_list,
                    const LightingUploadRecordParameters& parameters) {
                    RecordGlobalLightingData(recording_cmd_list, parameters);
                }
            );
            schedule_prepared_after(
                "Geometry",
                shadow_depth_pass_handle.setup,
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.scene)
                        .Read(graph_resources.hiz_previous)
                        .Write(graph_resources.base_color)
                        .Write(graph_resources.normal)
                        .Write(graph_resources.metal_rough_ao)
                        .Write(graph_resources.depth);
                },
                [&]() {
                    return geometry_pass->Prepare(raster_context, raster_config, camera);
                },
                [&](CommandList& recording_cmd_list,
                    const GeometryPass::RecordParameters& parameters) {
                    recording_cmd_list.PushScopeWithTimeScope(
                        RasterTool::GetGeometryPassProfileScopeName()
                    );
                    geometry_pass->Record(recording_cmd_list, parameters);
                    recording_cmd_list.PopScopeWithTimeScope();
                },
                // Publishes culling statistics into the frame's mutable config.
                RenderGraph::PrepareSafety::Unsafe
            );
            schedule_prepared(
                "TessellatedSurface",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.ReadWrite(graph_resources.base_color)
                        .ReadWrite(graph_resources.normal)
                        .ReadWrite(graph_resources.metal_rough_ao)
                        .ReadWrite(graph_resources.depth);
                },
                [&]() {
                    return tessellated_surface_pass->Prepare(
                        raster_context, raster_config, camera
                    );
                },
                [&](CommandList& recording_cmd_list,
                    const TessellatedSurfacePass::RecordParameters& parameters) {
                    tessellated_surface_pass->Record(recording_cmd_list, parameters);
                }
            );
            schedule_prepared(
                "HiZBuild",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.depth)
                        .Write(graph_resources.hiz_current);
                },
                [&]() { return hiz_build_pass->Prepare(raster_context); },
                [&](CommandList& recording_cmd_list,
                    const HiZBuildPass::RecordParameters& parameters) {
                    hiz_build_pass->Record(recording_cmd_list, parameters);
                }
            );
            schedule_prepared_after(
                "DirectionalShadowMask",
                shadow_depth_pass_handle.setup,
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.normal)
                        .Read(graph_resources.depth)
                        .Read(graph_resources.lighting_data)
                        .Read(graph_resources.shadow_maps)
                        .Write(graph_resources.shadow_mask);
                },
                [&]() { return directional_shadow_mask_pass->Prepare(raster_context); },
                [&](CommandList& recording_cmd_list,
                    const DirectionalShadowMaskPass::RecordParameters& parameters) {
                    directional_shadow_mask_pass->Record(recording_cmd_list, parameters);
                }
            );
            schedule_prepared(
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
                        .Write(graph_resources.lighting_output);
                    if (graph_resources.scene_lights.IsValid()) {
                        builder.Read(graph_resources.scene_lights);
                    }
                },
                [&]() { return lighting_pass->Prepare(raster_context, raster_config); },
                [&](CommandList& recording_cmd_list,
                    const LightingPass::RecordParameters& parameters) {
                    lighting_pass->Record(recording_cmd_list, parameters);
                }
            );
            schedule_prepared(
                "Skybox",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.depth)
                        .Read(graph_resources.cubemap)
                        .ReadWrite(graph_resources.lighting_output);
                },
                [&]() { return skybox_pass->Prepare(raster_context, raster_config, camera); },
                [&](CommandList& recording_cmd_list,
                    const SkyboxPass::RecordParameters& parameters) {
                    skybox_pass->Record(recording_cmd_list, parameters);
                }
            );

            if (draw_scene_gizmos && scene_gizmos.show_probe_gi) {
                schedule_prepared_after(
                    "ProbeGizmo",
                    probe_update_pass_handle.setup,
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
                        return probe_gizmo_pass->Prepare(
                            raster_context, probe_gizmo_config, camera
                        );
                    },
                    [&](CommandList& recording_cmd_list,
                        const ProbeGizmoPass::RecordParameters& parameters) {
                        probe_gizmo_pass->Record(recording_cmd_list, parameters);
                    }
                );
            }

            ao_result = ao_pass->DescribeNextOutput(raster_context, raster_config);
            raster_commit_ao_index = ao_result.ao_only_idx;
            schedule_prepared(
                "AmbientOcclusion",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.normal)
                        .Read(graph_resources.depth)
                        .Read(graph_resources.lighting_output)
                        .Write(graph_resources.ao_only)
                        .Write(graph_resources.camera_motion_vector)
                        .Write(graph_resources.motion_vector_data);
                },
                [&]() { return ao_pass->Prepare(raster_context, raster_config, camera, time); },
                [&](CommandList& recording_cmd_list,
                    const AoPass::RecordParameters& parameters) {
                    ao_pass->Record(recording_cmd_list, parameters);
                }
            );
            schedule_prepared(
                "RtaoDenoise",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.normal)
                        .Read(graph_resources.depth)
                        .Read(graph_resources.camera_motion_vector)
                        .Read(graph_resources.ao_history_read)
                        .ReadWrite(graph_resources.ao_only)
                        .Write(graph_resources.ao_history_write);
                },
                [&]() {
                    return rtao_denoiser_pass->Prepare(
                        raster_context, raster_config, ao_result.ao_only_idx
                    );
                },
                [&](CommandList& recording_cmd_list,
                    const RtaoDenoiserPass::RecordParameters& parameters) {
                    rtao_denoiser_pass->Record(recording_cmd_list, parameters);
                }
            );
            schedule_prepared(
                "AoComposite",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(graph_resources.ao_only)
                        .Read(graph_resources.lighting_output)
                        .Read(graph_resources.depth)
                        .Read(graph_resources.normal)
                        .Write(graph_resources.ao_output);
                },
                [&]() {
                    return ao_pass->PrepareComposite(
                        raster_context, raster_config, ao_result.ao_only
                    );
                },
                [&](CommandList& recording_cmd_list,
                    const AoPass::CompositeRecordParameters& parameters) {
                    ao_pass->RecordComposite(recording_cmd_list, parameters);
                }
            );
            processing_image = raster_context.textures.ao_output;
            processing_image_resource = graph_resources.ao_output;

#if WITH_CUDA
            if (raster_config.ai_is_cuda_enabled) {
                schedule_external(
                    "TensorRT",
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.Read(graph_resources.ao_only)
                            .Read(graph_resources.depth)
                            .Read(graph_resources.camera_motion_vector)
                            .ReadWrite(graph_resources.lighting_output)
                            .SideEffect();
                    },
                    [&]() {
                        processing_image = tensor_rt_pass->Process(
                            raster_context, raster_config, ao_result.ao_only_idx
                        );
                        processing_image_resource =
                            graph_resources.lighting_output;
                    }
                );
            }
#endif

            const TextureWithHandle bilateral_input = processing_image;
            schedule_prepared(
                "BilateralDenoise",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(processing_image_resource)
                        .Write(graph_resources.denoiser_output);
                },
                [&, bilateral_input]() {
                    return bilateral_filter_denoiser_pass->Prepare(
                        raster_context, raster_config, bilateral_input
                    );
                },
                [&](CommandList& recording_cmd_list,
                    const BilateralFilterDenoiserPass::RecordParameters& parameters) {
                    bilateral_filter_denoiser_pass->Record(recording_cmd_list, parameters);
                }
            );
            if (raster_config.denoiser_mode != EDenoiserMode::NONE) {
                processing_image = raster_context.textures.denoiser_output;
                processing_image_resource = graph_resources.denoiser_output;
            }

            const TextureWithHandle ssr_input = processing_image;
            schedule_prepared(
                "ScreenSpaceReflection",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(processing_image_resource)
                        .Read(graph_resources.normal)
                        .Read(graph_resources.depth)
                        .Read(graph_resources.metal_rough_ao)
                        .Write(graph_resources.ssr_output);
                },
                [&, ssr_input]() {
                    return ssr_pass->Prepare(raster_context, raster_config, camera, ssr_input);
                },
                [&](CommandList& recording_cmd_list,
                    const SsrPass::RecordParameters& parameters) {
                    ssr_pass->Record(recording_cmd_list, parameters);
                }
            );
            if (raster_config.ssr_is_ssr_enabled != 0) {
                processing_image = raster_context.textures.ssr_output;
                processing_image_resource = graph_resources.ssr_output;
            }
            const TextureWithHandle aa_input = processing_image;
            schedule_prepared(
                "AntiAliasing",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(processing_image_resource)
                        .Read(graph_resources.depth)
                        .Write(graph_resources.aa_output);
                },
                [&, aa_input]() {
                    return aa_pass->Prepare(
                        raster_context,
                        raster_config,
                        camera,
                        aa_input,
                        smaa_t2x_phase
                    );
                },
                [&](CommandList& recording_cmd_list,
                    const AaPass::RecordParameters& parameters) {
                    aa_pass->Record(recording_cmd_list, parameters);
                }
            );
            processing_image = raster_context.textures.aa_output;
            processing_image_resource = graph_resources.aa_output;
            const TextureWithHandle bloom_input = processing_image;
            schedule_prepared(
                "Bloom",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.ReadWrite(processing_image_resource)
                        .Write(graph_resources.bloom_downsample_chain)
                        .Write(graph_resources.bloom_upsample_chain);
                },
                [&, bloom_input]() {
                    return bloom_pass->Prepare(raster_context, raster_config, bloom_input);
                },
                [&](CommandList& recording_cmd_list,
                    const BloomPass::RecordParameters& parameters) {
                    bloom_pass->Record(recording_cmd_list, parameters);
                }
            );
            const TextureWithHandle tonemapping_input = processing_image;
            schedule_prepared(
                "Tonemapping",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Read(processing_image_resource)
                        .ReadWrite(graph_resources.tonemapping_histogram)
                        .ReadWrite(graph_resources.tonemapping_exposure)
                        .Write(graph_resources.tonemapping_output);
                },
                [&, tonemapping_input]() {
                    return tonemapping_pass->Prepare(
                        raster_context, raster_config, tonemapping_input
                    );
                },
                [&](CommandList& recording_cmd_list,
                    const TonemappingPass::RecordParameters& parameters) {
                    tonemapping_pass->Record(recording_cmd_list, parameters);
                }
            );
            processing_image = raster_context.textures.tonemapping_output;
            processing_image_resource = graph_resources.tonemapping_output;

            if (draw_scene_gizmos && scene_gizmos.show_main_camera) {
                schedule_prepared(
                    "CameraGizmo",
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.Read(graph_resources.scene)
                            .ReadWrite(graph_resources.tonemapping_output)
                            .SideEffect();
                    },
                    [&]() { return camera_gizmo_pass->Prepare(raster_context, camera, main_camera); },
                    [&](CommandList& recording_cmd_list,
                        const CameraGizmoPass::RecordParameters& parameters) {
                        camera_gizmo_pass->Record(recording_cmd_list, parameters);
                    }
                );
            }
            if (draw_scene_gizmos && scene_gizmos.show_csm) {
                schedule_prepared_after(
                    "CsmGizmo",
                    shadow_depth_pass_handle.setup,
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.Read(graph_resources.shadow_maps)
                            .ReadWrite(graph_resources.tonemapping_output)
                            .SideEffect();
                    },
                    [&]() {
                        return csm_gizmo_pass->Prepare(
                            raster_context, raster_config, scene_gizmos, camera, main_camera
                        );
                    },
                    [&](CommandList& recording_cmd_list,
                        const CsmGizmoPass::RecordParameters& parameters) {
                        csm_gizmo_pass->Record(recording_cmd_list, parameters);
                    }
                );
            }

            const TextureWithHandle ui_combine_input = processing_image;
            schedule_prepared(
                "UiCombine",
                [&](RenderGraph::PassBuilder& builder) {
                    if (!frame_packet.ui_composition.enabled) {
                        builder.Read(processing_image_resource)
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
                [&, ui_combine_input]() {
                    if (frame_packet.ui_composition.enabled) {
                        const auto& ui_frame = frame_packet.ui_composition;
                        return ui_combine_pass->Prepare(
                            ui_frame.separate_window,
                            ui_frame.output_resolution,
                            ui_frame.scene_color_position,
                            ui_frame.scene_color_resolution,
                            window_framebuffer_view,
                            selected_framebuffer_view,
                            raster_context.textures.output.tex
                        );
                    }
                    // Without editor UI composition, use the copy path directly instead of sampling an
                    // uninitialized UI buffer through the combine shader.
                    return ui_combine_pass->Prepare(
                        true,
                        presentation_resolution,
                        float2(0.f, 0.f),
                        float2(
                            static_cast<float>(presentation_resolution.x),
                            static_cast<float>(presentation_resolution.y)
                        ),
                        TextureView(raster_context.textures.output.tex),
                        ui_combine_input.tex,
                        raster_context.textures.output.tex
                    );
                },
                [&](CommandList& recording_cmd_list,
                    const UiCombinePass::RecordParameters& parameters) {
                    ui_combine_pass->Record(recording_cmd_list, parameters);
                }
            );
        };

        linear_source_order = next_profile_source_order;
        tail_source_order   = next_profile_source_order;
        auto execute_linear = [&]() {
            // A visual pipeline marker may not cross ExternalControl's explicit
            // Submit boundary. Modern capture instead uses closed per-pass
            // timestamp roots and rebinds the next recording generation.
            std::optional<ScopedGpuMarker> pipeline_marker{};
            if (!gpu_profile_frame.Valid()) {
                pipeline_marker.emplace(
                    cmd_list, "Raster Linear Pipeline", GpuMarkerPalette::RenderGraph()
                );
            }
            auto linear_schedule =
                [&](std::string_view                name,
                    auto&&,
                    RenderGraph::PassExecutionClass execution_class,
                    auto&&                          execute) {
                if (execution_class == RenderGraph::PassExecutionClass::CpuPrepare ||
                    execution_class == RenderGraph::PassExecutionClass::ExternalControl) {
                    std::forward<decltype(execute)>(execute)(cmd_list);
                    if (execution_class == RenderGraph::PassExecutionClass::ExternalControl &&
                        gpu_profile_frame.Valid()) {
                        ++linear_source_order;
                    }
                    return;
                }
                static_cast<void>(ensure_profile_source(
                    cmd_list, graphics_profile_binding, linear_source_order
                ));
                const std::string marker_name = std::format("Pass: {}", name);
                ScopedGpuMarker pass_marker(
                    cmd_list,
                    marker_name,
                    GpuMarkerPalette::Pass(),
                    cmd_list.HasGpuScopeRecorder() ?
                        EGpuMarkerMode::Timestamp :
                        EGpuMarkerMode::Label
                );
                std::forward<decltype(execute)>(execute)(cmd_list);
            };
            auto linear_prepared_schedule =
                [&](std::string_view                name,
                    auto&&,
                    RenderGraph::PassExecutionClass execution_class,
                    auto&&                          prepare,
                    auto&&                          record,
                    std::span<const RenderGraph::SetupPassHandle>,
                    RenderGraph::PrepareSafety) {
                    auto parameters = std::forward<decltype(prepare)>(prepare)();
                    linear_schedule(
                        name,
                        nullptr,
                        execution_class,
                        [parameters = std::move(parameters),
                         record = std::forward<decltype(record)>(record)](
                            CommandList& recording_cmd_list
                        ) mutable { record(recording_cmd_list, parameters); }
                    );
                    return RenderGraph::PreparedPassHandle{};
                };
            define_raster_passes(linear_schedule, linear_prepared_schedule);
            raster_pipeline_recorded = true;
            // ExternalControl may seal the current CommandList generation.
            // The tail either reuses the post-boundary generation or binds
            // this next unique order when no later GPU pass was recorded.
            tail_source_order = linear_source_order;
        };

        if (render_graph_enabled && !render_graph_fallback_latched) {
            // Profile sources and RDG barriers must use the initialized RHI's
            // physical queue identity rather than the test-only single-queue
            // default (native/family zero).
            RenderGraph graph(
                "RasterFrame", RenderGraph::QueueTopology::FromRHI()
            );
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
            const uint next_ao_only_index = ao_pass->NextAoOnlyIndex();
            const bool ao_half_resolution = raster_config.ao_half_resolution;
            const auto& ao_only_texture = next_ao_only_index == 0u ?
                (ao_half_resolution ? raster_context.textures.ao_output_ambient_only_half :
                                      raster_context.textures.ao_output_ambient_only) :
                (ao_half_resolution ? raster_context.textures.ao_output_ambient_only_1_half :
                                      raster_context.textures.ao_output_ambient_only_1);
            const auto& camera_motion_vector_texture = ao_half_resolution ?
                raster_context.textures.camera_motion_vector_half :
                raster_context.textures.camera_motion_vector;
            const auto& ao_history_read_texture = next_ao_only_index == 0u ?
                (ao_half_resolution ? raster_context.textures.ao_denoiser_accumulate_1_half :
                                      raster_context.textures.ao_denoiser_accumulate_1) :
                (ao_half_resolution ? raster_context.textures.ao_denoiser_accumulate_half :
                                      raster_context.textures.ao_denoiser_accumulate);
            const auto& ao_history_write_texture = next_ao_only_index == 0u ?
                (ao_half_resolution ? raster_context.textures.ao_denoiser_accumulate_half :
                                      raster_context.textures.ao_denoiser_accumulate) :
                (ao_half_resolution ? raster_context.textures.ao_denoiser_accumulate_1_half :
                                      raster_context.textures.ao_denoiser_accumulate_1);
            graph_resources.ao_only = import_texture("ao_only", ao_only_texture.tex);
            graph_resources.camera_motion_vector = import_texture(
                "camera_motion_vector", camera_motion_vector_texture.tex
            );
            graph_resources.ao_history_read = import_texture(
                "ao_history_read", ao_history_read_texture.tex
            );
            graph_resources.ao_history_write = import_texture(
                "ao_history_write", ao_history_write_texture.tex
            );
            const auto& motion_vector_data = ao_pass->GetMotionVectorDataBuffer();
            graph_resources.motion_vector_data = graph.ImportBuffer(
                "motion_vector_data",
                motion_vector_data,
                RenderGraph::BufferDesc{.byte_size = motion_vector_data->GetByteSize()}
            );
            graph_resources.ao_output =
                import_texture("ao_output", raster_context.textures.ao_output.tex);
            graph_resources.denoiser_output =
                import_texture("denoiser_output", raster_context.textures.denoiser_output.tex);
            graph_resources.ssr_output =
                import_texture("ssr_output", raster_context.textures.ssr_output.tex);
            graph_resources.aa_output =
                import_texture("aa_output", raster_context.textures.aa_output.tex);
            graph_resources.bloom_downsample_chain = import_texture(
                "bloom_downsample_chain", raster_context.textures.bloom_downsample_chain.tex
            );
            graph_resources.bloom_upsample_chain = import_texture(
                "bloom_upsample_chain", raster_context.textures.bloom_upsample_chain.tex
            );
            const auto& tonemapping_histogram = tonemapping_pass->GetHistogramBuffer();
            graph_resources.tonemapping_histogram = graph.ImportBuffer(
                "tonemapping_histogram",
                tonemapping_histogram,
                RenderGraph::BufferDesc{.byte_size = tonemapping_histogram->GetByteSize()}
            );
            const auto& tonemapping_exposure = tonemapping_pass->GetExposureBuffer();
            graph_resources.tonemapping_exposure = graph.ImportBuffer(
                "tonemapping_exposure",
                tonemapping_exposure,
                RenderGraph::BufferDesc{.byte_size = tonemapping_exposure->GetByteSize()}
            );
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

            if (render_graph_fallback_latched) {
                execute_linear();
            } else {
                const auto shadow_depth_handle = shadow_depth_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    camera,
                    {.scene = graph_resources.scene, .shadow_maps = graph_resources.shadow_maps}
                );
                const auto probe_update_handle = probe_update_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    camera,
                    time,
                    {.scene = graph_resources.scene, .probe_volume = graph_resources.probe_volume}
                );

                const StaticArray<RenderGraph::SetupPassHandle, 2> lighting_dependencies{
                    shadow_depth_handle.setup,
                    probe_update_handle.setup
                };
                LightingUploadPass{}.AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    camera,
                    {
                        .shadow_maps = graph_resources.shadow_maps,
                        .probe_volume = graph_resources.probe_volume,
                        .lighting_data = graph_resources.lighting_data
                    },
                    lighting_dependencies
                );

                const StaticArray<RenderGraph::SetupPassHandle, 1> shadow_dependency{
                    shadow_depth_handle.setup
                };
                geometry_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    camera,
                    {
                        .scene = graph_resources.scene,
                        .hiz_previous = graph_resources.hiz_previous,
                        .base_color = graph_resources.base_color,
                        .normal = graph_resources.normal,
                        .metal_rough_ao = graph_resources.metal_rough_ao,
                        .depth = graph_resources.depth
                    },
                    shadow_dependency
                );
                tessellated_surface_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    camera,
                    {
                        .base_color = graph_resources.base_color,
                        .normal = graph_resources.normal,
                        .metal_rough_ao = graph_resources.metal_rough_ao,
                        .depth = graph_resources.depth
                    }
                );
                hiz_build_pass->AddToGraph(
                    graph,
                    raster_context,
                    {.depth = graph_resources.depth, .hiz_current = graph_resources.hiz_current}
                );
                directional_shadow_mask_pass->AddToGraph(
                    graph,
                    raster_context,
                    {
                        .normal = graph_resources.normal,
                        .depth = graph_resources.depth,
                        .lighting_data = graph_resources.lighting_data,
                        .shadow_maps = graph_resources.shadow_maps,
                        .shadow_mask = graph_resources.shadow_mask
                    },
                    shadow_dependency
                );
                lighting_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    {
                        .base_color = graph_resources.base_color,
                        .normal = graph_resources.normal,
                        .metal_rough_ao = graph_resources.metal_rough_ao,
                        .depth = graph_resources.depth,
                        .shadow_mask = graph_resources.shadow_mask,
                        .lighting_data = graph_resources.lighting_data,
                        .cubemap = graph_resources.cubemap,
                        .probe_volume = graph_resources.probe_volume,
                        .lighting_output = graph_resources.lighting_output,
                        .scene_lights = graph_resources.scene_lights
                    }
                );
                skybox_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    camera,
                    {
                        .depth = graph_resources.depth,
                        .cubemap = graph_resources.cubemap,
                        .lighting_output = graph_resources.lighting_output
                    }
                );

                if (draw_scene_gizmos && scene_gizmos.show_probe_gi) {
                    RasterConfig probe_gizmo_config = raster_config;
                    probe_gizmo_config.probe_gi_gizmo_enabled =
                        scene_gizmos.show_probe_gi_probes;
                    probe_gizmo_config.probe_gi_volume_bounds_enabled =
                        scene_gizmos.show_probe_gi_volume_bounds;
                    if (scene_gizmos.show_probe_gi_adaptive_cells) {
                        probe_gizmo_config.probe_gi_debug_mode = 9;
                    }
                    const StaticArray<RenderGraph::SetupPassHandle, 1> probe_dependency{
                        probe_update_handle.setup
                    };
                    probe_gizmo_pass->AddToGraph(
                        graph,
                        raster_context,
                        std::move(probe_gizmo_config),
                        camera,
                        {
                            .probe_volume = graph_resources.probe_volume,
                            .lighting_output = graph_resources.lighting_output
                        },
                        probe_dependency
                    );
                }

                ao_result = ao_pass->DescribeNextOutput(raster_context, raster_config);
                raster_commit_ao_index = ao_result.ao_only_idx;
                ao_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    camera,
                    time,
                    {
                        .normal = graph_resources.normal,
                        .depth = graph_resources.depth,
                        .lighting_output = graph_resources.lighting_output,
                        .ao_only = graph_resources.ao_only,
                        .camera_motion_vector = graph_resources.camera_motion_vector,
                        .motion_vector_data = graph_resources.motion_vector_data
                    }
                );
                rtao_denoiser_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    ao_result.ao_only_idx,
                    {
                        .normal = graph_resources.normal,
                        .depth = graph_resources.depth,
                        .camera_motion_vector = graph_resources.camera_motion_vector,
                        .ao_only = graph_resources.ao_only,
                        .history_read = graph_resources.ao_history_read,
                        .history_write = graph_resources.ao_history_write
                    }
                );
                ao_pass->AddCompositeToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    ao_result.ao_only,
                    {
                        .ao_only = graph_resources.ao_only,
                        .lighting_output = graph_resources.lighting_output,
                        .depth = graph_resources.depth,
                        .normal = graph_resources.normal,
                        .ao_output = graph_resources.ao_output
                    }
                );
                processing_image = raster_context.textures.ao_output;
                processing_image_resource = graph_resources.ao_output;

#if WITH_CUDA
                if (raster_config.ai_is_cuda_enabled) {
                    graph.AddUnsafePass(
                        "TensorRT",
                        [&](RenderGraph::PassBuilder& builder) {
                            builder.Read(graph_resources.ao_only)
                                .Read(graph_resources.depth)
                                .Read(graph_resources.camera_motion_vector)
                                .ReadWrite(graph_resources.lighting_output)
                                .SideEffect()
                                .ExternalControl();
                        },
                        [&]() {
                            processing_image = tensor_rt_pass->Process(
                                raster_context, raster_config, ao_result.ao_only_idx
                            );
                            processing_image_resource =
                                graph_resources.lighting_output;
                        }
                    );
                }
#endif

                bilateral_filter_denoiser_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    processing_image,
                    {
                        .input = processing_image_resource,
                        .denoiser_output = graph_resources.denoiser_output
                    }
                );
                if (raster_config.denoiser_mode != EDenoiserMode::NONE) {
                    processing_image = raster_context.textures.denoiser_output;
                    processing_image_resource = graph_resources.denoiser_output;
                }
                ssr_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    camera,
                    processing_image,
                    {
                        .input = processing_image_resource,
                        .normal = graph_resources.normal,
                        .depth = graph_resources.depth,
                        .metal_rough_ao = graph_resources.metal_rough_ao,
                        .ssr_output = graph_resources.ssr_output
                    }
                );
                if (raster_config.ssr_is_ssr_enabled != 0) {
                    processing_image = raster_context.textures.ssr_output;
                    processing_image_resource = graph_resources.ssr_output;
                }
                aa_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    camera,
                    processing_image,
                    smaa_t2x_phase,
                    {
                        .input = processing_image_resource,
                        .depth = graph_resources.depth,
                        .aa_output = graph_resources.aa_output
                    }
                );
                processing_image = raster_context.textures.aa_output;
                processing_image_resource = graph_resources.aa_output;
                bloom_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    processing_image,
                    {
                        .input = processing_image_resource,
                        .downsample_chain = graph_resources.bloom_downsample_chain,
                        .upsample_chain = graph_resources.bloom_upsample_chain
                    }
                );
                tonemapping_pass->AddToGraph(
                    graph,
                    raster_context,
                    raster_config,
                    processing_image,
                    {
                        .input = processing_image_resource,
                        .histogram = graph_resources.tonemapping_histogram,
                        .exposure = graph_resources.tonemapping_exposure,
                        .tonemapping_output = graph_resources.tonemapping_output
                    }
                );
                processing_image = raster_context.textures.tonemapping_output;
                processing_image_resource = graph_resources.tonemapping_output;

                if (draw_scene_gizmos && scene_gizmos.show_main_camera) {
                    camera_gizmo_pass->AddToGraph(
                        graph,
                        raster_context,
                        camera,
                        main_camera,
                        {
                            .scene = graph_resources.scene,
                            .tonemapping_output = graph_resources.tonemapping_output
                        }
                    );
                }
                if (draw_scene_gizmos && scene_gizmos.show_csm) {
                    csm_gizmo_pass->AddToGraph(
                        graph,
                        raster_context,
                        raster_config,
                        scene_gizmos,
                        camera,
                        main_camera,
                        {
                            .shadow_maps = graph_resources.shadow_maps,
                            .tonemapping_output = graph_resources.tonemapping_output
                        },
                        shadow_dependency
                    );
                }

                const auto& ui_frame = frame_packet.ui_composition;
                ui_combine_pass->AddToGraph(
                    graph,
                    ui_frame.enabled,
                    ui_writes_external_window,
                    ui_frame.separate_window,
                    ui_frame.enabled ? ui_frame.output_resolution : presentation_resolution,
                    ui_frame.scene_color_position,
                    ui_frame.scene_color_resolution,
                    window_framebuffer_view,
                    selected_framebuffer_view,
                    TextureView(raster_context.textures.output.tex),
                    TextureView(processing_image.tex),
                    {
                        .processing_input = processing_image_resource,
                        .selected_framebuffer = graph_resources.selected_framebuffer,
                        .window_framebuffer = graph_resources.window_framebuffer,
                        .output = graph_resources.output
                    }
                );
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
                    // Passes record independent frontend command streams (and
                    // may do so in parallel). After the producer join, those
                    // streams are concatenated in compiled order into this
                    // backend-tracked frame list, retaining one native submit
                    // and one continuous cross-pass state-tracker lifetime.
                    if (renderer_marker) {
                        renderer_marker->Close();
                    }
                    const uint64 graph_source_order_base =
                        next_profile_source_order;
                    tail_source_order = graph_source_order_base;
                    static_cast<void>(ensure_profile_source(
                        cmd_list,
                        graphics_profile_binding,
                        graph_source_order_base
                    ));
                    RenderGraph::GpuProfilingOptions gpu_profiling{};
                    if (gpu_profile_frame.Valid()) {
                        gpu_profiling.try_bind_source =
                            [&](const RenderGraph::ExecutedPassInfo&,
                                CommandList&    recording_cmd_list,
                                RHIQueueBinding queue_binding,
                                uint64          source_order) noexcept {
                                return try_bind_profile_source(
                                    recording_cmd_list,
                                    queue_binding,
                                    source_order
                                );
                            };
                        // The caller-owned frame stream is the first logical
                        // profiling source. Per-pass frontend sources follow
                        // it while still sharing the final native submit.
                        gpu_profiling.source_order_base =
                            graph_source_order_base + 1;
                    }
                    const bool graph_recorded = graph.RecordAndMergeFrontendCommands(
                        cmd_list,
                        parallel_recording_enabled,
                        gpu_profiling
                    );
                    raster_pipeline_recorded = graph_recorded;
                    if (!graph_recorded) {
                        if (gpu_profile_frame.Valid() &&
                            (cmd_list.HasGpuScopeRecorder() ||
                             cmd_list
                                 .IsLegacyGpuProfilingSuppressedForGeneration())) {
                            // Reject any already-merged prefix so a failed
                            // graph can never leak partial commands into the
                            // unrelated frame tail.
                            CmdSubmit rejected_graph_generation =
                                cmd_list.Submit();
                            static_cast<void>(rejected_graph_generation);
                        }
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
    UiDrawFrameSlotClaim ui_slot_claim(frame_packet.ui_draw_frame);
    const bool ui_recording_claimed =
        ui_slot_claim.IsReadyForRecording();
    const auto record_frame_tail = [&]() {
        if (ui_recording_claimed) {
            RenderUiDrawFrame(
                cmd_list,
                default_output_texture->GetView(),
                frame_packet.ui_draw_frame,
                ui_execution_thread
            );
        } else {
            ui_slot_claim.Reject();
            skip_present = true;
            LOG_CRITICAL(
                "[Threading][UI] Raster copied-frame upload slots could not be "
                "claimed; preserving the slots and skipping this frame's presents."
            );
        }

        raster_context.probe_volume.TrackFrameSubmission(cmd_list, time);
        if (!skip_present && ui_recording_claimed &&
            !UiFrameGraphPass::RecordPresentationBoundary(
                cmd_list,
                frame_packet.ui_composition,
                frame_packet.ui_draw_frame,
                default_output_texture
            )) {
            skip_present = true;
            LOG_CRITICAL(
                "[Presentation] Raster UI frame tail did not satisfy the "
                "presentation-source resource contract; skipping this frame's "
                "main and platform-window presents."
            );
        }
    };
    const bool tail_profile_bound = ensure_profile_source(
        cmd_list, graphics_profile_binding, tail_source_order
    );
    if (tail_profile_bound) {
        ScopedGpuMarker tail_marker(
            cmd_list,
            "Raster Frame Tail",
            GpuMarkerPalette::Ui(),
            EGpuMarkerMode::Timestamp
        );
        record_frame_tail();
    } else {
        record_frame_tail();
    }
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
        const Extent2D presentation_extent =
            main_presentation_surface.GetExtent();
        if (frame_packet.window.transition != EWindowFrameTransition::Stable &&
            frame_packet.window.transition != EWindowFrameTransition::LogicalResized) {
            LOG_INFO(
                "[Threading][Resize] Main present source={}x{}, swapchain={}x{}, UI platform viewports={}.",
                output_view.extent.x,
                output_view.extent.y,
                presentation_extent.x,
                presentation_extent.y,
                frame_packet.ui_draw_frame.platform_viewports.size()
            );
        }
        PresentReceiptRef continuous_present_receipt{};
        present_request = main_presentation_surface.CreatePresentRequest(
            frame_packet.window,
            output_view,
            &continuous_present_receipt
        );
        if (!present_request.has_value()) {
            main_present_receipt = {};
            LOG_WARNING(
                "Skipping stale main-window present: source={}x{}, swapchain={}x{}.",
                output_view.extent.x,
                output_view.extent.y,
                presentation_extent.x,
                presentation_extent.y
            );
        } else {
            main_present_receipt = SelectMainPresentReceipt(
                frame_packet.scene_updates.scene_ready,
                continuous_present_receipt
            );
        }
    }

    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        std::move(frame_submit),
        ERHIExecSubmitFlags::FlushGPU,
        present_request ? &*present_request : nullptr
    );
    if (render_profile_capture != nullptr && gpu_profile_frame.Valid()) {
        static_cast<void>(render_profile_capture->Seal(gpu_profile_frame));
        static_cast<void>(render_profile_capture->DrainReadyFrames());
    }

    const bool frame_native_accepted = timeline->WaitSubmitted(time);
    if (frame_native_accepted && raster_pipeline_recorded) {
        raster_context.CommitHiZHistory(raster_commit_view_proj);
        ao_pass->CommitFrame(raster_commit_view_proj, raster_commit_ao_index);
        rtao_denoiser_pass->CommitFrame(frame_packet.raster_config);
        aa_pass->CommitFrame(
            frame_packet.raster_config.aa_mode,
            raster_commit_aa_phase,
            raster_commit_view_proj
        );
    }
    bool       ui_source_accepted    = false;
    if (frame_native_accepted && ui_recording_claimed) {
        ui_source_accepted = ui_slot_claim.CommitAccepted();
    } else {
        ui_slot_claim.Reject();
    }
    if (!ui_source_accepted) {
        LOG_CRITICAL(
            "[Threading][UI] Raster frame submission rejected the copied UI "
            "source; preserving its upload-ring slots."
        );
    }

    if (!skip_present && frame_native_accepted && ui_source_accepted) {
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
