#include "RaytracingRenderer.h"

// Runtime
#include "PixelFormat.h"
#include "config/ConfigManager.h"
#include "misc/BoundingBox.h"
#include "misc/Timer.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIExecutor.h"
#include "rhi/RHIResource.h"
#include "rhi/plugin/NrdPlugin.h"
#include "scene/GpuScene.h"
#include "shader/ShaderResourceManager.h"
#include "window/WindowContext.h"

// Renderer
#include "AntiAliasPass.h"
#include "CompositionPass.h"
#include "Configs.h"
#include "GBufferPass.h"
#include "LightingPass.h"
#include "PreprocessLightPass.h"
#include "RTResource.h"
#include "ShaderUtils.h"
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
           << " PreviousTLASRevision=" << feedback.previous_tlas_revision
           << " ConfiguredLocalLightSampling="
           << LocalLightSampleModeName(feedback.configured_local_light_sample_mode)
           << " EffectiveLocalLightSampling="
           << LocalLightSampleModeName(feedback.effective_local_light_sample_mode)
           << " AdaptiveFallback="
           << (feedback.adaptive_local_light_fallback_applied ? "Applied" : "NotApplied")
           << " LocalLights=" << feedback.local_light_count;
    LOG_INFO("{}", stream.str());
}

void ExecuteSceneUpdate(
    RenderScene&     render_scene,
    GpuSceneUpdate&& update,
    RenderDevice&    device,
    CommandQueue&    gfx_queue
) {
    (void)gfx_queue;
    (void)device;
    auto scene_cmd_list = render_scene.ApplyUpdate(std::move(update));
    Array<RHIBackendSubmissionBatchEntry> submissions{};
    submissions.emplace_back(EQueueType::Copy, scene_cmd_list.copy_queue_cmd_list.Submit());
    submissions.emplace_back(
        EQueueType::Graphics,
        scene_cmd_list.gfx_queue_cmd_list.Submit().TickProfiling()
    );
    RHIExecutor::Get().Submit(
        std::move(submissions),
        ERHIExecSubmitFlags::FlushGPU
    );
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
    ImportanceSamplingParams   importance_sampling_params;
    ImportanceSamplingContext importance_sampling_context;

    bool first_load = true;

    UnorderedMap<std::string, TextureWithHandle> material_textures;

    TextureRef output;
    TextureRef ui_frame_buffer;

    Timer timer;

    bool b_feedback_valid = false;
    bool b_export         = false;

    bool                         render_graph_enabled                 = false;
    bool                         render_graph_debug_dump              = false;
    bool                         render_graph_parallel_recording      = false;
    bool                         render_graph_fallback_latched        = false;
    uint8                        gbuffer_initialized_history_mask     = 0;
    bool                         normal_roughness_readable            = false;
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
    UniquePtr<GBufferPass>      g_buffer_pass;
    UniquePtr<LightingPass>     lighting_pass;
    UniquePtr<CompositionPass>  composition_pass;
    UniquePtr<VisualizePass>    visualize_pass;
    UniquePtr<RTContext>        rt_ctx;
    UniquePtr<ToneMappingPass>  tone_mapping_pass;

    AntialiasPass::CreateInfo antialias_pass_info{};
    UniquePtr<AntialiasPass>  antialias_pass;

#if WITH_NRD
    uint64                       nrd_time   = 0ull;
    Ext::NRDPlugin*              nrd_plugin = nullptr;
    UniquePtr<Ext::NRDInterface> nrd_interface;
#endif

    VisualizeConfig visualize_config{};

    explicit RuntimeState(RaytracingRenderer& renderer) :
        shader_utils(renderer.manager),
        importance_sampling_params(CreateImportanceSamplingParams(renderer.resolution)),
        importance_sampling_context(importance_sampling_params),
        output(renderer.device.CreateTexture(
            Extent2D(renderer.resolution.x, renderer.resolution.y),
            renderer.swapchain->format,
            ETextureUsageFlags::COLOR_ATTACHMENT
        )),
        ui_frame_buffer(renderer.device.CreateTexture(
            "ui_frame_buffer",
            Extent2D(renderer.resolution.x, renderer.resolution.y),
            PF_R8G8B8A8_SRGB,
            ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED
        )),
        exported_file_path(ConfigManager::GetInstance().GetWorkspacePath() / "saved"),
        prepare_light_pass(MakeUnique<PrepareLightPass>(renderer.manager, renderer.bindless_array)),
        g_buffer_pass(MakeUnique<GBufferPass>(renderer.device, renderer.manager, renderer.bindless_array)),
        lighting_pass(MakeUnique<LightingPass>(renderer.manager, renderer.bindless_array)),
        composition_pass(
            MakeUnique<CompositionPass>(renderer.device, renderer.manager, renderer.bindless_array)
        ),
        visualize_pass(MakeUnique<VisualizePass>(renderer.device, renderer.manager)),
        rt_ctx(MakeUnique<RTContext>(shader_utils, importance_sampling_context, renderer.bindless_array)) {
        if (!std::filesystem::exists(exported_file_path)) {
            std::filesystem::create_directory(exported_file_path);
        }

        rt_ctx->SetResolution(renderer.resolution);
        antialias_pass_info = AntialiasPass::CreateInfo{
            .motion              = rt_ctx->frame_rt.motion,
            .feedback_color_ping = rt_ctx->frame_rt.feedback_color_ping,
            .feedback_color_pong = rt_ctx->frame_rt.feedback_color_pong
        };
        antialias_pass = MakeUnique<AntialiasPass>(renderer.device, renderer.manager, antialias_pass_info);

#if WITH_NRD
        nrd_plugin    = renderer.device.LoadPlugin<Ext::NRDPlugin>();
        nrd_interface = nrd_plugin->CreateInterface(
            renderer.max_frame_in_flight, renderer.resolution.x, renderer.resolution.y
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
    RuntimeAssets&                runtime_assets
) :
    Renderer(resolution, config),
    runtime_assets(runtime_assets),
    runtime_state(MakeUnique<RuntimeState>(*this)) {
    const auto& graph_config =
        ConfigManager::GetInstance().GetConfig().engine.render.raytracing;
    runtime_state->render_graph_enabled            = graph_config.render_graph;
    runtime_state->render_graph_debug_dump         = graph_config.render_graph_debug_dump;
    runtime_state->render_graph_parallel_recording =
        graph_config.render_graph_parallel_recording;
    LOG_INFO(
        "[RenderGraph] Raytracing execution mode: {}, primary graph recording: {}",
        runtime_state->render_graph_enabled ? "graph-pilot" : "linear",
        runtime_state->render_graph_parallel_recording ? "parallel-eligible" :
                                                         "serial"
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

    auto& state     = *runtime_state;
    auto& ui_config = frame_packet.config;

    if (frame_packet.frame_id == 0) {
        const uint32_t frame_thread_id = IsCurrentlyRenderThread() ? GetRenderThreadId() : GetGameThreadId();
        LOG_INFO(
            "[Threading] Raytracing frames execute on {} thread id = {}",
            IsCurrentlyRenderThread() ? "Render" : "Game",
            frame_thread_id
        );
    }

    PrepareRenderFrame(frame_packet.window);
    bool skip_present                       = false;
    bool split_graph_profiling_frame       = false;
    bool graph_primary_recorded             = false;
    bool graph_composition_recorded         = false;
    bool linear_gbuffer_recorded            = false;
    bool linear_lighting_recorded           = false;
    bool nrd_recorded                       = false;
    uint8 gbuffer_history_bit               = 0;
    LightingPass::LocalLightSamplingDecision local_light_sampling{};

    switch (frame_packet.window.state) {
        case EWindowState::Hiding:
            std::this_thread::yield();
            skip_present = true;
            break;
        case EWindowState::SizeChanged:
            RecreateFrameResources(resolution);
            break;
        case EWindowState::Default:
            break;
        default:
            assert(false);
            break;
    }

    state.timer.Stop();
    [[maybe_unused]] const auto frame_time = state.timer.ElapsedMilliseconds();
    state.timer.Start();

    RaytracingFrameFeedback feedback{};
    feedback.frame_id                = frame_packet.frame_id;
    feedback.export_request_finished = ui_config.export_cfg.b_export;

    if (frame_packet.scene_updates.scene_ready && frame_packet.runtime_assets_ready) {
        const bool render_graph_boundary_frame =
            state.first_load || state.b_new_env_map ||
            frame_packet.window.state == EWindowState::SizeChanged ||
            ui_config.export_cfg.b_export;
        const bool nrd_active_this_frame =
            IsNrdDenoiserActive(ui_config.denoiser_cfg.denoiser_type);
        const bool scene_tlas_updated =
            ExecuteSceneUpdates(frame_packet.scene_updates);

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

            RefreshSceneRuntimeRefs();
            state.rt_ctx->FillLowDiscrepancySequence(cmd_list);
            cmd_list.UpdateBindlessArray(bindless_array);
            RHIExecutor::Get().Submit(
                EQueueType::Graphics, cmd_list.Submit(), ERHIExecSubmitFlags::FlushGPU
            );
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        }

        ScopedGpuMarker renderer_marker(
            cmd_list, "Raytracing Renderer", GpuMarkerPalette::Renderer()
        );

        if (state.b_new_env_map) {
            ScopedGpuMarker environment_marker(
                cmd_list, "Pass: Environment Setup", GpuMarkerPalette::Pass()
            );
            auto src_env_map = runtime_assets.GetDefaultEnvMap();
            state.env_map    = device.CreateTexture(
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

        bool tlas_built_this_frame = scene_tlas_updated;
        if (state.rt_scene) {
            const bool needs_tlas_build = IsLegacyTlasUpdateEnabled() ||
                                          state.current_tlas_revision != state.rt_instance_revision;
            if (needs_tlas_build) {
                ScopedGpuMarker tlas_marker(
                    cmd_list, "Pass: Scene Acceleration Structure", GpuMarkerPalette::Pass()
                );
                for (size_t i = 0; i < state.rt_scene->GetInstanceCount(); i++) {
                    auto& instance = state.rt_scene->GetInstance(i);
                    state.rt_scene->MarkModified(instance.instance_id);
                }
                cmd_list.PushScopeWithTimeScope("RTScene RendererTLAS");
                cmd_list.UpdateRaytracingScene(state.rt_scene);
                cmd_list.PopScopeWithTimeScope();
                state.current_tlas_revision = state.rt_instance_revision;
                ++state.renderer_tlas_build_count;
                tlas_built_this_frame = true;
            } else {
                ++state.renderer_tlas_skip_count;
            }
        }

        ToneMappingPass::Params tone_params{};
        AntialiasPass::Params   aa_params{};
        {
            auto grid_cfg      = state.importance_sampling_context.GetGridChangeableConfig();
            grid_cfg.cell_size = ui_config.grid_config.cell_size;
            grid_cfg.center    = camera.GetPosition();

            auto grid_static_cfg           = state.importance_sampling_context.GetGridConfig();
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
            state.rt_ctx->config.grid_min_local_light_count = static_cast<uint>(std::max(
                ui_config.restir_di_cfg.initial_sample_config.grid_min_local_light_count, 1
            ));
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

        static constexpr uint s_mesh_alloc_chunk      = 128;
        static constexpr uint s_triangle_alloc_chunk  = 1024;
        static constexpr uint s_primitive_alloc_chunk = 128;
        const auto&           scene_snapshot          = frame_packet.scene_snapshot;
        state.rt_ctx->CreateBuffersIfNeeded(
            (scene_snapshot.emissive_instance_count + s_mesh_alloc_chunk - 1) & ~(s_mesh_alloc_chunk - 1),
            (scene_snapshot.emissive_triangle_count + s_triangle_alloc_chunk - 1) &
                ~(s_triangle_alloc_chunk - 1),
            (scene_snapshot.light_count + s_primitive_alloc_chunk - 1) & ~(s_primitive_alloc_chunk - 1),
            scene_snapshot.primitive_count
        );
        cmd_list.UpdateBindlessArray(bindless_array);

        state.rt_ctx->Tick(camera, state.antialias_pass->GetPixelOffset());
        gbuffer_history_bit = state.rt_ctx->b_current_frame ? uint8(1) : uint8(2);
        const std::array<const Buffer*, 3> lighting_working_set{
            state.rt_ctx->light_reservoir_buf.Get(),
            state.rt_ctx->ris_buf.Get(),
            state.rt_ctx->ris_light_data_buf.Get()
        };
        const uint lighting_reservoir_block_array_pitch =
            state.rt_ctx->is_ctx.GetReSTIRDIRuntimeConfig()
                .reservoir_buffer_params.block_array_pitch;
        if (state.lighting_working_set != lighting_working_set ||
            state.lighting_reservoir_block_array_pitch !=
                lighting_reservoir_block_array_pitch) {
            state.lighting_working_set = lighting_working_set;
            state.lighting_reservoir_block_array_pitch =
                lighting_reservoir_block_array_pitch;
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
        {
            ScopedGpuMarker pass_marker(
                cmd_list, "Pass: Prepare Lights", GpuMarkerPalette::Pass()
            );
            state.prepare_light_pass->Process(cmd_list, *state.rt_ctx, scene_snapshot);
        }
        if (state.render_graph_enabled && !state.render_graph_fallback_latched &&
            state.gbuffer_initialized_history_mask == uint8(0b11) &&
            state.lighting_initialized_reservoir_mask == uint8(0b111) &&
            !render_graph_boundary_frame) {
            RenderGraph graph(
                nrd_active_this_frame ?
                    "Raytracing.PreDenoise" :
                    "Raytracing.FrameCore"
            );
            const RTGraphFrameResources graph_resources =
                RegisterRTGraphFrameResources(graph, *state.rt_ctx);
            const bool gbuffer_added = state.g_buffer_pass->AddPasses(
                    graph,
                    graph_resources,
                    *state.rt_ctx,
                    tlas_built_this_frame,
                    state.normal_roughness_readable
                );
            const bool lighting_added =
                gbuffer_added &&
                state.lighting_pass->AddPasses(
                    graph,
                    graph_resources,
                    *state.rt_ctx,
                    local_light_sampling
                );
            const bool composition_in_graph = !nrd_active_this_frame;
            const bool composition_added =
                !composition_in_graph ||
                (lighting_added &&
                 state.composition_pass->AddPasses(
                     graph,
                     graph_resources,
                     *state.rt_ctx
                 ));
            if (!gbuffer_added || !lighting_added || !composition_added) {
                state.render_graph_fallback_latched = true;
                LOG_ERROR(
                    "[RenderGraph][Fallback] Raytracing primary graph could not "
                    "import a required GBuffer/Lighting/Composition resource. "
                    "Using the linear path for this renderer instance."
                );
            } else if (!graph.Compile()) {
                state.render_graph_fallback_latched = true;
                LOG_ERROR(
                    "[RenderGraph][Fallback] Raytracing primary graph compile "
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

                // The caller-owned prefix and managed graph command lists are
                // distinct submissions. Close every scope before sealing the
                // prefix; the frame tail will finish the profiling transaction.
                renderer_marker.Close();
                if (!cmd_list.IsEmpty()) {
                    CmdSubmit prefix_submit =
                        cmd_list.Submit().DebugLabel(
                            std::format(
                                "Raytracing Frame {}/Graph Prefix",
                                frame_packet.frame_id
                            ),
                            GpuMarkerPalette::Renderer()
                        );
                    prefix_submit.SetProfilingPhase(
                        split_graph_profiling_frame ?
                            ERHIProfilingPhase::Continue :
                            ERHIProfilingPhase::Begin
                    );
                    split_graph_profiling_frame = true;
                    RHIExecutor::Get().Submit(
                        EQueueType::Graphics,
                        std::move(prefix_submit),
                        ERHIExecSubmitFlags::None
                    );
                }

                const auto configure_recording_source =
                    [&](const RenderGraph::ExecutedPassInfo& pass,
                        RHIRecordingSource&                  source) {
                        source.submit_metadata.debug_label = std::format(
                            "Raytracing Frame {}/{}",
                            frame_packet.frame_id,
                            pass.name
                        );
                        source.submit_metadata.debug_label_color =
                            GpuMarkerPalette::Pass();
                        source.submit_metadata.profiling_phase =
                            split_graph_profiling_frame ?
                                ERHIProfilingPhase::Continue :
                                ERHIProfilingPhase::Begin;
                        split_graph_profiling_frame = true;
                    };
                if (graph.ExecuteRecording(
                        {},
                        configure_recording_source,
                        state.render_graph_parallel_recording,
                        {},
                        RenderGraph::ActiveRecordingOptions{.enabled = true}
                    )) {
                    graph_primary_recorded     = true;
                    graph_composition_recorded = composition_in_graph;
                } else {
                    state.render_graph_fallback_latched = true;
                    LOG_ERROR(
                        "[RenderGraph][Fallback] Raytracing primary graph "
                        "recording failed before transaction commit: {}. "
                        "Re-recording its managed passes linearly this frame and "
                        "using the linear path from the next frame.",
                        graph.GetCompileError()
                    );
                }
            }
        }
        if (!graph_primary_recorded) {
            ScopedGpuMarker pass_marker(
                cmd_list, "Pass: GBuffer", GpuMarkerPalette::Pass()
            );
            state.g_buffer_pass->Process(cmd_list, *state.rt_ctx);
            linear_gbuffer_recorded = true;

            pass_marker.Close();
            ScopedGpuMarker lighting_marker(
                cmd_list, "Pass: Lighting", GpuMarkerPalette::Pass()
            );
            local_light_sampling =
                state.lighting_pass->Process(cmd_list, *state.rt_ctx);
            linear_lighting_recorded = true;
        } else {
            state.g_buffer_pass->RecordLegacyTailBridge(
                cmd_list,
                *state.rt_ctx,
                graph_composition_recorded
            );
        }
        feedback.configured_local_light_sample_mode = local_light_sampling.configured_mode;
        feedback.effective_local_light_sample_mode  = local_light_sampling.effective_mode;
        feedback.adaptive_local_light_fallback_applied =
            local_light_sampling.adaptive_fallback_applied;
        feedback.local_light_count = local_light_sampling.local_light_count;

#if WITH_NRD
        {
            const bool    b_current_frame = state.rt_ctx->b_current_frame;
            const auto&   frame_rt        = state.rt_ctx->frame_rt;
            nrd::Denoiser denoiser        = nrd::Denoiser::MAX_NUM;
            switch (ui_config.denoiser_cfg.denoiser_type) {
                case s_denoiser_mode_reblur:
                    denoiser = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
                    break;
                case s_denoiser_mode_relax:
                    denoiser = nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
                    break;
            }

            if (denoiser != nrd::Denoiser::MAX_NUM) {
                ScopedGpuMarker denoiser_marker(
                    cmd_list, "Pass: Radiance Denoising", GpuMarkerPalette::Pass()
                );
                state.nrd_interface->Begin();
                state.nrd_interface->UpdateCommonSettings(
                    state.nrd_time++,
                    Vector2ui(resolution.x, resolution.y),
                    state.antialias_pass->GetPixelOffset(),
                    Transpose(camera.GetViewMatrix()),
                    Transpose(camera.GetProjectionMatrix())
                );
                state.nrd_interface->SetInput(
                    Ext::NRDInterface::EResourceSlot::MOTION_VECTOR, state.rt_ctx->frame_rt.motion
                );
                state.nrd_interface->SetInput(
                    Ext::NRDInterface::EResourceSlot::NORMAL_ROUGHNESS, frame_rt.normal_roughness
                );
                state.nrd_interface->SetInput(
                    Ext::NRDInterface::EResourceSlot::VIEW_Z,
                    b_current_frame ? frame_rt.view_depth : frame_rt.prev_view_depth
                );
                state.nrd_interface->SetInput(
                    Ext::NRDInterface::EResourceSlot::IN_DIFFUSE, frame_rt.diffuse_lighting
                );
                state.nrd_interface->SetInput(
                    Ext::NRDInterface::EResourceSlot::IN_SPECULAR, frame_rt.specular_lighting
                );
                state.nrd_interface->SetOutput(
                    Ext::NRDInterface::EResourceSlot::OUT_DIFFUSE, frame_rt.denoised_diffuse_lighting
                );
                state.nrd_interface->SetOutput(
                    Ext::NRDInterface::EResourceSlot::OUT_SPECULAR, frame_rt.denoised_specular_lighting
                );
                state.nrd_interface->Denoise(cmd_list, denoiser, "Radiance Denoising");
                nrd_recorded = true;
            }
        }
#endif

        if (!graph_composition_recorded) {
            ScopedGpuMarker pass_marker(
                cmd_list, "Pass: Composition", GpuMarkerPalette::Pass()
            );
            state.composition_pass->Process(cmd_list, *state.rt_ctx);
        }
        {
            ScopedGpuMarker pass_marker(
                cmd_list, "Pass: Anti Aliasing", GpuMarkerPalette::Pass()
            );
            state.antialias_pass->Process(
                cmd_list,
                aa_params,
                state.b_feedback_valid,
                state.rt_ctx->frame_rt.hdr_color,
                state.rt_ctx->frame_rt.resolved_color
            );
        }
        {
            ScopedGpuMarker pass_marker(
                cmd_list, "Pass: Tone Mapping", GpuMarkerPalette::Pass()
            );
            state.tone_mapping_pass->Process(
                cmd_list,
                tone_params,
                state.rt_ctx->frame_rt.resolved_color,
                state.rt_ctx->frame_rt.ldr_color
            );
        }
        {
            ScopedGpuMarker pass_marker(
                cmd_list, "Pass: Visualize", GpuMarkerPalette::Pass()
            );
            state.visualize_pass->Process(
                cmd_list, *state.rt_ctx, state.visualize_config, bindless_array
            );
        }

        const auto& debug_input = frame_packet.debug_input;
        if (debug_input.show_final_texture &&
            state.material_textures.contains(debug_input.selected_material_texture_name)) {
            ScopedGpuMarker debug_texture_marker(
                cmd_list, "Pass: Debug Texture", GpuMarkerPalette::Pass()
            );
            TextureRef texture = state.material_textures[debug_input.selected_material_texture_name].tex;
            ShowTextureParams show_texture_params{};
            show_texture_params.dst_dim = resolution;
            show_texture_params.bdls_handle =
                state.material_textures[debug_input.selected_material_texture_name].hdl;
            show_texture_params.mip_level    = static_cast<uint>(debug_input.mip_level);
            show_texture_params.use_bindless = debug_input.use_bindless;
            state.shader_utils.ShowTexture(
                cmd_list, bindless_array, show_texture_params, texture, state.rt_ctx->frame_rt.ldr_color
            );
        }

        state.rt_ctx->AdvanceFrame();
        state.tone_mapping_pass->AdvanceFrame(camera.GetDeltaTime());
        state.antialias_pass->AdvanceFrame();
        state.b_feedback_valid = true;

        if (state.b_export) {
            renderer_marker.Close();
            RHIExecutor::Get().Submit(
                EQueueType::Graphics,
                cmd_list.Submit()
                    .DebugLabel(
                        std::format("Raytracing Export Frame {}", frame_packet.frame_id),
                        GpuMarkerPalette::Frame()
                    )
                    .TickProfiling(),
                ERHIExecSubmitFlags::FlushGPU
            );
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
            DumpTextureToFile(
                ui_config.export_cfg,
                state.rt_ctx->frame_rt,
                device,
                gfx_queue,
                state.exported_file_path,
                std::to_string(time)
            );
            state.b_export           = false;
            feedback.export_consumed = true;
        }

    }

    const TextureRef final_color = frame_packet.debug_input.show_final_texture ?
                                       state.rt_ctx->frame_rt.ldr_color :
                                       state.rt_ctx->frame_rt.debug_color;

    if (frame_packet.ui_composition.enabled) {
        const auto& ui_frame = frame_packet.ui_composition;
        const auto  window_frame_buffer =
            ui_frame.window_frame_buffer ? ui_frame.window_frame_buffer->GetView() : TextureView();
        ui_combine_pass->Process(
            cmd_list,
            ui_frame.separate_window,
            ui_frame.output_resolution,
            ui_frame.scene_color_position,
            ui_frame.scene_color_resolution,
            window_frame_buffer,
            final_color,
            state.ui_frame_buffer,
            state.output
        );
    } else {
        ui_combine_pass->Process(
            cmd_list,
            true,
            resolution,
            float2(0.f, 0.f),
            float2(static_cast<float>(resolution.x), static_cast<float>(resolution.y)),
            TextureView(state.output),
            state.rt_ctx->frame_rt.ldr_color,
            {},
            state.output
        );
    }

    const auto ui_execution_thread =
        IsCurrentlyRenderThread() ? EUiDrawExecutionThread::Render : EUiDrawExecutionThread::Game;
    RenderUiDrawFrame(cmd_list, state.output->GetView(), frame_packet.ui_draw_frame, ui_execution_thread);

    if (state.rt_scene) {
        state.rt_scene->AdvanceFrame();
        std::swap(state.current_tlas_revision, state.previous_tlas_revision);
    }

    time++;
    CmdSubmit frame_submit =
        cmd_list.Submit()
            .DebugLabel(
                std::format("Raytracing Frame {}", frame_packet.frame_id),
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
        const auto output_view = state.output->GetView();
        if (frame_packet.window.state == EWindowState::SizeChanged) {
            LOG_INFO(
                "[Threading][Resize] Raytracing main present source={}x{}, swapchain={}x{}, UI platform viewports={}.",
                output_view.extent.x,
                output_view.extent.y,
                swapchain->size.x,
                swapchain->size.y,
                frame_packet.ui_draw_frame.platform_viewports.size()
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
    if (linear_gbuffer_recorded || graph_primary_recorded) {
        state.gbuffer_initialized_history_mask |= gbuffer_history_bit;
    }
    if (linear_lighting_recorded) {
        const uint8 previous_reservoir_mask =
            state.lighting_initialized_reservoir_mask;
        const auto& buffer_indices =
            state.rt_ctx->is_ctx.GetReSTIRDIBufferIndices();
        for (const uint reservoir_slice : {
                 buffer_indices.initial_sample_output_buff_idx,
                 buffer_indices.temperal_resample_output_buff_idx,
                 buffer_indices.spatial_resample_output_buff_idx,
             }) {
            if (reservoir_slice < 3) {
                state.lighting_initialized_reservoir_mask |=
                    uint8(1u << reservoir_slice);
            }
        }
        if (previous_reservoir_mask != uint8(0b111) &&
            state.lighting_initialized_reservoir_mask == uint8(0b111)) {
            LOG_INFO(
                "[RenderGraph][Raytracing] All Lighting reservoir slices have "
                "accepted linear submissions; active RDG is eligible once "
                "both GBuffer history pings are initialized."
            );
        }
    }
    if (graph_primary_recorded) {
        state.normal_roughness_readable = true;
    } else if (linear_gbuffer_recorded) {
        state.normal_roughness_readable = nrd_recorded;
    }
    if (!skip_present) {
        PresentUiDrawFrame(frame_packet.ui_draw_frame, ui_execution_thread);
    }

    feedback.profiler_data = gfx_queue.GetProfilerEntry();
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

void RaytracingRenderer::RefreshSceneRuntimeRefs() {
    auto&       state            = *runtime_state;
    const auto& gpu_scene_res    = render_scene->GetGpuSceneRes();
    const bool  rt_scene_changed = state.rt_scene.Get() != gpu_scene_res.rt_scene.Get();

    state.rt_scene = gpu_scene_res.rt_scene;
    state.rt_ctx->SetBindlessHandles(gpu_scene_res);
    state.rt_ctx->SetRaytracingScene(state.rt_scene);
    if (rt_scene_changed) {
        state.b_feedback_valid = false;
        state.current_tlas_revision  = RuntimeState::invalid_tlas_revision;
        state.previous_tlas_revision = RuntimeState::invalid_tlas_revision;
    }
}

bool RaytracingRenderer::ExecuteSceneUpdates(SceneUpdateBatch& batch) {
    auto& state = *runtime_state;
    bool  updated = false;
    bool  rt_scene_updated = false;
    if (batch.initial_gpu_update) {
        const bool has_rt_update = batch.initial_gpu_update->raytracing_update !=
                                   EGpuSceneRaytracingUpdate::None;
        ExecuteSceneUpdate(*render_scene, std::move(*batch.initial_gpu_update), device, gfx_queue);
        updated          = true;
        rt_scene_updated = rt_scene_updated || has_rt_update;
        if (has_rt_update) {
            ++state.rt_instance_revision;
            ++state.scene_tlas_update_count;
        }
    }
    if (batch.update_gpu_update) {
        const bool has_rt_update = batch.update_gpu_update->raytracing_update !=
                                   EGpuSceneRaytracingUpdate::None;
        ExecuteSceneUpdate(*render_scene, std::move(*batch.update_gpu_update), device, gfx_queue);
        updated          = true;
        rt_scene_updated = rt_scene_updated || has_rt_update;
        if (has_rt_update) {
            ++state.rt_instance_revision;
            ++state.scene_tlas_update_count;
        }
    }
    if (updated) {
        RefreshSceneRuntimeRefs();
    }
    if (rt_scene_updated && state.rt_scene) {
        state.current_tlas_revision = state.rt_instance_revision;
    }
    return rt_scene_updated;
}

void RaytracingRenderer::RecreateFrameResources(uint2 new_extent) {
    auto& state = *runtime_state;

    state.output = device.CreateTexture(
        "output",
        Extent2D(new_extent.x, new_extent.y),
        swapchain->format,
        ETextureUsageFlags::COLOR_ATTACHMENT
    );
    state.ui_frame_buffer = device.CreateTexture(
        "ui_frame_buffer",
        Extent2D(new_extent.x, new_extent.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED
    );

    state.rt_ctx->SetResolution(new_extent);

    state.importance_sampling_context.~ImportanceSamplingContext();
    state.importance_sampling_params.render_size = new_extent;
    new (&state.importance_sampling_context) ImportanceSamplingContext(state.importance_sampling_params);

#if WITH_NRD
    state.nrd_interface =
        state.nrd_plugin->RecreateInterface(std::move(state.nrd_interface), new_extent.x, new_extent.y);
#endif

    state.antialias_pass_info.motion              = state.rt_ctx->frame_rt.motion;
    state.antialias_pass_info.feedback_color_ping = state.rt_ctx->frame_rt.feedback_color_ping;
    state.antialias_pass_info.feedback_color_pong = state.rt_ctx->frame_rt.feedback_color_pong;
    state.antialias_pass   = MakeUnique<AntialiasPass>(device, manager, state.antialias_pass_info);
    state.b_feedback_valid                       = false;
    state.gbuffer_initialized_history_mask       = 0;
    state.normal_roughness_readable              = false;
    state.lighting_working_set                   = {};
    state.lighting_reservoir_block_array_pitch = 0;
    state.lighting_initialized_reservoir_mask    = 0;
}

} // namespace Moer::Render::Raytracing
