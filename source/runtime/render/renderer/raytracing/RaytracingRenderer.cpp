#include "RaytracingRenderer.h"

// Runtime
#include "PixelFormat.h"
#include "config/ConfigManager.h"
#include "misc/BoundingBox.h"
#include "misc/Timer.h"
#include "profile/ProfileScope.h"
#include "profile/RenderProfileCapture.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIExecutor.h"
#include "rhi/RHIResource.h"
#include "rhi/plugin/NrdPlugin.h"
#include "renderer/common/UiFrameGraphPass.h"
#include "scene/GpuScene.h"
#include "shader/ShaderResourceManager.h"
#include "window/WindowContext.h"

// Renderer
#include "AntiAliasPass.h"
#include "CompositionPass.h"
#include "Configs.h"
#include "GBufferPass.h"
#include "LightingPass.h"
#include "NrdDenoisePass.h"
#include "PreprocessLightPass.h"
#include "RaytracingExportSubmission.h"
#include "RaytracingFrameSetupPass.h"
#include "RTResource.h"
#include "ShaderUtils.h"
#include "ShowTexturePass.h"
#include "ToneMappingPass.h"
#include "VisualizePass.h"

// 3rd party
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace Moer::Render::Raytracing {

namespace {

ImportanceSamplingParams CreateImportanceSamplingParams(uint2 resolution) {
    ImportanceSamplingParams params{};
    params.render_size = resolution;
    return params;
}

bool EqualFloat3(const float3& lhs, const float3& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool EqualQuaternion(const Quaternion& lhs, const Quaternion& rhs) {
    return lhs.vec.x == rhs.vec.x && lhs.vec.y == rhs.vec.y && lhs.vec.z == rhs.vec.z &&
           lhs.vec.w == rhs.vec.w;
}

double FindGpuTime(const ProfileData& profile_data, std::string_view name) {
    for (const auto& entry : profile_data.gpu_entries) {
        if (entry.name == name) {
            return entry.time;
        }
    }
    return -1.0;
}

bool IsProfileLogEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("MOER_RT_PROFILE_LOG");
        return value && value[0] != '\0' && std::string_view(value) != "0";
    }();
    return enabled;
}

bool IsLegacyTlasUpdateEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("MOER_RT_TLAS_EVERY_FRAME");
        return value && value[0] != '\0' && std::string_view(value) != "0";
    }();
    return enabled;
}

bool IsLightGridForced() {
    static const bool enabled = []() {
        const char* value = std::getenv("MOER_RT_FORCE_LIGHT_GRID");
        return value && value[0] != '\0' && std::string_view(value) != "0";
    }();
    return enabled;
}

bool IsRenderGraphRecordingFailureInjected() {
    static const bool enabled = []() {
        const char* value = std::getenv("MOER_RT_RDG_RECORDING_FAIL");
        return value && value[0] != '\0' && std::string_view(value) != "0";
    }();
    return enabled;
}

std::string_view LocalLightSampleModeName(uint mode) {
    switch (mode) {
        case s_di_local_light_sample_mode_uniform:
            return "Uniform";
        case s_di_local_light_sample_mode_power_ris:
            return "PowerRIS";
        case s_di_local_light_sample_mode_grid:
            return "Grid";
        default:
            return "Unknown";
    }
}

void TickAndLogProfiling(const RaytracingFrameFeedback& feedback) {
    const ProfileData& profile_data = feedback.profiler_data;
    if (!IsProfileLogEnabled() || profile_data.gpu_entries.empty()) {
        return;
    }

    static LoopedTimer timer(1.0, true);
    if (!timer.Tick()) {
        return;
    }

    constexpr std::pair<std::string_view, std::string_view> entries[] = {
        {"GraphicsExec", "Graphics Exec"},
        {"PrepareLights", "PrepareLights"},
        {"GBuffer", "GBufferPass"},
        {"LightingTotal", "LightingPass"},
        {"DIPresampleLocal", "ReSTIR DI Presample Local"},
        {"DIPresampleEnv", "ReSTIR DI Presample Environment"},
        {"DIPresampleGrid", "ReSTIR DI Presample Grid"},
        {"DIInitial", "ReSTIR DI Initial"},
        {"DITemporal", "ReSTIR DI Temporal"},
        {"DISpatial", "ReSTIR DI Spatial"},
        {"DIShade", "ReSTIR DI Shade"},
        {"RendererTLAS", "RTScene RendererTLAS"},
        {"TLASBuild", "RTScene BuildTLAS"},
        {"TLASUpdate", "RTScene UpdateTLAS"},
        {"AntiAlias", "AntiAliasPass"},
        {"ToneMapping", "ToneMappingPass"},
    };

    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(3);
    stream << "[RaytracingProfile] GPU(ms)";
    double di_shader_sum = 0.0;
    for (const auto& [key, scope_name] : entries) {
        const double duration = FindGpuTime(profile_data, scope_name);
        if (duration >= 0.0) {
            stream << ' ' << key << '=' << duration;
            if (key.starts_with("DI")) {
                di_shader_sum += duration;
            }
        }
    }
    const double lighting_total = FindGpuTime(profile_data, "LightingPass");
    if (lighting_total >= 0.0) {
        stream << " DIShaderSum=" << di_shader_sum
               << " LightingUnattributed=" << lighting_total - di_shader_sum;
    }
    stream << " TLASPolicy=" << (IsLegacyTlasUpdateEnabled() ? "LegacyEveryFrame" : "RevisionGated")
           << " RendererTLASBuilds=" << feedback.renderer_tlas_build_count
           << " RendererTLASSkips=" << feedback.renderer_tlas_skip_count
           << " SceneTLASUpdates=" << feedback.scene_tlas_update_count
           << " RTRevision=" << feedback.rt_instance_revision
           << " CurrentTLASRevision=" << feedback.current_tlas_revision
           << " PreviousTLASRevision=" << feedback.previous_tlas_revision << " ConfiguredLocalLightSampling="
           << LocalLightSampleModeName(feedback.configured_local_light_sample_mode)
           << " EffectiveLocalLightSampling="
           << LocalLightSampleModeName(feedback.effective_local_light_sample_mode) << " AdaptiveFallback="
           << (feedback.adaptive_local_light_fallback_applied ? "Applied" : "NotApplied")
           << " LocalLights=" << feedback.local_light_count;
    LOG_INFO("{}", stream.str());
}

#if WITH_NRD
nrd::Denoiser ResolveNrdDenoiser(int denoiser_mode) {
    switch (denoiser_mode) {
        case s_denoiser_mode_reblur:
            return nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
        case s_denoiser_mode_relax:
            return nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
        default:
            return nrd::Denoiser::MAX_NUM;
    }
}
#endif

void RecordGpuSceneReadBoundary(
    CommandList&         cmd_list,
    const GpuScene::Res& resources
) {
    Array<ReadBuffer>    buffer_reads;
    Array<const Buffer*> seen_buffers;
    const auto append_buffer = [&](const BufferWithHandle& resource) {
        if (!resource.buf ||
            std::find(
                seen_buffers.begin(),
                seen_buffers.end(),
                resource.buf.Get()
            ) != seen_buffers.end()) {
            return;
        }
        seen_buffers.emplace_back(resource.buf.Get());
        buffer_reads.emplace_back(
            ReadBuffer{
                resource.buf->GetView(),
                EBufferState::SHADER_RESOURCE
            }
        );
    };
    for (const BufferWithHandle* resource : {
             &resources.light_buf,
             &resources.material_buf,
             &resources.primitive_buf,
             &resources.instance_buf,
             &resources.position_buf,
             &resources.packed_normal_buf,
             &resources.packed_tangent_buf,
             &resources.texcoord0_buf,
             &resources.index_buf,
             &resources.rt_instance_buf,
             &resources.rt_primitive_table_buf,
         }) {
        append_buffer(*resource);
    }
    if (!buffer_reads.empty()) {
        cmd_list.BufferBarriers(
            EQueueType::Graphics,
            EQueueType::Graphics,
            EPassType::Compute,
            std::move(buffer_reads),
            Array<WriteBuffer>{}
        );
    }

    Array<ReadTexture>    texture_reads;
    Array<const Texture*> seen_textures;
    for (const TextureWithHandle& resource : resources.texture_array) {
        if (!resource.tex ||
            std::find(
                seen_textures.begin(),
                seen_textures.end(),
                resource.tex.Get()
            ) != seen_textures.end()) {
            continue;
        }
        seen_textures.emplace_back(resource.tex.Get());
        const auto usage = resource.tex->GetUsage();
        const bool supports_uav =
            (usage & ETextureUsageFlags::UNORDERED_ACCESS) ==
            ETextureUsageFlags::UNORDERED_ACCESS;
        texture_reads.emplace_back(
            ReadTexture{
                resource.tex->GetView(0, resource.tex->GetNumMips()),
                supports_uav ? ETextureState::SHADER_RESOURCE :
                               ETextureState::SAMPLE
            }
        );
    }
    if (!texture_reads.empty()) {
        cmd_list.TextureBarriers(
            EQueueType::Graphics,
            EQueueType::Graphics,
            EPassType::Compute,
            std::move(texture_reads)
        );
    }
}

void ExecuteSceneUpdate(
    RenderScene&     render_scene,
    GpuSceneUpdate&& update,
    RenderDevice&    device,
    CommandQueue&    gfx_queue,
    const GpuScene::PendingCommandListSetupCallback& setup_command_lists
) {
    (void)gfx_queue;
    (void)device;
    auto scene_cmd_list = render_scene.ApplyUpdate(
        std::move(update), setup_command_lists
    );
    // GpuScene uploads may finish as transfer writes while untouched aliases
    // retain their previous read state. Publish one deterministic SRV/Sampled
    // boundary on the same graphics stream so the following managed RT graph
    // never has to guess per-resource backend state.
    RecordGpuSceneReadBoundary(
        scene_cmd_list.gfx_queue_cmd_list,
        render_scene.GetGpuSceneRes()
    );
    const bool modern_graphics_profiling =
        scene_cmd_list.gfx_queue_cmd_list.HasGpuScopeRecorder() ||
        scene_cmd_list.gfx_queue_cmd_list
            .IsLegacyGpuProfilingSuppressedForGeneration();
    Array<RHIBackendSubmissionBatchEntry> submissions{};
    submissions.emplace_back(EQueueType::Copy, scene_cmd_list.copy_queue_cmd_list.Submit());
    CmdSubmit graphics_submit = scene_cmd_list.gfx_queue_cmd_list.Submit();
    if (!modern_graphics_profiling) {
        graphics_submit.TickProfiling();
    }
    submissions.emplace_back(
        EQueueType::Graphics, std::move(graphics_submit)
    );
    RHIExecutor::Get().Submit(std::move(submissions), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
}

} // namespace

struct RaytracingRenderer::RuntimeState {
    static constexpr uint64 invalid_tlas_revision = std::numeric_limits<uint64>::max();

    bool b_new_env_map = false;

    TextureRef         env_map{};
    Array<TextureView> env_mips;
    TextureWithHandle  env_tex_with_hdl;

    RaytracingSceneRef rt_scene{};

    ShaderUtils               shader_utils;
    ImportanceSamplingParams  importance_sampling_params;
    ImportanceSamplingContext importance_sampling_context;

    bool first_load = true;

    UnorderedMap<std::string, TextureWithHandle> material_textures;

    TextureRef output;

    Timer timer;

    bool b_feedback_valid = false;
    bool b_export         = false;

