#include "RaytracingRenderer.h"

// Runtime
#include "PixelFormat.h"
#include "config/ConfigManager.h"
#include "misc/BoundingBox.h"
#include "misc/Timer.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
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
#include <new>
#include <thread>

namespace Moer::Render::Raytracing {

namespace {

ImportantSamplingParams CreateImportanceSamplingParams(uint2 resolution) {
    ImportantSamplingParams params{};
    params.render_size = resolution;
    return params;
}

void ExecuteSceneUpdate(
    RenderScene&     render_scene,
    GpuSceneUpdate&& update,
    RenderDevice&    device,
    CommandQueue&    gfx_queue
) {
    auto scene_cmd_list = render_scene.ApplyUpdate(std::move(update));
    auto copy_event     = device.GetCopyQueue().Execute(scene_cmd_list.copy_queue_cmd_list.Submit());
    device.GetCopyQueue().Sync(copy_event.timeline);
    gfx_queue.Execute(scene_cmd_list.gfx_queue_cmd_list.Submit().TickProfiling());
    gfx_queue.Sync();
}

} // namespace

struct RaytracingRenderer::RuntimeState {
    bool b_new_env_map = false;

    TextureRef         env_map{};
    Array<TextureView> env_mips;
    TextureWithHandle  env_tex_with_hdl;

    RaytracingSceneRef rt_scene{};

    ShaderUtils               shader_utils;
    ImportantSamplingParams   importance_sampling_params;
    ImportanceSamplingContext importance_sampling_context;

    bool first_load = true;

    UnorderedMap<std::string, TextureWithHandle> material_textures;

    TextureRef output;
    TextureRef ui_frame_buffer;

    Timer timer;