    bool                         render_graph_enabled                   = false;
    bool                         render_graph_debug_dump                = false;
    bool                         render_graph_parallel_recording        = false;
    bool                         render_graph_fallback_latched          = false;
    bool                         render_graph_recording_failure_latched = false;
    uint8                        gbuffer_initialized_history_mask       = 0;
    bool                         normal_roughness_readable              = false;
    std::array<const Buffer*, 3> lighting_working_set{};
    uint                         lighting_reservoir_block_array_pitch = 0;
    uint8                        lighting_initialized_reservoir_mask  = 0;
    UnorderedSet<std::string>    logged_render_graph_dumps;

    uint64 rt_instance_revision      = 0;
    uint64 current_tlas_revision     = invalid_tlas_revision;
    uint64 previous_tlas_revision    = invalid_tlas_revision;
    uint64 renderer_tlas_build_count = 0;
    uint64 renderer_tlas_skip_count  = 0;
    uint64 scene_tlas_update_count   = 0;

    std::filesystem::path exported_file_path;

    UniquePtr<PrepareLightPass> prepare_light_pass;
    UniquePtr<RaytracingFrameSetupPass> frame_setup_pass;
    UniquePtr<GBufferPass>      g_buffer_pass;
    UniquePtr<LightingPass>     lighting_pass;
    UniquePtr<CompositionPass>  composition_pass;
    UniquePtr<VisualizePass>    visualize_pass;
    UniquePtr<ShowTexturePass>  show_texture_pass;
    UniquePtr<RTContext>        rt_ctx;
    UniquePtr<ToneMappingPass>  tone_mapping_pass;

    AntialiasPass::CreateInfo antialias_pass_info{};
    UniquePtr<AntialiasPass>  antialias_pass;

#if WITH_NRD
    uint64                        nrd_time       = 0ull;
    int                           nrd_mode       = -1;
    bool                          nrd_was_active = false;
    bool                          nrd_outputs_initialized = false;
    Ext::NRDPlugin*               nrd_plugin     = nullptr;
    SharedPtr<Ext::NRDInterface>  nrd_interface;
#endif

    VisualizeConfig visualize_config{};

    explicit RuntimeState(RaytracingRenderer& renderer, uint2 scene_extent) :
        shader_utils(renderer.manager),
        importance_sampling_params(CreateImportanceSamplingParams(scene_extent)),
        importance_sampling_context(importance_sampling_params),
        output(renderer.device.CreateTexture(
            Extent2D(renderer.resolution.x, renderer.resolution.y),
            renderer.swapchain->format,
            ETextureUsageFlags::COLOR_ATTACHMENT |
                ETextureUsageFlags::PRESENTATION_SOURCE |
                ETextureUsageFlags::TRANSFER_SRC |
                ETextureUsageFlags::TRANSFER_DST
        )),
        exported_file_path(ConfigManager::GetInstance().GetWorkspacePath() / "saved"),
        prepare_light_pass(MakeUnique<PrepareLightPass>(renderer.manager, renderer.bindless_array)),
        frame_setup_pass(MakeUnique<RaytracingFrameSetupPass>()),
        g_buffer_pass(MakeUnique<GBufferPass>(renderer.device, renderer.manager, renderer.bindless_array)),
        lighting_pass(MakeUnique<LightingPass>(renderer.manager, renderer.bindless_array)),
        composition_pass(
            MakeUnique<CompositionPass>(renderer.device, renderer.manager, renderer.bindless_array)
        ),
        visualize_pass(MakeUnique<VisualizePass>(renderer.device, renderer.manager)),
        show_texture_pass(MakeUnique<ShowTexturePass>(renderer.manager, renderer.bindless_array)),
        rt_ctx(MakeUnique<RTContext>(shader_utils, importance_sampling_context, renderer.bindless_array)) {
        if (!std::filesystem::exists(exported_file_path)) {
            std::filesystem::create_directory(exported_file_path);
        }

        rt_ctx->SetResolution(scene_extent);
        antialias_pass_info = AntialiasPass::CreateInfo{
            .motion              = rt_ctx->frame_rt.motion,
            .feedback_color_ping = rt_ctx->frame_rt.feedback_color_ping,
            .feedback_color_pong = rt_ctx->frame_rt.feedback_color_pong
        };
        antialias_pass = MakeUnique<AntialiasPass>(renderer.device, renderer.manager, antialias_pass_info);

#if WITH_NRD
        nrd_plugin    = renderer.device.LoadPlugin<Ext::NRDPlugin>();
        nrd_interface = nrd_plugin->CreateInterface(
            renderer.max_frame_in_flight, scene_extent.x, scene_extent.y
        );
#endif

        visualize_config.b_split        = false;
        visualize_config.split_ratio    = 0.5f;
        visualize_config.visualize_mode = EFC_DI;

        timer.Start();
    }
};

RaytracingRenderer::RaytracingRenderer(
    uint2&                        resolution,
    const SharedPtr<EditorConfig> config,
    RuntimeAssets&                runtime_assets,
    RenderProfileCapture*         render_profile_capture
) :
    Renderer(resolution, config, render_profile_capture),
    runtime_assets(runtime_assets),
    scene_render_extent_tracker(resolution),
    runtime_state(MakeUnique<RuntimeState>(*this, scene_render_extent_tracker.GetActiveExtent())) {
    const auto& graph_config            = ConfigManager::GetInstance().GetConfig().engine.render.raytracing;
    runtime_state->render_graph_enabled = graph_config.render_graph;
    runtime_state->render_graph_debug_dump         = graph_config.render_graph_debug_dump;
    runtime_state->render_graph_parallel_recording = graph_config.render_graph_parallel_recording;
    LOG_INFO(
        "[RenderGraph] Raytracing execution mode: {}, primary graph recording: {}",
        runtime_state->render_graph_enabled ? "graph-pilot" : "linear",
        runtime_state->render_graph_parallel_recording ? "parallel-eligible" : "serial"
    );
}

RaytracingRenderer::~RaytracingRenderer() {
    const char* thread_name = IsCurrentlyRenderThread() ? "Render" : "Game";
    LOG_INFO("[Threading] RaytracingRenderer destruction started on {} Thread.", thread_name);

    if (runtime_state && runtime_state->rt_ctx) {
        for (const uint buffer : runtime_state->rt_ctx->GetAllocatedBdlsBuf()) {
            bindless_array->UnbindBuffer(buffer);
        }
        for (const uint texture : runtime_state->rt_ctx->GetAllocatedBdlsTex()) {
            bindless_array->UnbindTexture(texture);
        }
    }

    ReleaseResources();
    LOG_INFO("[Threading] RaytracingRenderer destruction finished on {} Thread.", thread_name);
}

void RaytracingRenderer::Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    assert(IsCurrentlyGameThread());
    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        if (!RunSingle(editor_config, hooks)) {
            break;
        }
    }
    Shutdown(hooks);
}

RaytracingFramePacket
RaytracingRenderer::PrepareFrame(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    assert(IsCurrentlyGameThread());
    assert(editor_config);
    MOER_PROFILE_SCOPE("Raytracing.PrepareFrame");

    const bool            profile_logging = IsFramePrepareProfilingEnabled();
    const auto            prepare_started = BeginFramePrepareProfile();
    FramePrepareProfile   prepare_profile{};
    FramePrepareWorkload  prepare_workload{};
    RaytracingFramePacket frame_packet{};
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

    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.config_snapshot_ms);
        frame_packet.config               = editor_config->raytracing_config;
        frame_packet.runtime_assets_ready = runtime_assets.IsReady();
        frame_packet.debug_input          = debug_ui_frame_input;
    }

    CameraFrameInput camera_input{};
    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.camera_and_test_ms);
        camera_input = CameraFrameInput::Capture(*editor_config);

        const bool submit_export_request =
            frame_packet.config.export_cfg.b_export && !export_request_in_flight;
        frame_packet.config.export_cfg.b_export = submit_export_request;
        export_request_in_flight                = export_request_in_flight || submit_export_request;
    }

    if (scene.IsReady() && frame_packet.runtime_assets_ready) {
        {
            ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.camera_and_test_ms);
            const auto                     light_entity = scene.GetMainDirectionalLightEntity();
            if (light_entity != entt::null && scene.r().valid(light_entity) &&
                scene.r().all_of<ecs::CLightDirectional, ecs::CNode>(light_entity)) {
                const float3     desired_color(0.9f, 0.65f, 0.4f);
                const Quaternion desired_rotation(float3(0.f, 0.f, -1.f), -frame_packet.config.sun_direction);
                // Compare the authoritative scene components instead of caching the last config.
                // External edits are still corrected on the next frame, while stable values do not
                // manufacture dirty scene work every frame.
                const auto& light = scene.r().get<ecs::CLightDirectional>(light_entity);
                if (!EqualFloat3(light.color, desired_color) ||
                    light.intensity != frame_packet.config.exposure) {
                    scene.Patch<ecs::CLightDirectional>(light_entity, [&](auto& patched_light) {
                        patched_light.color     = desired_color;
                        patched_light.intensity = frame_packet.config.exposure;
                    });
                }

                const auto& node = scene.r().get<ecs::CNode>(light_entity);
                if (!EqualQuaternion(node.rotation, desired_rotation)) {
                    scene.Patch<ecs::CNode>(light_entity, [&](auto& patched_node) {
                        patched_node.rotation = desired_rotation;
                    });
                }
            }
        }

        {
            ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.scene_update_ms);
            frame_packet.scene_updates = scene.PrepareUpdateBatch(false, capture_scene_geometry_snapshot);
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
            ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.scene_snapshot_ms);
            frame_packet.scene_snapshot = CaptureRaytracingSceneFrameSnapshot(scene);
            if (profile_logging) {
                prepare_workload.scene_snapshot_build_frames = 1u;
            }
        }
        {
            ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.camera_and_test_ms);
            if (frame_packet.scene_updates.scene_ready) {
                Camera camera = frame_packet.scene_updates.main_camera;
                camera.Tick(camera_input);
                frame_packet.scene_updates.main_camera = camera;
                scene.GetMainCamera().camera           = camera;
            }
            if (frame_packet.scene_updates.geometry) {
                capture_scene_geometry_snapshot = false;
            }
            if (frame_packet.scene_updates.scene_ready) {
                EnsureDebugUiRegistered(hooks);
            }
        }
    }

    {
        ScopedFramePrepareProfileTimer timer(profile_logging, prepare_profile.ui_composition_ms);
        if (hooks.on_capture_ui_composition) {
            frame_packet.ui_composition = hooks.on_capture_ui_composition();
        }
        frame_packet.scene_render_extent = CaptureSceneRenderExtentRequest(
            frame_packet.ui_composition.enabled,
            frame_packet.ui_composition.scene_color_resolution,
            frame_packet.window.resolution
        );
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
            "[Threading] RaytracingFramePacket boundary active on Game Thread; resolution={}x{}, "
            "UI main vertices={}, platform viewports={}.",
            frame_packet.window.resolution.x,
            frame_packet.window.resolution.y,
            frame_packet.ui_draw_frame.main_viewport.vertices.size(),
            frame_packet.ui_draw_frame.platform_viewports.size()
        );
    }

    RecordFramePrepareProfile("Raytracing", prepare_started, prepare_profile, prepare_workload);
    return frame_packet;
}

RaytracingFrameFeedback RaytracingRenderer::RenderFrame(RaytracingFramePacket frame_packet) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyRenderThread());
    assert(runtime_state);
    MOER_PROFILE_SCOPE("Raytracing.RenderFrame");

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
            if (render_profile_capture == nullptr ||
                !gpu_profile_frame.Valid()) {
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
                command_list
                    .IsLegacyGpuProfilingSuppressedForGeneration()) {
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
                // Profiling degradation must not change renderer behavior.
            }
            return false;
        };
    // Order zero is reserved for any already resident persistent-list prefix.
    // Every later order is allocated by CommandList generation, never by
    // worker arrival/completion order or by logical pass count.
    uint64 next_profile_source_order = 1;
    auto ensure_next_profile_source =
        [&](CommandList& command_list,
            RHIQueueBinding queue_binding) noexcept {
            if (command_list.HasGpuScopeRecorder()) {
                return true;
            }
            if (!gpu_profile_frame.Valid() ||
                command_list
                    .IsLegacyGpuProfilingSuppressedForGeneration()) {
                return false;
            }
            const uint64 source_order = next_profile_source_order++;
            return ensure_profile_source(
                command_list, queue_binding, source_order
            );
        };
    const auto marker_mode_for = [](const CommandList& command_list) {
        return command_list.HasGpuScopeRecorder() ?
                   EGpuMarkerMode::Timestamp :
                   EGpuMarkerMode::Label;
    };

    auto& state     = *runtime_state;
    auto& ui_config = frame_packet.config;
    auto  scene_extent_request = frame_packet.scene_render_extent;
    // Keep dock-only changes debounced, but accept a matching OS-window
    // change immediately because presentation resources already have to move.
    if (frame_packet.window.state == EWindowState::SizeChanged && scene_extent_request.valid) {
        scene_extent_request.immediate = true;
    }
    const bool scene_extent_advanced =
        scene_render_extent_tracker.Observe(scene_extent_request);
    const uint2 active_scene_extent = scene_render_extent_tracker.GetActiveExtent();
    const auto ui_execution_thread =
        IsCurrentlyRenderThread() ?
            EUiDrawExecutionThread::Render :
            EUiDrawExecutionThread::Game;
    const auto prepared_ui = UiFrameGraphPass::Prepare(
        std::move(frame_packet.ui_composition),
        std::move(frame_packet.ui_draw_frame),
        ui_execution_thread
    );

    if (frame_packet.frame_id == 0) {
        const uint32_t frame_thread_id = IsCurrentlyRenderThread() ? GetRenderThreadId() : GetGameThreadId();
        LOG_INFO(
            "[Threading] Raytracing frames execute on {} thread id = {}",
            IsCurrentlyRenderThread() ? "Render" : "Game",
            frame_thread_id
        );
    }

    PrepareRenderFrame(frame_packet.window);
    bool  skip_present                    = false;
    bool  split_graph_profiling_frame     = false;
    bool  frame_setup_graph_recorded      = false;
    bool  frame_setup_linear_recorded     = false;
    bool  graph_primary_recorded          = false;
    bool  graph_nrd_recorded              = false;
    bool  graph_composition_recorded      = false;
    bool  graph_antialias_recorded        = false;
    bool  graph_tone_mapping_recorded     = false;
    bool  graph_visualize_recorded        = false;
    bool  graph_show_texture_recorded     = false;
    bool  graph_ui_recorded               = false;
    bool  graph_recording_failed          = state.render_graph_recording_failure_latched;
    bool  linear_gbuffer_recorded         = false;
    bool  linear_lighting_recorded        = false;
    bool  linear_antialias_recorded       = false;
    bool  linear_tone_mapping_recorded    = false;
    bool  linear_visualize_recorded       = false;
    bool  linear_ui_recorded              = false;
    bool  nrd_recorded                    = false;
    bool  lighting_temporal_history_valid = false;
    bool  scene_resources_recreated       = false;
    bool  output_resources_recreated      = false;
    bool  scene_resize_deferred            = false;
    uint8 gbuffer_history_bit             = 0;
    float tone_mapping_elapsed_for_commit = 0.f;
    bool  tone_mapping_enabled_for_commit = false;
    LightingPass::LocalLightSamplingDecision         local_light_sampling{};
    std::optional<PrepareLightPass::PreparedCommand> prepared_lights{};
    std::optional<RaytracingFrameSetupPass::PreparedCommand> prepared_frame_setup{};
#if WITH_NRD
    std::optional<NrdDenoisePass::PreparedCommand>           prepared_nrd{};
    uint64                                                   nrd_graph_submission_value_count = 0;