    bool        b_feedback_valid               = false;
    bool        b_export                       = false;
    std::string selected_material_texture_name = "";
    bool        b_final_show_texture           = false;
    bool        b_use_bindless                 = true;
    uint        mip_level                      = 0;

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
        shader_utils(renderer.device, renderer.manager),
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
            .feedback_color_pong = rt_ctx->frame_rt.feedback_color_pong,
            .resolved_color      = rt_ctx->frame_rt.resolved_color,
            .hdr_color           = rt_ctx->frame_rt.hdr_color
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
    const EngineHooks&            hooks,
    RuntimeAssets&                runtime_assets
) :
    Renderer(resolution, config, hooks),
    runtime_assets(runtime_assets),
    runtime_state(MakeUnique<RuntimeState>(*this)) {}

RaytracingRenderer::~RaytracingRenderer() {
    LOG_INFO("[Threading] RaytracingRenderer destruction started on Game Thread.");

    if (runtime_state && runtime_state->rt_ctx) {
        for (const uint buffer : runtime_state->rt_ctx->GetAllocatedBdlsBuf()) {
            bindless_array->UnbindBuffer(buffer);
        }
        for (const uint texture : runtime_state->rt_ctx->GetAllocatedBdlsTex()) {
            bindless_array->UnbindTexture(texture);
        }
    }

    ReleaseResources();
    LOG_INFO("[Threading] RaytracingRenderer destruction finished on Game Thread.");
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

    LogSceneLoadStatus(*editor_config);

    RaytracingFramePacket frame_packet{};
    frame_packet.frame_id = next_frame_id++;
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

    frame_packet.config               = editor_config->raytracing_config;
    frame_packet.camera_input         = CameraFrameInput::Capture(*editor_config);
    frame_packet.runtime_assets_ready = runtime_assets.IsReady();

    if (scene.IsReady() && frame_packet.runtime_assets_ready) {
        const auto light_entity = scene.GetMainDirectionalLightEntity();
        if (light_entity != entt::null && scene.r().valid(light_entity) &&
            scene.r().all_of<ecs::CLightDirectional, ecs::CNode>(light_entity)) {
            scene.Patch<ecs::CLightDirectional>(light_entity, [&](auto& light) {
                light.color     = float3(0.9f, 0.65f, 0.4f);
                light.intensity = frame_packet.config.exposure;
            });
            scene.Patch<ecs::CNode>(light_entity, [&](auto& node) {
                node.rotation = Quaternion(float3(0.f, 0.f, -1.f), -frame_packet.config.sun_direction);
            });
        }

        frame_packet.scene_updates  = scene.PrepareUpdateBatch(false, capture_scene_geometry_snapshot);
        frame_packet.scene_snapshot = CaptureRaytracingSceneFrameSnapshot(scene);
        if (frame_packet.scene_updates.geometry) {
            capture_scene_geometry_snapshot = false;
        }
        if (frame_packet.scene_updates.scene_ready) {
            EnsureDebugUiRegistered(hooks);
        }
    }

    if (hooks.on_capture_ui_composition) {
        frame_packet.ui_composition = hooks.on_capture_ui_composition();
    }
    if (hooks.on_capture_ui_draw_frame) {
        frame_packet.ui_draw_frame = hooks.on_capture_ui_draw_frame();
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

    return frame_packet;
}

void RaytracingRenderer::RenderFrame(RaytracingFramePacket frame_packet) {
    assert(IsCurrentlyGameThread());
    assert(runtime_state);

    auto& state     = *runtime_state;
    auto& ui_config = frame_packet.config;

    PrepareRenderFrame(frame_packet.window);
    bool skip_present = false;

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
    feedback.frame_id = frame_packet.frame_id;

    if (frame_packet.scene_updates.scene_ready && frame_packet.runtime_assets_ready) {
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
            gfx_queue.Execute(cmd_list.Submit());
            gfx_queue.Sync();
        }

        if (state.b_new_env_map) {
            auto src_env_map = runtime_assets.GetDefaultEnvMap();
            state.env_map    = device.CreateTexture(
                src_env_map->GetName(),
                Extent3D(src_env_map->GetExtent()),
                PF_R16G16B16A16_SFLOAT,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                src_env_map->GetNumMips()
            );
            state.shader_utils.SampleTextureCS(
                cmd_list, src_env_map->GetView(0, 1), state.env_map->GetView(0, 1), src_env_map->GetFormat()
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
        camera.Tick(frame_packet.camera_input);
        state.b_export = ui_config.export_cfg.b_export;

        if (state.rt_scene) {
            for (size_t i = 0; i < state.rt_scene->GetInstanceCount(); i++) {
                auto& instance = state.rt_scene->GetInstance(i);
                state.rt_scene->MarkModified(instance.instance_id);
            }
            cmd_list.UpdateRaytracingScene(state.rt_scene);
        }

        ToneMappingPass::Params tone_params{};
        AntialiasPass::Params   aa_params{};
        {
            auto grid_cfg      = state.importance_sampling_context.GetGridChangableConfig();
            grid_cfg.cell_size = ui_config.grid_config.cell_size;
            grid_cfg.center    = camera.GetPosition();

            auto grid_static_cfg           = state.importance_sampling_context.GetGridConfig();
            grid_static_cfg.light_per_ceil = ui_config.grid_config.light_per_ceil;
            grid_static_cfg.grid_mode      = ui_config.grid_config.grid_mode;
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

            state.importance_sampling_context.SetReSTIRDIIInitialSampleParams(di_initial_sample_config);
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
        state.prepare_light_pass->Process(cmd_list, *state.rt_ctx, scene_snapshot);
        state.g_buffer_pass->Process(cmd_list, *state.rt_ctx);
        state.lighting_pass->Process(cmd_list, *state.rt_ctx);

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
            }
        }
#endif

        state.composition_pass->Process(cmd_list, *state.rt_ctx);
        state.antialias_pass->Process(
            cmd_list,
            *state.rt_ctx,
            aa_params,
            state.b_feedback_valid,
            state.rt_ctx->frame_rt.hdr_color,
            state.rt_ctx->frame_rt.resolved_color
        );
        state.tone_mapping_pass->Process(
            cmd_list,
            *state.rt_ctx,
            tone_params,
            state.rt_ctx->frame_rt.resolved_color,
            state.rt_ctx->frame_rt.ldr_color
        );
        state.visualize_pass->Process(cmd_list, *state.rt_ctx, state.visualize_config, bindless_array);

        if (state.b_final_show_texture &&
            state.material_textures.contains(state.selected_material_texture_name)) {
            TextureRef        texture = state.material_textures[state.selected_material_texture_name].tex;
            ShowTextureParams show_texture_params{};
            show_texture_params.dst_dim = resolution;
            show_texture_params.bdls_handle =
                state.material_textures[state.selected_material_texture_name].hdl;
            show_texture_params.mip_level    = state.mip_level;
            show_texture_params.use_bindless = state.b_use_bindless;
            state.shader_utils.ShowTexture(
                cmd_list, bindless_array, show_texture_params, texture, state.rt_ctx->frame_rt.ldr_color
            );
        }

        state.rt_ctx->AdvanceFrame();
        state.tone_mapping_pass->AdvanceFrame(camera.GetDeltaTime());
        state.antialias_pass->AdvanceFrame();
        state.b_feedback_valid = true;

        if (state.b_export) {
            gfx_queue.Execute(cmd_list.Submit().TickProfiling());
            gfx_queue.Sync();
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

        feedback.has_main_camera = true;
        feedback.main_camera     = camera;
    }

    const TextureRef final_color =
        state.b_final_show_texture ? state.rt_ctx->frame_rt.ldr_color : state.rt_ctx->frame_rt.debug_color;

    if (frame_packet.ui_composition.enabled) {
        const auto& ui_frame = frame_packet.ui_composition;
        ui_combine_pass->Process(
            cmd_list,
            ui_frame.separate_window,
            ui_frame.output_resolution,
            ui_frame.scene_color_position,
            ui_frame.scene_color_resolution,
            ui_frame.window_frame_buffer,
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

    RenderUiDrawFrame(
        cmd_list, state.output->GetView(), frame_packet.ui_draw_frame, EUiDrawExecutionThread::Game
    );

    if (state.rt_scene) {
        state.rt_scene->AdvanceFrame();
    }

    time++;
    gfx_queue.Execute(cmd_list.Submit().Signal(timeline, time).DeleteResources().TickProfiling());
    if (!skip_present) {
        gfx_queue.Present(swapchain, state.output);
        PresentUiDrawFrame(frame_packet.ui_draw_frame, EUiDrawExecutionThread::Game);
    }

    latest_frame_feedback = std::move(feedback);
}

void RaytracingRenderer::ApplyFrameFeedback(RaytracingConfig& target_config) {
    assert(IsCurrentlyGameThread());
    if (!latest_frame_feedback) {
        return;
    }

    if (latest_frame_feedback->has_grid_cell_size) {
        target_config.grid_config.cell_size = latest_frame_feedback->grid_cell_size;
    }
    if (latest_frame_feedback->export_consumed) {
        target_config.export_cfg.b_export = false;
    }
    if (latest_frame_feedback->has_main_camera && scene.IsReady()) {
        scene.GetMainCamera().camera = latest_frame_feedback->main_camera;
    }
    latest_frame_feedback.reset();
}

bool RaytracingRenderer::RunSingle(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    RenderFrame(PrepareFrame(editor_config, hooks));
    ApplyFrameFeedback(editor_config->raytracing_config);
    return !hooks.on_is_need_reload || !hooks.on_is_need_reload();
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
        auto& state = *runtime_state;

        ImGui::Checkbox("Show Final Texture", &state.b_final_show_texture);
        ImGui::SliderInt("Mip Level", reinterpret_cast<int*>(&state.mip_level), 0, 12);
        ImGui::Checkbox("Use Bindless", &state.b_use_bindless);
        if (ImGui::TreeNode("MaterialTexture")) {
            for (auto& [name, texture] : state.material_textures) {
                if (ImGui::Selectable(name.data(), state.selected_material_texture_name == name)) {
                    state.selected_material_texture_name = name;
                }
            }
            ImGui::TreePop();
        }

        const auto entries = gfx_queue.GetProfilerEntry();
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
    }
}

void RaytracingRenderer::ExecuteSceneUpdates(SceneUpdateBatch& batch) {
    bool updated = false;
    if (batch.initial_gpu_update) {
        ExecuteSceneUpdate(*render_scene, std::move(*batch.initial_gpu_update), device, gfx_queue);
        updated = true;
    }
    if (batch.update_gpu_update) {
        ExecuteSceneUpdate(*render_scene, std::move(*batch.update_gpu_update), device, gfx_queue);
        updated = true;
    }
    if (updated) {
        RefreshSceneRuntimeRefs();
    }
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
    state.antialias_pass_info.resolved_color      = state.rt_ctx->frame_rt.resolved_color;
    state.antialias_pass_info.hdr_color           = state.rt_ctx->frame_rt.hdr_color;
    state.antialias_pass   = MakeUnique<AntialiasPass>(device, manager, state.antialias_pass_info);
    state.b_feedback_valid = false;
}

} // namespace Moer::Render::Raytracing