#endif
    TextureRef                                       selected_debug_texture{};
    bool                                             show_texture_requested = false;
    ShowTextureParams                                show_texture_params{};
    FenceRef                                         graph_submission_fence{};
    FenceRef                                         ui_graph_submission_fence{};
    ExportSubmissionTransaction                      export_submission{};
    uint64                                           graph_submission_value_count = 0;
    UiFrameGraphPass::GraphPasses                    ui_graph_passes{};

    switch (frame_packet.window.state) {
        case EWindowState::Hiding:
            std::this_thread::yield();
            skip_present = true;
            break;
        case EWindowState::SizeChanged:
            break;
        case EWindowState::Default:
            break;
        default:
            assert(false);
            break;
    }

    const uint3 current_scene_extent_3d  = state.rt_ctx->frame_rt.ldr_color->GetExtent();
    const uint3 current_output_extent_3d = state.output->GetExtent();
    const bool recreate_scene_resources = !EqualRenderExtent(
        uint2(current_scene_extent_3d.x, current_scene_extent_3d.y), active_scene_extent
    );
    const bool recreate_output_resources = !EqualRenderExtent(
        uint2(current_output_extent_3d.x, current_output_extent_3d.y),
        frame_packet.window.resolution
    );

    if (!skip_present && (recreate_scene_resources || recreate_output_resources)) {
        if (recreate_output_resources) {
            RecreateOutputResources(frame_packet.window.resolution);
            output_resources_recreated = true;
        }
        if (recreate_scene_resources) {
            if (state.render_graph_recording_failure_latched) {
                scene_resize_deferred = scene_extent_advanced;
                if (scene_resize_deferred) {
                    LOG_WARNING(
                        "[RenderGraph][Fallback] Preserving the last accepted "
                        "raytracing SceneColor resources instead of resizing "
                        "them to {}x{}.",
                        active_scene_extent.x,
                        active_scene_extent.y
                    );
                }
            } else {
                scene_resources_recreated = RecreateSceneResources(active_scene_extent);
            }
        }

        if (scene_resources_recreated || output_resources_recreated) {
            ++render_extent_generation;
            LOG_INFO(
                "[RenderExtent][Raytracing] generation={} requested={}x{} request_valid={} "
                "active_scene={}x{} presentation_output={}x{} swapchain={}x{} "
                "scene_recreated={} output_recreated={} temporal_history_reset={}.",
                render_extent_generation,
                frame_packet.scene_render_extent.extent.x,
                frame_packet.scene_render_extent.extent.y,
                frame_packet.scene_render_extent.valid,
                active_scene_extent.x,
                active_scene_extent.y,
                frame_packet.window.resolution.x,
                frame_packet.window.resolution.y,
                swapchain->size.x,
                swapchain->size.y,
                scene_resources_recreated,
                output_resources_recreated,
                scene_resources_recreated
            );
        }
    }
    const uint3 allocated_scene_extent_3d =
        state.rt_ctx->frame_rt.ldr_color->GetExtent();
    const uint2 allocated_scene_extent(
        allocated_scene_extent_3d.x,
        allocated_scene_extent_3d.y
    );

    state.timer.Stop();
    [[maybe_unused]] const auto frame_time = state.timer.ElapsedMilliseconds();
    state.timer.Start();

    RaytracingFrameFeedback feedback{};
    feedback.frame_id                = frame_packet.frame_id;
    feedback.export_request_finished = ui_config.export_cfg.b_export;

    if (frame_packet.scene_updates.scene_ready && frame_packet.runtime_assets_ready &&
        !graph_recording_failed) {
        bool render_graph_boundary_frame =
            state.first_load || state.b_new_env_map || scene_resources_recreated ||
            output_resources_recreated || ui_config.export_cfg.b_export;
        const bool nrd_active_this_frame = IsNrdDenoiserActive(ui_config.denoiser_cfg.denoiser_type);
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
        const SceneUpdateResult scene_update =
            ExecuteSceneUpdates(
                frame_packet.scene_updates,
                setup_scene_update_profiling
            );
#if WITH_NRD
        const int nrd_mode_this_frame =
            ui_config.denoiser_cfg.denoiser_type;
        if (nrd_active_this_frame) {
            if (!state.nrd_was_active ||
                state.nrd_mode != nrd_mode_this_frame) {
                state.nrd_interface->ResetAcceptedHistory();
                state.nrd_time = 0;
                if (!state.nrd_outputs_initialized) {
                    LOG_INFO(
                        "[NRD] Warming denoised output state on the linear "
                        "path before managed graph activation."
                    );
                }
            }
            state.nrd_was_active = true;
            state.nrd_mode       = nrd_mode_this_frame;
        } else if (state.nrd_was_active) {
            state.nrd_interface->ResetAcceptedHistory();
            state.nrd_time       = 0;
            state.nrd_mode       = -1;
            state.nrd_was_active = false;
        }
#endif
        // Scene updates publish and drain a deterministic SRV/Sampled boundary
        // before returning. FrameSetup captures those refreshed identities and
        // normalizes only the external ASWrite TLAS inside the managed graph,
        // matching dev_parallel_rhi for dynamic-scene frames.
        render_graph_boundary_frame =
            render_graph_boundary_frame || scene_update.rt_scene_replaced;

        if (state.first_load) {
            state.first_load    = false;
            state.b_new_env_map = true;

            if (frame_packet.scene_updates.geometry) {
                Box3D scene_bounds{};
                scene_bounds.min = float3(0.f);
                scene_bounds.max = float3(0.f);
                for (const auto& instance : frame_packet.scene_updates.geometry->instances) {
                    if (instance.bounds.IsValid()) {
                        scene_bounds.Expand(instance.bounds);
                    }
                }

                const float3 extent     = scene_bounds.max - scene_bounds.min;
                const float  max_extent = std::max(std::max(extent.x, extent.y), extent.z);
                const uint   grid_width = state.importance_sampling_context.GetGridConfig().grid_size.x;
                if (grid_width > 0u) {
                    ui_config.grid_config.cell_size = max_extent * 2.0f / grid_width;
                    feedback.has_grid_cell_size     = true;
                    feedback.grid_cell_size         = ui_config.grid_config.cell_size;
                }
            }

            (void)RefreshSceneRuntimeRefs();
            static_cast<void>(ensure_next_profile_source(
                cmd_list, graphics_profile_binding
            ));
            {
                ScopedGpuMarker bootstrap_marker(
                    cmd_list,
                    "Raytracing First Load Bootstrap",
                    GpuMarkerPalette::Transfer(),
                    marker_mode_for(cmd_list)
                );
                state.rt_ctx->FillLowDiscrepancySequence(cmd_list);
                cmd_list.UpdateBindlessArray(bindless_array);
            }
            RHIExecutor::Get().Submit(EQueueType::Graphics, cmd_list.Submit(), ERHIExecSubmitFlags::FlushGPU);
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        }

        std::optional<ScopedGpuMarker> renderer_marker{};
        const auto ensure_renderer_marker = [&]() {
            if (!renderer_marker) {
                static_cast<void>(ensure_next_profile_source(
                    cmd_list, graphics_profile_binding
                ));
                renderer_marker.emplace(
                    cmd_list,
                    "Raytracing Renderer",
                    GpuMarkerPalette::Renderer(),
                    marker_mode_for(cmd_list)
                );
            }
        };
        if (render_graph_boundary_frame || !state.render_graph_enabled ||
            state.render_graph_fallback_latched) {
            ensure_renderer_marker();
        }
        const auto close_renderer_marker = [&]() {
            if (renderer_marker) {
                renderer_marker->Close();
            }
        };

        if (state.b_new_env_map) {
            ScopedGpuMarker environment_marker(
                cmd_list,
                "Pass: Environment Setup",
                GpuMarkerPalette::Pass(),
                marker_mode_for(cmd_list)
            );
            auto            src_env_map = runtime_assets.GetDefaultEnvMap();
            state.env_map               = device.CreateTexture(
                src_env_map->GetName(),
                Extent3D(src_env_map->GetExtent()),
                PF_R16G16B16A16_SFLOAT,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                src_env_map->GetNumMips()
            );
            state.shader_utils.SampleTextureCS(
                cmd_list, src_env_map->GetView(0, 1), state.env_map->GetView(0, 1)
            );

            Sampler sampler{SF_CUBIC, SAM_REPEAT};
            state.env_tex_with_hdl.tex = state.env_map;
            state.env_tex_with_hdl.hdl = bindless_array->AllocateTexture(
                state.env_map->GetView(0, state.env_map->GetNumMips()), sampler
            );

            for (int i = 0; i < state.env_map->GetNumMips(); ++i) {
                state.env_mips.push_back(state.env_map->GetView(i));
            }
            state.shader_utils.GenerateMips(cmd_list, state.env_mips);
            state.b_new_env_map = false;

            state.rt_ctx->LoadDefaultResources(runtime_assets);
            state.rt_ctx->CreateEnvMapResources(state.env_tex_with_hdl, cmd_list);
        }

        if (!state.tone_mapping_pass) {
            ToneMappingPass::CreateInfo info{};
            info.color_lut          = state.rt_ctx->default_res.black_tex;
            state.tone_mapping_pass = MakeUnique<ToneMappingPass>(device, manager, info);
        }

        Camera& camera = frame_packet.scene_updates.main_camera;
        state.rt_ctx->FillLowDiscrepancySequence(cmd_list);
        state.b_export = ui_config.export_cfg.b_export;

        ToneMappingPass::Params tone_params{};
        AntialiasPass::Params   aa_params{};
        {
            auto grid_cfg      = state.importance_sampling_context.GetGridChangeableConfig();
            grid_cfg.cell_size = ui_config.grid_config.cell_size;
            grid_cfg.center    = camera.GetPosition();

            auto grid_static_cfg = state.importance_sampling_context.GetGridConfig();
            grid_static_cfg.SetLightsPerCell(ui_config.grid_config.GetLightsPerCell());
            grid_static_cfg.grid_mode = ui_config.grid_config.grid_mode;
            state.importance_sampling_context.SetGridConfig(grid_static_cfg);
            state.importance_sampling_context.SetChangeableGridConfig(grid_cfg);

            state.rt_ctx->config.reblur_diffuse_hit_dist_params =
                *reinterpret_cast<const float4*>(&ui_config.denoiser_cfg.hit_dist_params);
            state.rt_ctx->config.reblur_specular_hit_dist_params =
                *reinterpret_cast<const float4*>(&ui_config.denoiser_cfg.hit_dist_params);
            state.rt_ctx->config.denoiser_mode = ui_config.denoiser_cfg.denoiser_type;

            auto di_initial_sample_config = state.importance_sampling_context.GetDIInitialSampleParams();
            auto di_temporal_resampling_config =
                state.importance_sampling_context.GetDITemporalResampleParams();
            auto di_spatial_resampling_config =
                state.importance_sampling_context.GetDISpatialResampleParams();

            di_initial_sample_config.local_light_sample_mode =
                ui_config.restir_di_cfg.initial_sample_config.local_light_sample_mode;
            state.rt_ctx->config.enable_adaptive_local_light_sampling =
                ui_config.restir_di_cfg.initial_sample_config.enable_adaptive_local_light_sampling &&
                !IsLightGridForced();
            state.rt_ctx->config.grid_min_local_light_count = static_cast<uint>(
                std::max(ui_config.restir_di_cfg.initial_sample_config.grid_min_local_light_count, 1)
            );
            di_initial_sample_config.env_map_is =
                state.importance_sampling_context.GetLightBufferParams().env_light.light_cnt;
            di_temporal_resampling_config.bias_correction_mode =
                ui_config.restir_di_cfg.temporal_resample_config.bias_correction;
            di_temporal_resampling_config.depth_threshold =
                ui_config.restir_di_cfg.temporal_resample_config.depth_threshold;
            di_temporal_resampling_config.normal_threshold =
                ui_config.restir_di_cfg.temporal_resample_config.normal_threshold;
            di_spatial_resampling_config.bias_correction_mode =
                ui_config.restir_di_cfg.spatial_resample_config.bias_correction;
            di_spatial_resampling_config.depth_threshold =
                ui_config.restir_di_cfg.spatial_resample_config.depth_threshold;
            di_spatial_resampling_config.normal_threshold =
                ui_config.restir_di_cfg.spatial_resample_config.normal_threshold;
            di_spatial_resampling_config.num_spatial_samples =
                ui_config.restir_di_cfg.spatial_resample_config.num_spatial_samples;

            state.importance_sampling_context.SetReSTIRDIInitialSampleParams(di_initial_sample_config);
            state.importance_sampling_context.SetReSTIRDITemporalResampleParams(di_temporal_resampling_config
            );
            state.importance_sampling_context.SetReSTIRDISpatialResampleParams(di_spatial_resampling_config);

            const auto& tone_cfg                  = ui_config.tone_mapping_cfg;
            tone_params.eye_adaptation_speed_down = tone_cfg.eye_adaptation_speed_down;
            tone_params.eye_adaptation_speed_up   = tone_cfg.eye_adaptation_speed_up;
            tone_params.exposure_bias             = tone_cfg.exposure_bias;
            tone_params.max_adapted_luminance     = tone_cfg.max_adapted_luminance;
            tone_params.min_adapted_luminance     = tone_cfg.min_adapted_luminance;
            tone_params.histogram_low_percentile  = tone_cfg.histogram_low_percentile;
            tone_params.histogram_high_percentile = tone_cfg.histogram_high_percentile;
            tone_params.white_point               = tone_cfg.white_point;
            tone_params.enable_tone_mapping       = tone_cfg.enable_tone_mapping;
            tone_mapping_elapsed_for_commit       = camera.GetDeltaTime();
            tone_mapping_enabled_for_commit       = tone_params.enable_tone_mapping;

            const auto& aa_cfg             = ui_config.aa_cfg;
            aa_params.clamping_factor      = aa_cfg.clamping_factor;
            aa_params.new_frame_weight     = aa_cfg.new_frame_weight;
            aa_params.max_radiance         = aa_cfg.max_radiance;
            aa_params.enable_history_clamp = aa_cfg.enable_history_clamping;

            state.antialias_pass->SetJitter(aa_cfg.jitter_mode);
            state.rt_ctx->b_parallel_process_light = ui_config.process_light_cfg.parallel_mode;
            state.rt_ctx->num_threads              = ui_config.process_light_cfg.num_threads;
        }

        state.importance_sampling_context.TickFrame(time);
        state.visualize_config.visualize_mode = ui_config.final_color;

        const auto& scene_snapshot = frame_packet.scene_snapshot;

        const auto& debug_input = frame_packet.debug_input;
        const auto  selected_texture_it =
            state.material_textures.find(debug_input.selected_material_texture_name);
        show_texture_requested = debug_input.show_final_texture &&
                                 selected_texture_it != state.material_textures.end() &&
                                 selected_texture_it->second.tex;
        if (show_texture_requested) {
            selected_debug_texture           = selected_texture_it->second.tex;
            show_texture_params.bdls_handle  = selected_texture_it->second.hdl;
            show_texture_params.mip_level    = static_cast<uint>(std::max(debug_input.mip_level, 0));
            show_texture_params.use_bindless = debug_input.use_bindless ? 1u : 0u;
        }

        state.rt_ctx->Tick(camera, state.antialias_pass->GetPixelOffset());
        gbuffer_history_bit = state.rt_ctx->b_current_frame ? uint8(1) : uint8(2);
        const std::array<const Buffer*, 3> lighting_working_set{
            state.rt_ctx->light_reservoir_buf.Get(),
            state.rt_ctx->ris_buf.Get(),
            state.rt_ctx->ris_light_data_buf.Get()
        };
        const uint lighting_reservoir_block_array_pitch =
            state.rt_ctx->is_ctx.GetReSTIRDIRuntimeConfig().reservoir_buffer_params.block_array_pitch;
        if (state.lighting_working_set != lighting_working_set ||
            state.lighting_reservoir_block_array_pitch != lighting_reservoir_block_array_pitch) {
            state.lighting_working_set                 = lighting_working_set;
            state.lighting_reservoir_block_array_pitch = lighting_reservoir_block_array_pitch;
            // ReSTIR DI rotates three logical reservoir slices. Track the
            // slices actually written by accepted linear frames rather than
            // coupling graph eligibility to an implicit frame countdown.
            state.lighting_initialized_reservoir_mask = 0;
            LOG_INFO(
                "[RenderGraph][Raytracing] Lighting working set changed; "
                "warming reservoir slices linearly (reservoir block pitch={}).",
                lighting_reservoir_block_array_pitch
            );
        }
        const auto& lighting_buffer_indices =
            state.rt_ctx->is_ctx.GetReSTIRDIBufferIndices();
        const uint temporal_input_slice =
            lighting_buffer_indices.temperal_resample_input_buff_idx;
        const uint8 temporal_input_bit =
            temporal_input_slice < 3u ? uint8(1u << temporal_input_slice) : uint8(0);
        const uint8 previous_gbuffer_bit =
            state.rt_ctx->b_current_frame ? uint8(2) : uint8(1);
        lighting_temporal_history_valid =
            temporal_input_bit != 0 &&
            (state.lighting_initialized_reservoir_mask & temporal_input_bit) != 0 &&
            (state.gbuffer_initialized_history_mask & previous_gbuffer_bit) != 0;
        prepared_lights.emplace(
            state.prepare_light_pass->Prepare(*state.rt_ctx, scene_snapshot, state.rt_instance_revision)
        );
        if (!graph_recording_failed && state.rt_scene) {
            const bool needs_tlas_build =
                IsLegacyTlasUpdateEnabled() ||
                state.current_tlas_revision != state.rt_instance_revision;
            prepared_frame_setup.emplace(state.frame_setup_pass->Prepare(
                *state.rt_ctx,
                state.rt_scene,
                bindless_array,
                state.rt_instance_revision,
                needs_tlas_build,
                scene_update.gpu_resources_updated,
                scene_update.tlas_built
            ));
        }
#if WITH_NRD
        if (!graph_recording_failed && nrd_active_this_frame) {
            auto command = NrdDenoisePass::Prepare(
                state.nrd_interface,
                static_cast<uint32>(state.nrd_time),
                Vector2ui(allocated_scene_extent.x, allocated_scene_extent.y),
                state.antialias_pass->GetPixelOffset(),
                Transpose(camera.GetViewMatrix()),
                Transpose(camera.GetProjectionMatrix()),
                ResolveNrdDenoiser(
                    ui_config.denoiser_cfg.denoiser_type
                ),
                *state.rt_ctx
            );
            if (command.IsValid()) {
                prepared_nrd.emplace(std::move(command));
            }
        }
#endif

        const bool unified_graph_eligible =
            prepared_frame_setup && state.render_graph_enabled &&
            !state.render_graph_fallback_latched &&
            state.gbuffer_initialized_history_mask == uint8(0b11) &&
            state.lighting_initialized_reservoir_mask == uint8(0b111) &&
            state.antialias_pass->IsHistoryReadyForGraph() &&
#if WITH_NRD
            (!nrd_active_this_frame ||
             (prepared_nrd.has_value() &&
              state.nrd_outputs_initialized)) &&
#endif
            !render_graph_boundary_frame;

        if (unified_graph_eligible) {
            RenderGraph graph(
                "Raytracing.Frame",
                RenderGraph::QueueTopology::FromRHI()
            );
            RTGraphFrameSetupResources setup_resources{};
            const bool setup_passes_added = state.frame_setup_pass->AddPasses(
                graph,
                *prepared_frame_setup,
                setup_resources
            );
            RTGraphFrameResources graph_resources =
                setup_passes_added ?
                    RegisterRTGraphFrameResources(
                        graph,
                        *state.rt_ctx,
                        setup_resources
                    ) :
                    RTGraphFrameResources{};
            const bool prepare_lights_added =
                setup_passes_added && state.prepare_light_pass->AddPasses(
                graph,
                *prepared_lights,
                setup_resources
            );
            const bool gbuffer_added        = prepare_lights_added && state.g_buffer_pass->AddPasses(
                                                                   graph,
                                                                   graph_resources,
                                                                   *state.rt_ctx,
                                                                   state.normal_roughness_readable
                                                               );
            const bool lighting_added = gbuffer_added && state.lighting_pass->AddPasses(
                                                             graph,
                                                             graph_resources,
                                                             *state.rt_ctx,
                                                             prepared_lights->GetLightBufferParams(),
                                                             true,
                                                             local_light_sampling
                                                         );
            bool nrd_added = true;
#if WITH_NRD
            if (nrd_active_this_frame) {
                nrd_added =
                    lighting_added && prepared_nrd &&
                    NrdDenoisePass::AddPasses(
                        graph,
                        graph_resources,
                        *prepared_nrd,
                        state.nrd_outputs_initialized
                    );
            }
#endif
            const bool composition_in_graph = true;
            const bool composition_added =
                lighting_added && nrd_added &&
                state.composition_pass->AddPasses(
                    graph,
                    graph_resources,
                    *state.rt_ctx
                );
            const bool antialias_in_graph = true;
            const bool antialias_added =
                composition_added && state.antialias_pass->AddPasses(
                                                                 graph,
                                                                 graph_resources,
                                                                 *state.rt_ctx,
                                                                 aa_params,
                                                                 state.b_feedback_valid,
                                                                 state.rt_ctx->frame_rt.hdr_color,
                                                                 state.rt_ctx->frame_rt.resolved_color
                                                             );
            const bool tone_mapping_in_graph = true;
            const bool tone_mapping_added =
                antialias_added && state.tone_mapping_pass->AddPasses(
                                                                  graph,
                                                                  graph_resources,
                                                                  *state.rt_ctx,
                                                                  tone_params,
                                                                  state.rt_ctx->frame_rt.resolved_color,
                                                                  state.rt_ctx->frame_rt.ldr_color
                                                              );
            const bool visualize_in_graph = true;
            const bool visualize_added =
                tone_mapping_added && state.visualize_pass->AddPasses(
                    graph,
                    graph_resources,
                    *state.rt_ctx,
                    state.visualize_config
                );
            const bool show_texture_in_graph = show_texture_requested;
            RenderGraph::TextureHandle selected_graph_texture{};
            if (show_texture_in_graph) {
                selected_graph_texture =
                    ImportRTGraphTexture(graph, "RT.selected_material_texture", selected_debug_texture);
            }
            const bool show_texture_added =
                !show_texture_in_graph || (visualize_added && state.show_texture_pass->AddPass(
                                                                  graph,
                                                                  selected_graph_texture,
                                                                  graph_resources.ldr_color,
                                                                  show_texture_params,
                                                                  selected_debug_texture,
                                                                  state.rt_ctx->frame_rt.ldr_color,
                                                                  graph_resources.frame_setup.ready,
                                                                  graph_resources.presentation_ready
                                                              ));
            const TextureRef graph_final_color =
                frame_packet.debug_input.show_final_texture ?
                    state.rt_ctx->frame_rt.ldr_color :
                    state.rt_ctx->frame_rt.debug_color;
            const RenderGraph::TextureHandle graph_final_color_handle =
                frame_packet.debug_input.show_final_texture ?
                    graph_resources.ldr_color :
                    graph_resources.debug_color;
            const bool ui_added =
                show_texture_added &&
                UiFrameGraphPass::AddPasses(
                    graph,
                    *ui_combine_pass,
                    prepared_ui,
                    graph_final_color_handle,
                    graph_final_color,
                    state.output,
                    graph_resources.presentation_ready,
                    ui_graph_passes
                );
            const bool graph_passes_added = prepare_lights_added && gbuffer_added && lighting_added &&
                                            nrd_added && composition_added && antialias_added &&
                                            tone_mapping_added &&
                                            visualize_added && show_texture_added && ui_added;
            if (graph_passes_added && IsRenderGraphRecordingFailureInjected()) {
                const RenderGraph::PassHandle fault_dependency =
                    ui_graph_passes.draw;
                graph.AddRecordPass(
                    "RT.FaultInjection.RecordingFailure",
                    [fault_dependency](RenderGraph::PassBuilder& builder) {
                        builder
                            .ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                            .DependsOn(fault_dependency)
                            .SideEffect();
                    },
                    [](CommandList&) {
                        throw std::runtime_error("MOER_RT_RDG_RECORDING_FAIL requested a synthetic "
                                                 "managed-record failure");
                    },
                    RenderGraph::PassExecutionClass::ParallelRecordEligible
                );
                LOG_WARNING("[RenderGraph][Injection] Raytracing managed-record failure "
                            "armed by MOER_RT_RDG_RECORDING_FAIL.");
            }
            if (!graph_passes_added) {
                state.render_graph_fallback_latched = true;
                ensure_renderer_marker();
                LOG_ERROR("[RenderGraph][Fallback] Raytracing unified graph could not "
                          "import a required FrameSetup/PrepareLights/GBuffer/Lighting/"
                          "NRD/Composition/AntiAlias/"
                          "ToneMapping/Visualize/ShowTexture/UI resource. Using the linear "
                          "path for this renderer instance.");
            } else if (!graph.Compile()) {
                state.render_graph_fallback_latched = true;
                ensure_renderer_marker();
                LOG_ERROR(
                    "[RenderGraph][Fallback] Raytracing unified graph compile "
                    "failed before execution: {}. Using the linear path for "
                    "this renderer instance.",
                    graph.GetCompileError()
                );
            } else {
                if (state.render_graph_debug_dump) {
                    std::string dump = graph.Dump();
                    if (state.logged_render_graph_dumps.emplace(dump).second) {
                        LOG_INFO("[RenderGraph][Raytracing][DebugDump]\n{}", dump);
                    }
                }

                // The unified graph is the first managed transaction on a
                // steady-state frame. Boundary work normally makes the graph
                // ineligible, so this prefix is only a defensive ordered
                // submission for an unexpected caller-side command.
                close_renderer_marker();
                if (!cmd_list.IsEmpty()) {
                    static_cast<void>(ensure_next_profile_source(
                        cmd_list, graphics_profile_binding
                    ));
                    CmdSubmit prefix_submit = cmd_list.Submit().DebugLabel(
                        std::format("Raytracing Frame {}/Graph Prefix", frame_packet.frame_id),
                        GpuMarkerPalette::Renderer()
                    );
                    prefix_submit.SetProfilingPhase(
                        split_graph_profiling_frame ? ERHIProfilingPhase::Continue : ERHIProfilingPhase::Begin
                    );
                    split_graph_profiling_frame = true;
                    RHIExecutor::Get().Submit(
                        EQueueType::Graphics, std::move(prefix_submit), ERHIExecSubmitFlags::None
                    );
                }

                const uint64 graph_source_order_base =
                    next_profile_source_order;
                next_profile_source_order =
                    graph_source_order_base +
                    static_cast<uint64>(
                        graph.GetCompiledPlan().execution_order.size()
                    );
                bool       graph_recording_profiling_started = split_graph_profiling_frame;
                graph_submission_fence    = device.CreateFence();
                ui_graph_submission_fence = device.CreateFence();
                const auto configure_recording_source        = [&](const RenderGraph::ExecutedPassInfo& pass,
                                                            RHIRecordingSource&                  source) {
                    source.submit_metadata.debug_label =
                        std::format("Raytracing Frame {}/{}", frame_packet.frame_id, pass.name);
                    source.submit_metadata.debug_label_color = GpuMarkerPalette::Pass();
                    source.submit_metadata.profiling_phase   = graph_recording_profiling_started ?
                                                                          ERHIProfilingPhase::Continue :
                                                                          ERHIProfilingPhase::Begin;
                    graph_recording_profiling_started        = true;
                    source.submit_metadata.signal_fences.emplace_back(
                        RHIRecordingFencePoint{
                            .fence = graph_submission_fence,
                            .value = ++graph_submission_value_count,
                        }
                    );
                    if (pass.handle == ui_graph_passes.draw) {
                        source.submit_metadata.signal_fences.emplace_back(
                            RHIRecordingFencePoint{
                                .fence = ui_graph_submission_fence,
                                .value = 1,
                            }
                        );
                    }
#if WITH_NRD
                    if (nrd_active_this_frame && prepared_nrd) {
                        source.submit_metadata.signal_fences.emplace_back(
                            RHIRecordingFencePoint{
                                .fence =
                                    prepared_nrd->graph_submission_fence,
                                .value =
                                    ++nrd_graph_submission_value_count,
                            }
                        );
                    }
#endif
                };
                RenderGraph::GpuProfilingOptions gpu_profiling{};
                if (gpu_profile_frame.Valid()) {
                    gpu_profiling.try_bind_source =
                        [&](const RenderGraph::ExecutedPassInfo&,
                            CommandList&    recording_command_list,
                            RHIQueueBinding queue_binding,
                            uint64          source_order) noexcept {
                            return try_bind_profile_source(
                                recording_command_list,
                                queue_binding,
                                source_order
                            );
                        };
                    gpu_profiling.source_order_base =
                        graph_source_order_base;
                }
                if (graph.ExecuteRecording(
                        {},
                        configure_recording_source,
                        state.render_graph_parallel_recording,
                        {},
                        RenderGraph::ActiveRecordingOptions{.enabled = true},
                        gpu_profiling
                    )) {
                    split_graph_profiling_frame = graph_recording_profiling_started;
                    frame_setup_graph_recorded  = true;
                    graph_primary_recorded      = true;
#if WITH_NRD
                    graph_nrd_recorded          =
                        nrd_active_this_frame && nrd_added;
#endif
                    graph_composition_recorded  = composition_in_graph;
                    graph_antialias_recorded    = antialias_in_graph;
                    graph_tone_mapping_recorded = tone_mapping_in_graph;
                    graph_visualize_recorded    = visualize_in_graph;
                    graph_show_texture_recorded = show_texture_in_graph;
                    graph_ui_recorded           = ui_added;
                } else {
                    if (gpu_profile_frame.Valid() &&
                        (cmd_list.HasGpuScopeRecorder() ||
                         cmd_list
                             .IsLegacyGpuProfilingSuppressedForGeneration())) {
                        // A failed caller-owned graph source must never leak
                        // into the unrelated UI/present tail generation.
                        CmdSubmit rejected_graph_generation =
                            cmd_list.Submit();
                        static_cast<void>(rejected_graph_generation);
                    }
                    // ExecuteRecording joins every producer and fails the
                    // graph-wide commit gate. Sync only joins the resulting
                    // rejection cleanup and the already accepted Prefix; it is
                    // not used as a resource-state substitute.
                    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
                    graph_recording_failed                       = true;
                    state.render_graph_fallback_latched          = true;
                    state.render_graph_recording_failure_latched = true;
                    prepared_ui->Abandon();
                    LOG_ERROR(
                        "[RenderGraph][Fallback] Raytracing unified graph "
                        "recording failed before transaction commit: {}. "
                        "Preserving the last accepted output and disabling "
                        "managed renderer passes until this renderer instance "
                        "is recreated.",
                        graph.GetCompileError()
                    );
                }
            }
        }
        if (!graph_recording_failed) {
            // Every linear suffix after a managed graph belongs to one fresh
            // persistent-list generation. Repeated calls below reuse this
            // recorder until export or the final tail seals the generation.
            static_cast<void>(ensure_next_profile_source(
                cmd_list, graphics_profile_binding
            ));
        }
        if (!graph_recording_failed && prepared_frame_setup &&
            !frame_setup_graph_recorded) {
            ScopedGpuMarker frame_setup_marker(
                cmd_list,
                "Pass: Frame Setup",
                GpuMarkerPalette::Pass(),
                marker_mode_for(cmd_list)
            );
            frame_setup_linear_recorded = state.frame_setup_pass->ProcessLinear(
                cmd_list, *prepared_frame_setup
            );
            if (!frame_setup_linear_recorded) {
                graph_recording_failed              = true;
                state.render_graph_fallback_latched = true;
                LOG_ERROR(
                    "[RenderGraph][Fallback] Raytracing linear FrameSetup could not "
                    "record a valid bindless/TLAS boundary. Skipping managed "
                    "rendering for this frame; the linear path will retry next frame."
                );
            }
        }
        if (!graph_recording_failed && !graph_primary_recorded) {
            {
                ScopedGpuMarker pass_marker(
                    cmd_list,
                    "Pass: Prepare Lights",
                    GpuMarkerPalette::Pass(),
                    marker_mode_for(cmd_list)
                );
                state.prepare_light_pass->Process(cmd_list, *prepared_lights);
            }
            ScopedGpuMarker pass_marker(
                cmd_list,
                "Pass: GBuffer",
                GpuMarkerPalette::Pass(),
                marker_mode_for(cmd_list)
            );
            state.g_buffer_pass->Process(cmd_list, *state.rt_ctx);
            linear_gbuffer_recorded = true;

            pass_marker.Close();
            ScopedGpuMarker lighting_marker(
                cmd_list,
                "Pass: Lighting",
                GpuMarkerPalette::Pass(),
                marker_mode_for(cmd_list)
            );
            state.prepare_light_pass->RecordLightingInputTransitions(cmd_list, *prepared_lights);
            local_light_sampling = state.lighting_pass->Process(
                cmd_list,
                *state.rt_ctx,
                prepared_lights->GetLightBufferParams(),
                lighting_temporal_history_valid
            );
            state.prepare_light_pass->RecordAcceptedBoundary(cmd_list, *prepared_lights);
            linear_lighting_recorded = true;
        } else if (graph_primary_recorded && !graph_ui_recorded) {
            state.g_buffer_pass->RecordLegacyTailBridge(
                cmd_list,
                *state.rt_ctx,
                graph_composition_recorded,
                graph_antialias_recorded,
                graph_tone_mapping_recorded,
                graph_visualize_recorded
            );
        }
        feedback.configured_local_light_sample_mode    = local_light_sampling.configured_mode;
        feedback.effective_local_light_sample_mode     = local_light_sampling.effective_mode;
        feedback.adaptive_local_light_fallback_applied = local_light_sampling.adaptive_fallback_applied;
        feedback.local_light_count                     = local_light_sampling.local_light_count;

#if WITH_NRD
        if (!graph_recording_failed && nrd_active_this_frame) {
            if (graph_nrd_recorded) {
                nrd_recorded = true;
            } else if (prepared_nrd) {
                ScopedGpuMarker denoiser_marker(
                    cmd_list,
                    "Pass: Radiance Denoising",
                    GpuMarkerPalette::Pass(),
                    marker_mode_for(cmd_list)
                );
                NrdDenoisePass::Process(cmd_list, *prepared_nrd);
                nrd_recorded = true;
            } else {
                graph_recording_failed              = true;
                state.render_graph_fallback_latched = true;
                LOG_ERROR(
                    "[NRD] Failed to prepare an immutable denoise frame. "
                    "Preserving the last accepted output."
                );
            }
        }
#endif

        if (!graph_recording_failed && !graph_composition_recorded) {
            ScopedGpuMarker pass_marker(
                cmd_list,
                "Pass: Composition",
                GpuMarkerPalette::Pass(),
                marker_mode_for(cmd_list)
            );
            state.composition_pass->Process(cmd_list, *state.rt_ctx);
        }
        if (!graph_recording_failed && !graph_antialias_recorded) {
            ScopedGpuMarker pass_marker(
                cmd_list,
                "Pass: Anti Aliasing",
                GpuMarkerPalette::Pass(),
                marker_mode_for(cmd_list)
            );
            state.antialias_pass->Process(
                cmd_list,
                aa_params,
                state.b_feedback_valid,
                state.rt_ctx->frame_rt.hdr_color,
                state.rt_ctx->frame_rt.resolved_color
            );
            linear_antialias_recorded = true;
        }
        if (!graph_recording_failed && !graph_tone_mapping_recorded) {
            ScopedGpuMarker pass_marker(
                cmd_list,
                "Pass: Tone Mapping",
                GpuMarkerPalette::Pass(),
                marker_mode_for(cmd_list)
            );
            state.tone_mapping_pass->Process(
                cmd_list, tone_params, state.rt_ctx->frame_rt.resolved_color, state.rt_ctx->frame_rt.ldr_color
            );
            linear_tone_mapping_recorded = true;
        }
        if (!graph_recording_failed && !graph_visualize_recorded) {
            ScopedGpuMarker pass_marker(
                cmd_list,
                "Pass: Visualize",
                GpuMarkerPalette::Pass(),
                marker_mode_for(cmd_list)
            );
            state.visualize_pass->Process(cmd_list, *state.rt_ctx, state.visualize_config);
            linear_visualize_recorded = true;
        }

        if (!graph_recording_failed && show_texture_requested && !graph_show_texture_recorded) {
            ScopedGpuMarker debug_texture_marker(
                cmd_list,
                "Pass: Debug Texture",
                GpuMarkerPalette::Pass(),
                marker_mode_for(cmd_list)
            );
            state.show_texture_pass->Process(
                cmd_list, show_texture_params, selected_debug_texture, state.rt_ctx->frame_rt.ldr_color
            );
        }

        if (state.b_export) {
            close_renderer_marker();
            static_cast<void>(ensure_next_profile_source(
                cmd_list, graphics_profile_binding
            ));
            {
                ScopedGpuMarker export_marker(
                    cmd_list,
                    "Raytracing Export Prefix",
                    GpuMarkerPalette::Transfer(),
                    marker_mode_for(cmd_list)
                );
            }
            const bool modern_export_profiling =
                cmd_list.HasGpuScopeRecorder() ||
                cmd_list
                    .IsLegacyGpuProfilingSuppressedForGeneration();
            export_submission.Reset(device.CreateFence());
            CmdSubmit export_submit =
                cmd_list.Submit()
                    .DebugLabel(
                        std::format("Raytracing Export Frame {}", frame_packet.frame_id),
                        GpuMarkerPalette::Frame()
                    );
            if (!modern_export_profiling) {
                export_submit.TickProfiling();
            }
            export_submission.AttachSignal(export_submit);
            RHIExecutor::Get().Submit(
                EQueueType::Graphics,
                std::move(export_submit),
                ERHIExecSubmitFlags::FlushGPU
            );
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
            const auto export_source_outcome =
                export_submission.SourceOutcome();
            if (export_source_outcome ==
                EExportSubmissionOutcome::Accepted) {
                const uint64 readback_source_order =
                    next_profile_source_order++;
                const auto setup_readback_profile =
                    [&](CommandList& readback_command_list) noexcept {
                        return ensure_profile_source(
                            readback_command_list,
                            graphics_profile_binding,
                            readback_source_order
                        );
                    };
                const bool readback_accepted = DumpTextureToFile(
                    ui_config.export_cfg,
                    state.rt_ctx->frame_rt,
                    device,
                    gfx_queue,
                    state.exported_file_path,
                    std::to_string(time),
                    export_submission,
                    setup_readback_profile
                );
                if (!readback_accepted) {
                    if (export_submission.ReadbackOutcome() ==
                        EExportSubmissionOutcome::Failed) {
                        LOG_CRITICAL(
                            "[Raytracing][Export] Texture readback failed "
                            "without an explicit recoverable rejection for "
                            "frame {}. Skipping encode; the frame failure "
                            "policy will latch the renderer.",
                            frame_packet.frame_id
                        );
                    } else {
                        LOG_WARNING(
                            "[Raytracing][Export] Texture readback was rejected "
                            "or could not be scheduled for frame {}. Skipping "
                            "encode and retaining the export request for retry.",
                            frame_packet.frame_id
                        );
                    }
                }
            } else if (
                export_source_outcome ==
                EExportSubmissionOutcome::Rejected
            ) {
                LOG_CRITICAL(
                    "[Raytracing][Export] Native submission rejected export "
                    "frame {}. Skipping readback and retaining the export "
                    "request for retry.",
                    frame_packet.frame_id
                );
            } else {
                LOG_CRITICAL(
                    "[Raytracing][Export] Export frame {} failed without an "
                    "explicit recoverable rejection. Skipping readback; the "
                    "frame failure policy will latch the renderer.",
                    frame_packet.frame_id
                );
            }
        }
        // Keep the long linear renderer root inside the scene-rendering
        // generation. The UI/present tail below owns a separate central root
        // and may belong to a fresh generation after export.
        close_renderer_marker();
    }

    const TextureRef final_color = frame_packet.debug_input.show_final_texture ?
                                       state.rt_ctx->frame_rt.ldr_color :
                                       state.rt_ctx->frame_rt.debug_color;

    static_cast<void>(ensure_next_profile_source(
        cmd_list, graphics_profile_binding
    ));
    std::optional<ScopedGpuMarker> frame_tail_marker{};
    frame_tail_marker.emplace(
        cmd_list,
        "Raytracing Frame Tail",
        GpuMarkerPalette::Ui(),
        marker_mode_for(cmd_list)
    );
    if (!graph_ui_recorded &&
        prepared_ui->GetRecordingState() ==
            UiFrameGraphPass::ERecordingState::Prepared) {
        try {
            linear_ui_recorded = UiFrameGraphPass::ProcessLinear(
                cmd_list,
                *ui_combine_pass,
                prepared_ui,
                final_color,
                state.output
            );
        } catch (const std::exception& exception) {
            // UI recording is part of the frame transaction. Discard every
            // caller-owned command already recorded into this tail rather
            // than submitting a partially composed/potentially replayed
            // packet.
            frame_tail_marker->Close();
            frame_tail_marker.reset();
            static_cast<void>(cmd_list.Submit());
            static_cast<void>(ensure_next_profile_source(
                cmd_list, graphics_profile_binding
            ));
            frame_tail_marker.emplace(
                cmd_list,
                "Raytracing Frame Tail",
                GpuMarkerPalette::Ui(),
                marker_mode_for(cmd_list)
            );
            prepared_ui->Abandon();
            graph_recording_failed                       = true;
            state.render_graph_fallback_latched          = true;
            state.render_graph_recording_failure_latched = true;
            LOG_CRITICAL(
                "[RenderGraph][UI] Linear copied-frame recording failed: {}. "
                "Discarding the frame tail and preserving the last accepted "
                "presentation.",
                exception.what()
            );
        } catch (...) {
            frame_tail_marker->Close();
            frame_tail_marker.reset();
            static_cast<void>(cmd_list.Submit());
            static_cast<void>(ensure_next_profile_source(
                cmd_list, graphics_profile_binding
            ));
            frame_tail_marker.emplace(
                cmd_list,
                "Raytracing Frame Tail",
                GpuMarkerPalette::Ui(),
                marker_mode_for(cmd_list)
            );
            prepared_ui->Abandon();
            graph_recording_failed                       = true;
            state.render_graph_fallback_latched          = true;
            state.render_graph_recording_failure_latched = true;
            LOG_CRITICAL(
                "[RenderGraph][UI] Linear copied-frame recording failed. "
                "Discarding the frame tail and preserving the last accepted "
                "presentation."
            );
        }
    }

    frame_tail_marker->Close();
    time++;
    CmdSubmit frame_submit =
        cmd_list.Submit()
            .DebugLabel(std::format("Raytracing Frame {}", frame_packet.frame_id), GpuMarkerPalette::Frame())
            .Signal(timeline, time)
            .DeleteResources();
    if (export_submission.IsActive()) {
        // The final UI/present tail belongs to the same frame transaction as
        // the separately submitted export prefix. A rejected prefix must not
        // publish or present an otherwise accepted stale tail.
        export_submission.AttachDependentWait(frame_submit);
    }
    if (split_graph_profiling_frame) {
        frame_submit.SetProfilingPhase(ERHIProfilingPhase::End);
    } else {
        // The final tail remains the one legacy Complete boundary for linear
        // and boundary frames. Modern per-scope queries coexist with this
        // submit-level phase; omitting it would freeze GetProfilerEntry().
        frame_submit.TickProfiling();
    }
    std::optional<RHIPresentRequest> present_request{};
    if (!skip_present) {
        const auto output_view = state.output->GetView();
        if (frame_packet.window.state == EWindowState::SizeChanged) {
            LOG_INFO(
                "[Threading][Resize] Raytracing main present source={}x{}, swapchain={}x{}, UI platform "
                "viewports={}.",
                output_view.extent.x,
                output_view.extent.y,
                swapchain->size.x,
                swapchain->size.y,
                prepared_ui->GetDrawFrame().platform_viewports.size()
            );
        }
        if (output_view.extent.x == swapchain->size.x && output_view.extent.y == swapchain->size.y) {
            feedback.main_present_receipt = CreateMainPresentReceipt(
                frame_packet.scene_updates.scene_ready && frame_packet.runtime_assets_ready
            );
            present_request.emplace(swapchain, output_view, feedback.main_present_receipt);
        } else {
            LOG_WARNING(
                "Skipping stale raytracing main-window present: source={}x{}, swapchain={}x{}.",
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
    if (render_profile_capture != nullptr && gpu_profile_frame.Valid()) {
        static_cast<void>(
            render_profile_capture->Seal(gpu_profile_frame)
        );
        static_cast<void>(render_profile_capture->DrainReadyFrames());
    }
    // Accepted renderer state follows native queue publication, not CPU
    // recording completion. The final tail proves the ordered suffix was
    // accepted; the export and per-source graph fences prevent an accepted
    // tail from hiding a rejected separately submitted source.
    const EExportSubmissionOutcome tail_submission_outcome =
        ExportSubmissionTransaction::ResolveReceiptOutcome(
            timeline.Get(),
            time
        );
    bool frame_native_accepted =
        tail_submission_outcome ==
        EExportSubmissionOutcome::Accepted;
    bool independent_submission_source_failed =
        state.render_graph_recording_failure_latched;
    frame_native_accepted =
        export_submission.FoldAcceptance(frame_native_accepted);
    if (export_submission.ReadbackOutcome() ==
        EExportSubmissionOutcome::Failed) {
        independent_submission_source_failed = true;
        frame_native_accepted                 = false;
    }
    if (graph_submission_fence) {
        for (uint64 value = 1; value <= graph_submission_value_count; ++value) {
            const bool source_accepted =
                graph_submission_fence->WaitSubmitted(value);
            independent_submission_source_failed =
                independent_submission_source_failed ||
                !source_accepted;
            frame_native_accepted =
                source_accepted && frame_native_accepted;
        }
    }

    bool ui_source_accepted = false;
    if (graph_ui_recorded && ui_graph_submission_fence) {
        ui_source_accepted =
            ui_graph_submission_fence->WaitSubmitted(1);
        independent_submission_source_failed =
            independent_submission_source_failed ||
            !ui_source_accepted;
    } else if (linear_ui_recorded) {
        // Linear UI work lives in the final tail itself.
        ui_source_accepted =
            tail_submission_outcome ==
            EExportSubmissionOutcome::Accepted;
    }
    bool ui_recording_committed = false;
    if (prepared_ui->GetRecordingState() ==
        UiFrameGraphPass::ERecordingState::Recorded) {
        if (ui_source_accepted) {
            ui_recording_committed =
                prepared_ui->CommitAcceptedSource();
            independent_submission_source_failed =
                independent_submission_source_failed ||
                !ui_recording_committed;
        } else {
            prepared_ui->RejectSource();
        }
    } else if (prepared_ui->GetRecordingState() ==
                   UiFrameGraphPass::ERecordingState::SourceAccepted ||
               prepared_ui->GetRecordingState() ==
                   UiFrameGraphPass::ERecordingState::FrameAccepted) {
        ui_recording_committed = true;
    } else {
        independent_submission_source_failed = true;
        prepared_ui->RejectSource();
    }
    frame_native_accepted =
        frame_native_accepted && ui_recording_committed;
#if WITH_NRD
    if (nrd_recorded && prepared_nrd) {
        const bool upstream_sources_accepted =
            frame_native_accepted;
        const bool nrd_submission_accepted =
            upstream_sources_accepted &&
            NrdDenoisePass::CommitAcceptedSubmission(
                *prepared_nrd,
                timeline,
                time,
                nrd_graph_submission_value_count
            );
        if (upstream_sources_accepted &&
            !nrd_submission_accepted) {
            independent_submission_source_failed = true;
        }
        frame_native_accepted =
            frame_native_accepted && nrd_submission_accepted;
        if (nrd_submission_accepted) {
            ++state.nrd_time;
            state.nrd_outputs_initialized = true;
        } else {
            // NRD NewFrame/Denoise may already have translated before a later
            // graph source or the final tail was rejected. Renderer temporal
            // state must not pretend that partial frame was accepted; force
            // the next linear frame to CLEAR_AND_RESTART.
            prepared_nrd->interface->ResetAcceptedHistory();
            state.nrd_time                = 0;
            state.nrd_outputs_initialized = false;
            if (upstream_sources_accepted) {
                LOG_CRITICAL(
                    "[NRD] Native submission rejected the NRD source or the "
                    "immutable temporal snapshot could not commit. Resetting "
                    "NRD history; the independent frame failure will latch "
                    "managed RT recording."
                );
            } else {
                LOG_WARNING(
                    "[NRD] Upstream export/frame submission was rejected. "
                    "Resetting NRD history without independently latching the "
                    "renderer."
                );
            }
        }
    }
#endif
    if (frame_native_accepted) {
        const bool ui_frame_committed =
            prepared_ui->CommitAcceptedFrame();
        independent_submission_source_failed =
            independent_submission_source_failed ||
            !ui_frame_committed;
        frame_native_accepted =
            frame_native_accepted && ui_frame_committed;
    }
    const ExportFrameSubmissionDecision export_frame_decision =
        export_submission.ClassifyFrameAcceptance(
            tail_submission_outcome,
            frame_native_accepted,
            independent_submission_source_failed
        );
    frame_native_accepted = export_frame_decision.frame_accepted;
    feedback.export_consumed =
        export_frame_decision.export_consumed;
    if (feedback.export_consumed) {
        state.b_export = false;
    }
    if (!frame_native_accepted) {
        if (export_frame_decision.latch_renderer) {
            state.render_graph_fallback_latched          = true;
            state.render_graph_recording_failure_latched = true;
            LOG_CRITICAL(
                "[RenderGraph][Raytracing] Native submission rejected the "
                "final tail, a managed graph/UI/NRD source, or failed without "
                "an explicit recoverable outcome. Preserving accepted "
                "renderer history and disabling managed recording for this "
                "renderer instance."
            );
        } else {
            // TickFrame rotates the planned ReSTIR reservoir indices before
            // native acceptance is known. A recoverably rejected export frame
            // must not let the next retry treat that planned slice as accepted
            // temporal history. Keep the GBuffer history, but conservatively
            // warm the reservoir ring again on the linear path.
            state.lighting_initialized_reservoir_mask = 0;
            LOG_WARNING(
                "[Raytracing][Export] Export prefix and its dependent tail "
                "were recoverably rejected. Preserving accepted GBuffer "
                "history and the export request, while resetting ReSTIR "
                "reservoir eligibility without latching managed recording."
            );
        }
    }
    const bool frame_setup_accepted =
        frame_native_accepted &&
        (frame_setup_graph_recorded || frame_setup_linear_recorded);
    if (frame_setup_accepted && prepared_frame_setup) {
        state.frame_setup_pass->CommitAccepted(*prepared_frame_setup);
        if (prepared_frame_setup->BuildsTlas()) {
            state.current_tlas_revision = state.rt_instance_revision;
            ++state.renderer_tlas_build_count;
        } else {
            ++state.renderer_tlas_skip_count;
        }
    }
    if (frame_native_accepted && prepared_lights &&
        (linear_lighting_recorded || graph_primary_recorded)) {
        state.prepare_light_pass->CommitAcceptedFrame(*state.rt_ctx, std::move(*prepared_lights));
    }
    if (frame_native_accepted &&
        (linear_antialias_recorded || graph_antialias_recorded)) {
        state.antialias_pass->CommitAcceptedFrame();
        state.b_feedback_valid = true;
    }
    if (frame_native_accepted &&
        (linear_tone_mapping_recorded || graph_tone_mapping_recorded)) {
        state.tone_mapping_pass->CommitAcceptedFrame(
            tone_mapping_elapsed_for_commit, tone_mapping_enabled_for_commit
        );
    }
    if (frame_native_accepted &&
        (linear_gbuffer_recorded || graph_primary_recorded)) {
        state.gbuffer_initialized_history_mask |= gbuffer_history_bit;
    }
    if (frame_native_accepted && linear_lighting_recorded) {
        const uint8 previous_reservoir_mask = state.lighting_initialized_reservoir_mask;
        const auto& buffer_indices          = state.rt_ctx->is_ctx.GetReSTIRDIBufferIndices();
        for (const uint reservoir_slice : {
                 buffer_indices.initial_sample_output_buff_idx,
                 buffer_indices.temperal_resample_output_buff_idx,
                 buffer_indices.spatial_resample_output_buff_idx,
             }) {
            if (reservoir_slice < 3) {
                state.lighting_initialized_reservoir_mask |= uint8(1u << reservoir_slice);
            }
        }
        if (previous_reservoir_mask != uint8(0b111) &&
            state.lighting_initialized_reservoir_mask == uint8(0b111)) {
            LOG_INFO("[RenderGraph][Raytracing] All Lighting reservoir slices have "
                     "accepted linear submissions; active RDG is eligible once "
                     "both GBuffer history pings are initialized.");
        }
    }
    if (frame_native_accepted) {
        if (graph_primary_recorded) {
            state.normal_roughness_readable = true;
        } else if (linear_gbuffer_recorded) {
            state.normal_roughness_readable =
                nrd_recorded || linear_visualize_recorded;
        }
    }
    if (frame_setup_accepted) {
        state.rt_ctx->AdvanceFrame();
    }
    if (frame_setup_accepted && state.rt_scene) {
        // RaytracingScene is a ping-pong builder: the TLAS consumed by this
        // accepted frame becomes GetPrevTlas(), while GetTlas() rotates to the
        // slot that may need rebuilding next frame. Mirror that slot rotation
        // in the revision pair instead of treating the names as immutable
        // temporal roles.
        state.rt_scene->AdvanceFrame();
        std::swap(state.current_tlas_revision, state.previous_tlas_revision);
    }
    if (!skip_present && frame_native_accepted && prepared_ui->CanPresent()) {
        PresentUiDrawFrame(
            prepared_ui->GetDrawFrame(),
            ui_execution_thread
        );
    }

    feedback.profiler_data             = gfx_queue.GetProfilerEntry();
    feedback.renderer_tlas_build_count = state.renderer_tlas_build_count;
    feedback.renderer_tlas_skip_count  = state.renderer_tlas_skip_count;
    feedback.scene_tlas_update_count   = state.scene_tlas_update_count;
    feedback.rt_instance_revision      = state.rt_instance_revision;
    feedback.current_tlas_revision     = state.current_tlas_revision;
    feedback.previous_tlas_revision    = state.previous_tlas_revision;
    feedback.material_texture_names.reserve(state.material_textures.size());
    for (const auto& entry : state.material_textures) {
        feedback.material_texture_names.emplace_back(entry.first);
    }
    return feedback;
}

void RaytracingRenderer::ApplyFrameFeedback(
    RaytracingFrameFeedback feedback,
    RaytracingConfig&       target_config
) {
    assert(IsCurrentlyGameThread());
    TickAndLogProfiling(feedback);
    if (feedback.has_grid_cell_size) {
        target_config.grid_config.cell_size = feedback.grid_cell_size;
    }
    if (feedback.export_request_finished) {
        export_request_in_flight = false;
        if (feedback.export_consumed) {
            target_config.export_cfg.b_export = false;
        }
    }
    debug_ui_profiler_data          = std::move(feedback.profiler_data);
    debug_ui_material_texture_names = std::move(feedback.material_texture_names);
}

void RaytracingRenderer::ApplyFrameFeedback(
    RaytracingFrameFeedback feedback,
    RaytracingConfig&       target_config,
    const EngineHooks&      hooks
) {
    assert(IsCurrentlyGameThread());
    ApplyMainPresentReceipt(feedback.main_present_receipt, hooks);
    ApplyFrameFeedback(std::move(feedback), target_config);
}

bool RaytracingRenderer::RunSingle(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    ApplyFrameFeedback(
        RenderFrame(PrepareFrame(editor_config, hooks)), editor_config->raytracing_config, hooks
    );
    return !hooks.should_reload || !hooks.should_reload();
}

void RaytracingRenderer::Shutdown(const EngineHooks& hooks) {
    assert(IsCurrentlyGameThread());
    if (debug_ui_registered && hooks.on_unregister_ui_func) {
        hooks.on_unregister_ui_func("Display MaterialTexture");
    }
    debug_ui_registered = false;
}

void RaytracingRenderer::EnsureDebugUiRegistered(const EngineHooks& hooks) {
    assert(IsCurrentlyGameThread());
    if (debug_ui_registered || !hooks.on_register_ui_func) {
        return;
    }

    hooks.on_register_ui_func("Display MaterialTexture", [this]() {
        assert(IsCurrentlyGameThread());
        auto& debug_input = debug_ui_frame_input;

        ImGui::Checkbox("Show Final Texture", &debug_input.show_final_texture);
        ImGui::SliderInt("Mip Level", &debug_input.mip_level, 0, 12);
        ImGui::Checkbox("Use Bindless", &debug_input.use_bindless);
        if (ImGui::TreeNode("MaterialTexture")) {
            for (const auto& name : debug_ui_material_texture_names) {
                if (ImGui::Selectable(name.data(), debug_input.selected_material_texture_name == name)) {
                    debug_input.selected_material_texture_name = name;
                }
            }
            ImGui::TreePop();
        }

        const auto& entries = debug_ui_profiler_data;
        if (!entries.cpu_entries.empty()) {
            ImGui::Text("CPU Time:");
            for (const auto& [name, duration] : entries.cpu_entries) {
                if (name.ends_with("Percentage")) {
                    ImGui::Text("%s: %.3f%%", name.c_str(), duration * 100);
                } else {
                    ImGui::Text("%s: %.3f ms", name.c_str(), duration);
                }
            }
        }
        if (!entries.gpu_entries.empty()) {
            ImGui::Text("GPU Time:");
            for (const auto& [name, duration] : entries.gpu_entries) {
                ImGui::Text("%s: %.3f ms", name.c_str(), duration);
            }
        }
    });
    debug_ui_registered = true;
}

bool RaytracingRenderer::RefreshSceneRuntimeRefs() {
    auto&       state            = *runtime_state;
    const auto& gpu_scene_res    = render_scene->GetGpuSceneRes();
    const bool  rt_scene_changed = state.rt_scene.Get() != gpu_scene_res.rt_scene.Get();

    state.rt_scene = gpu_scene_res.rt_scene;
    state.rt_ctx->SetBindlessHandles(gpu_scene_res);
    state.rt_ctx->SetRaytracingScene(state.rt_scene);

    state.material_textures.clear();
    state.material_textures.reserve(gpu_scene_res.texture_array.size());
    for (const TextureWithHandle& texture : gpu_scene_res.texture_array) {
        if (!texture.tex || texture.tex->GetDimension() != ETextureDimension::TEX_2D ||
            texture.tex->GetNumArray() != 1 || texture.tex->GetNumMips() == 0 ||
            texture.tex->GetExtent().x == 0 || texture.tex->GetExtent().y == 0) {
            continue;
        }
        std::string base_name(texture.tex->GetName());
        if (base_name.empty()) {
            base_name = std::format("Material Texture {}", texture.hdl);
        }
        std::string display_name = base_name;
        if (state.material_textures.contains(display_name)) {
            display_name = std::format("{} [{}]", base_name, texture.hdl);
        }
        uint duplicate_index = 2;
        while (state.material_textures.contains(display_name)) {
            display_name = std::format("{} [{}:{}]", base_name, texture.hdl, duplicate_index++);
        }
        state.material_textures.emplace(std::move(display_name), texture);
    }

    if (rt_scene_changed) {
        state.b_feedback_valid                    = false;
        state.current_tlas_revision               = RuntimeState::invalid_tlas_revision;
        state.previous_tlas_revision              = RuntimeState::invalid_tlas_revision;
        state.gbuffer_initialized_history_mask    = 0;
        state.lighting_initialized_reservoir_mask = 0;
        state.normal_roughness_readable           = false;
        state.frame_setup_pass->ResetAcceptedResources();
#if WITH_NRD
        if (state.nrd_interface) {
            state.nrd_interface->ResetAcceptedHistory();
            state.nrd_time = 0;
        }
#endif
        LOG_INFO(
            "[RenderGraph][Raytracing] RT scene identity changed; resetting "
            "FrameSetup acceptance and temporal graph warmup."
        );
    }
    return rt_scene_changed;
}

RaytracingRenderer::SceneUpdateResult RaytracingRenderer::ExecuteSceneUpdates(
    SceneUpdateBatch&                                  batch,
    const GpuScene::PendingCommandListSetupCallback& setup_command_lists
) {
    auto& state            = *runtime_state;
    bool  updated          = false;
    bool  rt_scene_updated = false;
    if (batch.initial_gpu_update) {
        const bool has_rt_update =
            batch.initial_gpu_update->raytracing_update != EGpuSceneRaytracingUpdate::None;
        ExecuteSceneUpdate(
            *render_scene,
            std::move(*batch.initial_gpu_update),
            device,
            gfx_queue,
            setup_command_lists
        );
        updated          = true;
        rt_scene_updated = rt_scene_updated || has_rt_update;
        if (has_rt_update) {
            ++state.rt_instance_revision;
            ++state.scene_tlas_update_count;
        }
    }
    if (batch.update_gpu_update) {
        const bool has_rt_update =
            batch.update_gpu_update->raytracing_update != EGpuSceneRaytracingUpdate::None;
        ExecuteSceneUpdate(
            *render_scene,
            std::move(*batch.update_gpu_update),
            device,
            gfx_queue,
            setup_command_lists
        );
        updated          = true;
        rt_scene_updated = rt_scene_updated || has_rt_update;
        if (has_rt_update) {
            ++state.rt_instance_revision;
            ++state.scene_tlas_update_count;
        }
    }
    const bool rt_scene_replaced = updated && RefreshSceneRuntimeRefs();
    if (rt_scene_updated && state.rt_scene) {
        state.current_tlas_revision = state.rt_instance_revision;
    }
    return SceneUpdateResult{
        .gpu_resources_updated = updated,
        .tlas_built             = rt_scene_updated,
        .rt_scene_replaced      = rt_scene_replaced
    };
}

void RaytracingRenderer::RecreateOutputResources(uint2 new_extent) {
    auto& state = *runtime_state;

    state.output = device.CreateTexture(
        "output",
        Extent2D(new_extent.x, new_extent.y),
        swapchain->format,
        ETextureUsageFlags::COLOR_ATTACHMENT |
            ETextureUsageFlags::PRESENTATION_SOURCE |
            ETextureUsageFlags::TRANSFER_SRC |
            ETextureUsageFlags::TRANSFER_DST
    );
}

bool RaytracingRenderer::RecreateSceneResources(uint2 new_extent) {
    auto& state = *runtime_state;
    assert(!state.render_graph_recording_failure_latched);

    state.rt_ctx->SetResolution(new_extent);

    state.importance_sampling_context.~ImportanceSamplingContext();
    state.importance_sampling_params.render_size = new_extent;
    new (&state.importance_sampling_context) ImportanceSamplingContext(state.importance_sampling_params);

#if WITH_NRD
    state.nrd_interface =
        state.nrd_plugin->RecreateInterface(std::move(state.nrd_interface), new_extent.x, new_extent.y);
    state.nrd_time       = 0;
    state.nrd_mode       = -1;
    state.nrd_was_active = false;
    state.nrd_outputs_initialized = false;
#endif

    state.antialias_pass_info.motion              = state.rt_ctx->frame_rt.motion;
    state.antialias_pass_info.feedback_color_ping = state.rt_ctx->frame_rt.feedback_color_ping;
    state.antialias_pass_info.feedback_color_pong = state.rt_ctx->frame_rt.feedback_color_pong;
    state.antialias_pass   = MakeUnique<AntialiasPass>(device, manager, state.antialias_pass_info);
    state.b_feedback_valid = false;
    state.gbuffer_initialized_history_mask     = 0;
    state.normal_roughness_readable            = false;
    state.lighting_working_set                 = {};
    state.lighting_reservoir_block_array_pitch = 0;
    state.lighting_initialized_reservoir_mask  = 0;
    return true;
}

} // namespace Moer::Render::Raytracing
