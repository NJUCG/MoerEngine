#include "RaytracingRenderer.h"

#include "../../../../editor/raytracing_ui/RaytracingUI.h"

// Runtime
#include "PixelFormat.h"
#include "RaytracingConfig.h"
#include "config/ConfigManager.h"
#include "misc/BoundingBox.h"
#include "misc/Timer.h"
#include "renderer/common/RuntimeAssets.h"
#include "rendergraph/RenderGraph.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIImpl.h"
#include "rhi/RHIResource.h"
#include "rhi/plugin/NrdPlugin.h"
#include "rhi/vulkan/VulkanThreadHeartbeat.h"
#include "scene/GpuScene.h"
#include "string/Format.h"
#include "string/String.h"
#include "string/StringConvert.h"
#include "taskgraph/TaskGraph.h"
#include "trace/Trace.h"
#include "window/WindowContext.h"

// Editor
#include "AntiAliasPass.h"
#include "CompositionPass.h"
#include "Configs.h" // TODO: merge it with raster
#include "GBufferPass.h"
#include "LightingPass.h"
#include "PreprocessLightPass.h"
#include "RaytracingGraphResources.h"
#include "RTResource.h"
#include "ShaderUtils.h"
#include "ToneMappingPass.h"
#include "VisualizePass.h"

// 3rd party
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <stb/stb_image_write.h>

namespace Moer::Render::Raytracing {

static constexpr ERGPassFlags s_rg_graphics_compute_pass = ERGPassFlags::Graphics | ERGPassFlags::ComputeShader;

union FloatBits {
    float        f;
    unsigned int ui;
};

static Box3D scene_bounding{};

struct RGRaytracingNrdParams {
    DEFINE_RG_TEXTURE_ACCESS(motion, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(normal_roughness, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(specular_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(denoised_diffuse_lighting, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(denoised_specular_lighting, ETextureState::UNORDERED_ACCESS);

    DEFINE_RG_PARAMETER_ACCESS(
        motion,
        normal_roughness,
        view_depth,
        diffuse_lighting,
        specular_lighting,
        denoised_diffuse_lighting,
        denoised_specular_lighting
    );
};

struct RGRaytracingShowTextureParams {
    DEFINE_RG_TEXTURE_ACCESS(selected_texture, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(ldr_color, ETextureState::RENDER_TARGET);

    DEFINE_RG_PARAMETER_ACCESS(selected_texture, ldr_color);
};

struct RGRaytracingGuiClearParams {
    DEFINE_RG_TEXTURE_ACCESS(output, ETextureState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(output);
};

struct RGRaytracingGuiRenderParams {
    DEFINE_RG_TEXTURE_ACCESS(scene_output, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(output, ETextureState::RENDER_TARGET);
    DEFINE_RG_PARAMETER_ACCESS(scene_output, output);
};

struct RGRaytracingCommandParams {
    DEFINE_RG_PARAMETER_ACCESS();
};

struct RGRaytracingBuildTlasParams {
    DEFINE_RG_BUFFER_ACCESS(tlas_buffer, EBufferState::ACCELERATION_STRUCTURE_WRITE);
    DEFINE_RG_BUFFER_ACCESS_ARRAY(related_geometry_buffers);
    DEFINE_RG_PARAMETER_ACCESS(tlas_buffer, related_geometry_buffers);
};

struct RTPreparedSceneUpdateCommand {
    UniquePtr<Command> command{};
};

struct RGRaytracingLowDiscrepancyParams {
    DEFINE_RG_BUFFER_ACCESS(neighbor_offset_buf, EBufferState::UNORDERED_ACCESS);

    DEFINE_RG_PARAMETER_ACCESS(neighbor_offset_buf);
};

RaytracingRenderer::RaytracingRenderer(
    uint2&                        _resolution,
    uint2&                        _render_resolution,
    const SharedPtr<EditorConfig> _config,
    const EngineHooks&            _hooks,
    RuntimeAssets&                _runtime_assets
) :
    Renderer(_resolution, _render_resolution, _config, _hooks, _runtime_assets) {}

void RaytracingRenderer::Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    ::Moer::RaytracingUI config_ui(editor_config->raytracing_config);
    if (hooks.on_register_renderer_config_section) {
        hooks.on_register_renderer_config_section(
            "Raytracing", "Settings", [&config_ui](Synapse::Context& ui) {
                config_ui.ShowConfig(ui);
            }
        );
    }

    bool b_new_env_map = false;

    TextureRef         env_map{};
    Array<TextureView> env_mips;
    TextureRef         env_pdf{};
    Array<TextureView> env_pdf_mips;
    TextureWithHandle  env_tex_with_hdl;

    RaytracingSceneRef rt_scene = {};
    ShaderUtils        sd_utils(device, manager);

    ImportantSamplingParams is_params{};
    is_params.render_size = render_resolution;
    ImportanceSamplingContext is_ctx(is_params);

    bool first_load = true;

    uint2 current_render_resolution = render_resolution;

    UnorderedMap<String, TextureWithHandle> material_textures;

    float elapsed_time = 0.0f;

    TextureRef output = device.CreateTexture(
        MOER_TEXT("output"),
        Extent2D(render_resolution.x, render_resolution.y),
        presentation_surface->GetFormat(),
        ETextureUsageFlags::COLOR_ATTACHMENT
    );

    TextureRef combine_output = device.CreateTexture(
        MOER_TEXT("combine_output"),
        Extent2D(resolution.x, resolution.y),
        presentation_surface->GetFormat(),
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::TRANSFER_SRC
    );

    auto create_output_textures = [&](uint2 _scene_size, uint2 _window_size) {
        output = device.CreateTexture(
            MOER_TEXT("output"),
            Extent2D(_scene_size.x, _scene_size.y),
            presentation_surface->GetFormat(),
            ETextureUsageFlags::COLOR_ATTACHMENT
        );

        combine_output = device.CreateTexture(
            MOER_TEXT("combine_output"),
            Extent2D(_window_size.x, _window_size.y),
            presentation_surface->GetFormat(),
            ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::TRANSFER_SRC
        );
    };

    Timer timer;
    timer.Start();
    uint64                last_time               = 0ull;
    bool                  b_feedback_valid        = false;
    bool                  b_export                = false;
    String                selected_material_texture_name{};
    bool                  b_final_show_texture = false;
    bool                  b_use_bindless       = true;
    uint                  mip_level            = 0;
    std::filesystem::path exported_file_path   = ConfigManager::GetInstance().GetWorkspacePath() / "saved";
    if (!std::filesystem::exists(exported_file_path)) {
        std::filesystem::create_directory(exported_file_path);
    }
    //////////////////////////////////////////////////////////////////////////
    // passes
    //////////////////////////////////////////////////////////////////////////
    UniquePtr<PrepareLightPass> prepare_light_pass = MakeUnique<PrepareLightPass>(device, manager, scene);
    UniquePtr<GBufferPass>      g_buffer_pass      = MakeUnique<GBufferPass>(device, manager);
    UniquePtr<LightingPass>     lighting_pass      = MakeUnique<LightingPass>(manager);
    UniquePtr<CompositionPass>  composition_pass   = MakeUnique<CompositionPass>(device, manager);
    UniquePtr<VisualizePass>    visualize_pass     = MakeUnique<VisualizePass>(device, manager);
    UniquePtr<RTContext>        rt_ctx             = MakeUnique<RTContext>(sd_utils, is_ctx, bindless_array);
    UniquePtr<ToneMappingPass>  tone_mapping_pass;

    rt_ctx->SetResolution(render_resolution);
    AntialiasPass::CreateInfo antialias_pass_info{
        .motion              = RTRHI(rt_ctx->frame_rt.motion),
        .feedback_color_ping = RTRHI(rt_ctx->frame_rt.feedback_color_ping),
        .feedback_color_pong = RTRHI(rt_ctx->frame_rt.feedback_color_pong),
        .resolved_color      = RTRHI(rt_ctx->frame_rt.resolved_color),
        .hdr_color           = RTRHI(rt_ctx->frame_rt.hdr_color)
    };
    UniquePtr<AntialiasPass> antialias_pass =
        MakeUnique<AntialiasPass>(device, manager, scene, antialias_pass_info);

//////////////////////////////////////////////////////////////////////////
// NRD
//////////////////////////////////////////////////////////////////////////
#if WITH_NRD
    uint64 nrd_time      = 0ull;
    auto*  nrd_plugin    = device.LoadPlugin<Ext::NRDPlugin>();
    auto   nrd_interface = nrd_plugin->CreateInterface(max_frame_in_flight, render_resolution.x, render_resolution.y);
#endif

    VisualizeConfig visualize_config{};
    visualize_config.b_split        = false;
    visualize_config.split_ratio    = 0.5f;
    visualize_config.visualize_mode = EFC_DI;

    Array<std::function<void(uint)>> on_free_buffer_callbacks;

    auto add_on_free_buffer = [&](uint _buffer_handle) {
        on_free_buffer_callbacks.emplace_back([&, _buffer_handle](uint _timeline) {
            bindless_array->UnbindBuffer(_buffer_handle);
        });
    };

    auto add_on_free_texture = [&](uint _texture_handle) {
        on_free_buffer_callbacks.emplace_back([&, _texture_handle](uint _timeline) {
            bindless_array->UnbindTexture(_texture_handle);
        });
    };

    auto&      thread_heartbeat = VulkanThreadHeartbeat::Get();
    auto       main_thread_heartbeat =
        thread_heartbeat.Register(MOER_TEXT("RaytracingRenderer.Main"), MOER_TEXT("EnterLoop"));
    auto       pulse_main_thread             = [&](const PlatformChar* stage) {
        thread_heartbeat.Pulse(main_thread_heartbeat, stage);
    };

    pulse_main_thread(MOER_TEXT("EnterLoop"));

    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        TRACE_SCOPE_CAT("Raytracing.Frame", "Frame");

        pulse_main_thread(MOER_TEXT("Begin"));

        {
            TRACE_SCOPE_CAT("Raytracing.PumpAsyncLoads", "Frame");
            PumpAsyncLoads();
        }

        RaytracingConfig& ui_config = editor_config->raytracing_config;

        {
            TRACE_SCOPE_CAT("Raytracing.LogSceneLoadStatus", "Frame");
            LogSceneLoadStatus(*editor_config);
        }

        auto window_state = [&]() {
            TRACE_SCOPE_CAT("Raytracing.WindowTick", "Frame");
            return TickWindowContext(hooks);
        }();
        bool skip_present = false;

        pulse_main_thread(MOER_TEXT("WindowTick"));

        if (window_state == EWindowState::Hiding) {
            std::this_thread::yield();
            skip_present = true;

        } else if (window_state == EWindowState::SizeChanged || window_state == EWindowState::Default) {
            // 分辨率变化的具体处理推迟到 TickUI 之后，因为 render_resolution 可能在此期间改变

        } else {
            assert(false);
        }

        if (hooks.on_tick_ui) {
            TRACE_SCOPE_CAT("Raytracing.TickUI", "Frame");
            hooks.on_tick_ui(scene);
        }

        // 检查窗口尺寸与实际场景渲染尺寸是否发生变化，需要时重建输出纹理与 RT 上下文
        if (!skip_present) {
            const bool window_size_changed       = (window_state == EWindowState::SizeChanged);
            const bool render_resolution_changed =
                current_render_resolution.x != render_resolution.x ||
                current_render_resolution.y != render_resolution.y;
            const bool render_resolution_valid =
                render_resolution.x > 0 && render_resolution.y > 0;

            if ((window_size_changed || render_resolution_changed) && render_resolution_valid) {
                LOG_INFO(
                    MOER_TEXT("Raytracing recreate resources: window_size_changed={}, render_resolution_changed={}")
                    , window_size_changed
                    , render_resolution_changed
                );

                create_output_textures(render_resolution, resolution);

                if (render_resolution_changed) {
                    rt_ctx->SetResolution(render_resolution);

                    is_ctx.~ImportanceSamplingContext();
                    is_params.render_size = render_resolution;
                    new (&is_ctx) ImportanceSamplingContext(is_params);

#if WITH_NRD
                    nrd_interface = nrd_plugin->RecreateInterface(
                        std::move(nrd_interface), render_resolution.x, render_resolution.y
                    );
#endif

                    antialias_pass_info.motion              = RTRHI(rt_ctx->frame_rt.motion);
                    antialias_pass_info.feedback_color_ping = RTRHI(rt_ctx->frame_rt.feedback_color_ping);
                    antialias_pass_info.feedback_color_pong = RTRHI(rt_ctx->frame_rt.feedback_color_pong);
                    antialias_pass_info.resolved_color      = RTRHI(rt_ctx->frame_rt.resolved_color);
                    antialias_pass_info.hdr_color           = RTRHI(rt_ctx->frame_rt.hdr_color);
                    antialias_pass = MakeUnique<AntialiasPass>(device, manager, scene, antialias_pass_info);
                    b_feedback_valid = false;

                    current_render_resolution = render_resolution;
                }
            }
        }

        timer.Stop();
        auto frame_time = timer.ElapsedMilliseconds();
        timer.Start();

        Array<CommandList> pre_frame_cmd_lists{};
        bool               advance_rt_frame             = false;
        bool               advance_post_process_history = false;
        bool               advance_rt_scene             = false;
        float              post_process_frame_time      = 0.f;
        TextureRef         final_color{};
        TextureRef         present_output{};
        bool               gui_rendered_in_graph        = false;

        if (scene.IsReady() && runtime_assets.IsReady()) {
            TRACE_SCOPE_CAT("Raytracing.SceneReadyFrame", "Frame");
            pulse_main_thread(MOER_TEXT("SceneReady"));

            // Submit scene-loading uploads before frame graph execution.
            {
                TRACE_SCOPE_CAT("Raytracing.SceneUploads.PopPendingCommandList", "Frame");
                auto&& scene_cmd_list = scene.PopPendingCommandList();
                if (!scene_cmd_list.gfx_queue_cmd_list.IsEmpty()) {
                    pre_frame_cmd_lists.emplace_back(std::move(scene_cmd_list.gfx_queue_cmd_list));
                }
            }

            // load scene
            if (first_load) {
                TRACE_SCOPE_CAT("Raytracing.FirstLoad", "Frame");
                first_load = false;

                b_new_env_map = true;
                // calculate bounding box
                {
                    TRACE_SCOPE_CAT("Raytracing.FirstLoad.SceneBounds", "Frame");
                    scene_bounding.min = float3(0.f);
                    scene_bounding.max = float3(0.f);

                    scene.r().view<ecs::CRenderable, ecs::CNode>().each(
                        [&](auto entity_id, const auto& c_renderable, const auto& c_transform) {
                            if (c_transform.d_aabb.IsValid()) {
                                scene_bounding.Expand(c_transform.d_aabb);
                            }
                        }
                    );
                }

                // new_cell size
                float3 extent = scene_bounding.max - scene_bounding.min;

                float max_extent                = std::max(std::max(extent.x, extent.y), extent.z);
                float cell_size                 = max_extent * 2 / is_ctx.GetGridConfig().grid_size.x;
                ui_config.grid_config.cell_size = cell_size;

                {
                    TRACE_SCOPE_CAT("Raytracing.FirstLoad.BindSceneResources", "Frame");
                    rt_scene = scene.GetGpuSceneRes().rt_scene;

                    rt_ctx->SetBindlessHandles(scene.GetGpuSceneRes());
                    rt_ctx->SetRaytracingScene(rt_scene);
                }
                if (hooks.on_register_renderer_config_section) {
                    TRACE_SCOPE_CAT("Raytracing.FirstLoad.RegisterUI", "Frame");

                    auto& debug_queue = gfx_queue;

                    hooks.on_register_renderer_config_section(
                        "Raytracing",
                        "Material Texture",
                        [&material_textures,
                         &selected_material_texture_name,
                         &b_use_bindless,
                         &b_final_show_texture,
                         &mip_level,
                         &debug_queue](Synapse::Context& ui) {
                            ui.Checkbox("Show Final Texture", &b_final_show_texture);
                            ui.SliderInt("Mip Level", (int*)&mip_level, 0, 12);
                            ui.Checkbox("Use Bindless", &b_use_bindless);
                            if (ui.TreeNode("MaterialTexture")) {
                                for (auto& [name, tex] : material_textures) {
                                    // selectable
                                    const Utf8String ui_name = PlatformToUtf8(name);
                                    if (ui.Selectable(
                                            ui_name.c_str(), selected_material_texture_name == name
                                        )) {
                                        selected_material_texture_name = name;
                                    }
                                }
                                ui.TreePop();
                            }

                            //Pass profiling
                            auto entrys = debug_queue.GetProfilerEntry();
                            if (!entrys.cpu_entries.empty()) {
                                ui.Text("CPU Time:");
                                for (auto& [name, time] : entrys.cpu_entries) {
                                    if (name.ends_with(MOER_TEXT("Percentage"))) {
                                        ui.Text("%s: %.3f%%", name.c_str(), time * 100);

                                    } else
                                        ui.Text("%s: %.3f ms", name.c_str(), time);
                                }
                            }
                            if (!entrys.gpu_entries.empty()) {
                                ui.Text("GPU Time:");
                                for (auto& [name, time] : entrys.gpu_entries) {
                                    ui.Text("%s: %.3f ms", name.c_str(), time);
                                }
                            }
                        }
                    );
                }

                // cmd_list.UpdateRaytracingScene(rt_scene);
                if (!cmd_list.IsEmpty()) {
                    // Keep first-load bootstrap in async submit chain.
                    pre_frame_cmd_lists.emplace_back(std::move(cmd_list));
                    cmd_list = CommandList(EQueueType::Graphics);
                }
            }

            if (b_new_env_map) {
                TRACE_SCOPE_CAT("Raytracing.EnvMap.Update", "Frame");
                auto src_env_map = runtime_assets.GetDefaultEnvMap();
                {
                    TRACE_SCOPE_CAT("Raytracing.EnvMap.CreateTexture", "Frame");
                    env_map = device.CreateTexture(
                        src_env_map->GetName(),
                        Extent3D(src_env_map->GetExtent()),
                        PF_R16G16B16A16_SFLOAT,
                        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                        src_env_map->GetNumMips()
                    );
                }
                {
                    TRACE_SCOPE_CAT("Raytracing.EnvMap.RecordSample", "Frame");
                    sd_utils.SampleTextureCS(
                        cmd_list, src_env_map->GetView(0, 1), env_map->GetView(0, 1), src_env_map->GetFormat()
                    );
                }

                {
                    TRACE_SCOPE_CAT("Raytracing.EnvMap.Bindless", "Frame");
                    Sampler sampler{SF_CUBIC, SAM_REPEAT};
                    env_tex_with_hdl.tex = env_map;
                    env_tex_with_hdl.hdl =
                        bindless_array->AllocateTexture(env_map->GetView(0, env_map->GetNumMips()), sampler);
                }

                {
                    TRACE_SCOPE_CAT("Raytracing.EnvMap.RecordMips", "Frame");
                    for (int i = 0; i < env_map->GetNumMips(); ++i) {
                        env_mips.push_back(env_map->GetView(i));
                    }
                    sd_utils.GenerateMips(cmd_list, env_mips);
                }
                b_new_env_map = false;

                {
                    TRACE_SCOPE_CAT("Raytracing.EnvMap.CreatePdfResources", "Frame");
                    rt_ctx->LoadDefaultResources(runtime_assets);
                    rt_ctx->CreateEnvMapResources(env_tex_with_hdl, cmd_list);
                }
            }

            if (!tone_mapping_pass) {
                TRACE_SCOPE_CAT("Raytracing.CreateToneMappingPass", "Frame");
                ToneMappingPass::CreateInfo info{};
                info.color_lut    = rt_ctx->default_res.black_tex;
                tone_mapping_pass = MakeUnique<ToneMappingPass>(device, manager, scene, info);
            }

            auto& camera = scene.GetMainCamera().camera;

            // prepare frame
            {
                TRACE_SCOPE_CAT("Raytracing.CameraTick", "Frame");
                camera.Tick(editor_config);
            }

            // update light direction from ui data
            {
                TRACE_SCOPE_CAT("Raytracing.UpdateDirectionalLight", "Frame");
                b_export = ui_config.export_cfg.b_export;

                auto light_entity = scene.GetMainDirectionalLightEntity();
                if (light_entity != entt::null && scene.r().valid(light_entity) &&
                    scene.r().all_of<ecs::CLightDirectional, ecs::CNode>(light_entity)) {
                    auto& c_light_dir     = scene.r().get<ecs::CLightDirectional>(light_entity);
                    c_light_dir.color     = float3(0.9f, 0.65f, 0.4f);
                    c_light_dir.intensity = ui_config.exposure;

                    auto& c_transform    = scene.r().get<ecs::CNode>(light_entity);
                    c_transform.rotation = Quaternion(float3(0.f, 0.f, -1.f), -ui_config.sun_direction);
                    c_transform.is_dirty = true;
                }
            }

            ToneMappingPass::Params tone_params{};
            AntialiasPass::Params   aa_params{};
            // fill ui data
            {

                TRACE_SCOPE_CAT("Raytracing.Configs", "Frame");

                auto        grid_cfg           = is_ctx.GetGridChangableConfig();
                const auto& ui_cfg             = ui_config;
                grid_cfg.cell_size             = ui_config.grid_config.cell_size;
                grid_cfg.center                = camera.GetPosition();
                auto grid_static_cfg           = is_ctx.GetGridConfig();
                grid_static_cfg.light_per_ceil = ui_config.grid_config.light_per_ceil;
                grid_static_cfg.grid_mode      = ui_config.grid_config.grid_mode;
                is_ctx.SetGridConfig(grid_static_cfg);
                is_ctx.SetChangeableGridConfig(grid_cfg);

                rt_ctx->config.reblur_diffuse_hit_dist_params =
                    *reinterpret_cast<const float4*>(&ui_cfg.denoiser_cfg.hit_dist_params);
                rt_ctx->config.reblur_specular_hit_dist_params =
                    *reinterpret_cast<const float4*>(&ui_cfg.denoiser_cfg.hit_dist_params);
                rt_ctx->config.denoiser_mode = ui_cfg.denoiser_cfg.denoiser_type;

                auto di_initial_sample_config      = is_ctx.GetDIInitialSampleParams();
                auto di_temporal_resampling_config = is_ctx.GetDITemporalResampleParams();
                auto di_spatial_resampling_config  = is_ctx.GetDISpatialResampleParams();
                di_initial_sample_config.local_light_sample_mode =
                    ui_cfg.restir_di_cfg.initial_sample_config.local_light_sample_mode;
                di_initial_sample_config.env_map_is = is_ctx.GetLightBufferParams().env_light.light_cnt;
                di_temporal_resampling_config.bias_correction_mode =
                    ui_cfg.restir_di_cfg.temporal_resample_config.bias_correction;
                di_temporal_resampling_config.depth_threshold =
                    ui_cfg.restir_di_cfg.temporal_resample_config.depth_threshold;
                di_temporal_resampling_config.normal_threshold =
                    ui_cfg.restir_di_cfg.temporal_resample_config.normal_threshold;

                di_spatial_resampling_config.bias_correction_mode =
                    ui_cfg.restir_di_cfg.spatial_resample_config.bias_correction;
                di_spatial_resampling_config.depth_threshold =
                    ui_cfg.restir_di_cfg.spatial_resample_config.depth_threshold;
                di_spatial_resampling_config.normal_threshold =
                    ui_cfg.restir_di_cfg.spatial_resample_config.normal_threshold;
                di_spatial_resampling_config.num_spatial_samples =
                    ui_cfg.restir_di_cfg.spatial_resample_config.num_spatial_samples;

                is_ctx.SetReSTIRDIIInitialSampleParams(di_initial_sample_config);
                is_ctx.SetReSTIRDITemporalResampleParams(di_temporal_resampling_config);
                is_ctx.SetReSTIRDISpatialResampleParams(di_spatial_resampling_config);

                // params.min_adapted_luminance   = 0.002f;
                // params.max_adapted_luminance   = 0.5f;
                // params.exposure_bias           = -1.f;
                // params.eye_adaptation_speed_up = 2.f;
                // params.eye_adaptation_speed_up = 1.f;
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

                antialias_pass->SetJitter(aa_cfg.jitter_mode);

                rt_ctx->b_parallel_process_light = ui_config.process_light_cfg.parallel_mode;
                rt_ctx->num_threads              = ui_config.process_light_cfg.num_threads;
            }

            {
                TRACE_SCOPE_CAT("Raytracing.ImportanceSampling.TickFrame", "Frame");
                is_ctx.TickFrame(time);
            }
            bool   uses_post_process_output = false;
            float2 pixel_jitter             = float2(0.f);
            {
                TRACE_SCOPE_CAT("Raytracing.ResolveFrameModes", "Frame");
                visualize_config.visualize_mode = ui_config.final_color;
                uses_post_process_output =
                    b_final_show_texture || ui_config.final_color == Render::EFinalColor::EFC_SceneColor;
                pixel_jitter = uses_post_process_output ? antialias_pass->GetPixelOffset() : float2(0.f);
            }

            PrepareLightPass::PreparedCommand prepare_lights_command{};
            {
                TRACE_SCOPE_CAT("Raytracing.PrepareLights.PrepareFrame", "Frame");
                prepare_lights_command = prepare_light_pass->Prepare(*rt_ctx);
            }

            {
                TRACE_SCOPE_CAT("Raytracing.Frame.RTContextTick", "Frame");
                rt_ctx->Tick(camera, pixel_jitter);
            }

            UniquePtr<Command> update_rt_scene_command{};
            {
                TRACE_SCOPE_CAT("Raytracing.RTScene.UpdateCommand", "Frame");
                if (rt_scene) {
                    update_rt_scene_command = rt_scene->UpdateScene();
                }
            }

            RenderGraph                 raytracing_graph(MOER_TEXT("RT.RenderGraph"));
            RTGraphFrameResources       rg_rt{};
            {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.RegisterFrameResources", "Frame");
                rg_rt = RegisterRTGraphFrameResources(raytracing_graph, *rt_ctx);
            }
            RGTexture*                  current_view_depth = rg_rt.current_view_depth;

            {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.FrameSetup", "Frame");
                RG_EVENT_SCOPE(raytracing_graph, MOER_TEXT("RT.FrameSetup"));
                if (auto low_discrepancy_command = rt_ctx->PrepareLowDiscrepancySequence(); low_discrepancy_command.enabled) {
                    auto* params                = raytracing_graph.Alloc<RGRaytracingLowDiscrepancyParams>();
                    params->neighbor_offset_buf = RGBufferView{
                        .buffer = rg_rt.neighbor_offset_buf,
                    };
                    auto* command = raytracing_graph.Alloc<RTContext::LowDiscrepancySequenceCommand>(
                        low_discrepancy_command
                    );
                    raytracing_graph.AddPass(
                        MOER_TEXT("RT.GenerateLowDiscrepancySequence"),
                        params,
                        s_rg_graphics_compute_pass,
                        [context = rt_ctx.get(), command](CommandList& graph_cmd_list, RGContext) {
                            context->RecordLowDiscrepancySequence(graph_cmd_list, *command);
                        }
                    );
                }

                {
                    auto* params = raytracing_graph.Alloc<RGRaytracingCommandParams>();
                    raytracing_graph.AddPass(
                        MOER_TEXT("RT.UpdateBindlessArray"),
                        params,
                        s_rg_graphics_compute_pass,
                        [bdls = bindless_array](CommandList& graph_cmd_list, RGContext) {
                            graph_cmd_list.Barriers(
                                EQueueType::Graphics,
                                EQueueType::Graphics,
                                EPassType::Compute,
                                ETrackedStateUpdateMode::Update,
                                WriteBindlessArray{bdls, EBufferState::UNORDERED_ACCESS}
                            );
                            graph_cmd_list.UpdateBindlessArray(bdls);
                            graph_cmd_list.Barriers(
                                EQueueType::Graphics,
                                EQueueType::Graphics,
                                EPassType::Compute,
                                ETrackedStateUpdateMode::Update,
                                ReadBindlessArray{bdls, EBufferState::SHADER_RESOURCE}
                            );
                        }
                    );
                }
            }

            if (update_rt_scene_command) {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.SceneUpdate", "Frame");
                RG_EVENT_SCOPE(raytracing_graph, MOER_TEXT("RT.SceneUpdate"));
                assert(update_rt_scene_command->Type() == Command::EType::BuildTLAS);
                const auto* update_cmd = static_cast<const UpdateRaytracingSceneCmd*>(update_rt_scene_command.get());
                RaytracingTlasRef tlas(reinterpret_cast<RaytracingTlas*>(update_cmd->TlasHandle()));
                RGBuffer* tlas_buffer = ImportRTTlasBufferIfValid(
                    raytracing_graph,
                    MOER_TEXT("RT.BuildTLAS.tlas_buffer"),
                    tlas
                );
                auto* related_geometry_buffers = raytracing_graph.Alloc<RGBufferAccessArray>(
                    static_cast<uint32_t>(update_cmd->RelatedGeometries().size())
                );
                uint32_t geometry_index = 0;
                for (const auto& [geometry_handle, instance_count] : update_cmd->RelatedGeometries()) {
                    (void)instance_count;
                    RaytracingGeometryRef geometry(reinterpret_cast<RaytracingGeometry*>(geometry_handle));
                    String geometry_name(MOER_TEXT("RT.BuildTLAS.geometry_buffer_"));
                    RGAppendDecimal(geometry_name, geometry_index++);
                    RGBuffer* geometry_buffer = ImportRTGeometryBufferIfValid(raytracing_graph, geometry_name, geometry);
                    related_geometry_buffers->AddAccess(
                        RGBufferView{.buffer = geometry_buffer},
                        EBufferState::ACCELERATION_STRUCTURE_READ
                    );
                }

                auto* params = raytracing_graph.Alloc<RGRaytracingBuildTlasParams>();
                params->tlas_buffer = RGBufferView{.buffer = tlas_buffer};
                params->related_geometry_buffers = related_geometry_buffers;
                auto* command = raytracing_graph.Alloc<RTPreparedSceneUpdateCommand>();
                command->command = std::move(update_rt_scene_command);
                raytracing_graph.AddPass(
                    MOER_TEXT("RT.BuildTLAS"),
                    params,
                    ERGPassFlags::Graphics,
                    [command](CommandList& graph_cmd_list, RGContext) {
                        graph_cmd_list.UpdateRaytracingScene(std::move(command->command));
                    }
                );
            }

            {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.PrepareLights", "Frame");
                RG_EVENT_SCOPE(raytracing_graph, MOER_TEXT("RT.PrepareLights"));
                prepare_light_pass->AddPasses(raytracing_graph, rg_rt, *rt_ctx, std::move(prepare_lights_command));
            }
            {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.GBuffer", "Frame");
                RG_EVENT_SCOPE(raytracing_graph, MOER_TEXT("RT.GBuffer"));
                g_buffer_pass->AddPasses(raytracing_graph, rg_rt, *rt_ctx);
            }
            {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.Lighting", "Frame");
                RG_EVENT_SCOPE(raytracing_graph, MOER_TEXT("RT.Lighting"));
                lighting_pass->AddPasses(raytracing_graph, rg_rt, *rt_ctx);
            }

//////////////////////////////////////////////////////////////////////////
// NRD
//////////////////////////////////////////////////////////////////////////
#if WITH_NRD
            {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.NRD", "Frame");
                nrd::Denoiser denoiser = nrd::Denoiser::MAX_NUM;
                switch (ui_config.denoiser_cfg.denoiser_type) {
                    case s_denoiser_mode_reblur:
                        denoiser = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
                        break;
                    case s_denoiser_mode_relax:
                        denoiser = nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
                        break;
                }

                if (denoiser != nrd::Denoiser::MAX_NUM) {
                    const bool  b_current_frame = rt_ctx->b_current_frame;
                    const auto& frame_rt        = rt_ctx->frame_rt;
                    nrd_interface->Begin();
                    nrd_interface->UpdateCommonSettings(
                        nrd_time++,
                        Vector2ui(render_resolution.x, render_resolution.y),
                        pixel_jitter,
                        Transpose(camera.GetViewMatrix()),
                        Transpose(camera.GetProjectionMatrix())
                    );
                    nrd_interface->SetInput(Ext::NRDInterface::EResourceSlot::MOTION_VECTOR, RTRHI(frame_rt.motion));
                    nrd_interface->SetInput(
                        Ext::NRDInterface::EResourceSlot::NORMAL_ROUGHNESS,
                        RTRHI(frame_rt.normal_roughness)
                    );
                    nrd_interface->SetInput(
                        Ext::NRDInterface::EResourceSlot::VIEW_Z,
                        RTRHI(b_current_frame ? frame_rt.view_depth : frame_rt.prev_view_depth)
                    );
                    nrd_interface->SetInput(
                        Ext::NRDInterface::EResourceSlot::IN_DIFFUSE, RTRHI(frame_rt.diffuse_lighting)
                    );
                    nrd_interface->SetInput(
                        Ext::NRDInterface::EResourceSlot::IN_SPECULAR, RTRHI(frame_rt.specular_lighting)
                    );
                    nrd_interface->SetOutput(
                        Ext::NRDInterface::EResourceSlot::OUT_DIFFUSE,
                        RTRHI(frame_rt.denoised_diffuse_lighting)
                    );
                    nrd_interface->SetOutput(
                        Ext::NRDInterface::EResourceSlot::OUT_SPECULAR,
                        RTRHI(frame_rt.denoised_specular_lighting)
                    );

                    auto* params   = raytracing_graph.Alloc<RGRaytracingNrdParams>();
                    params->motion = RGTextureView{
                        .texture = rg_rt.motion,
                    };
                    params->normal_roughness = RGTextureView{
                        .texture = rg_rt.normal_roughness,
                    };
                    params->view_depth = RGTextureView{
                        .texture = current_view_depth,
                    };
                    params->diffuse_lighting = RGTextureView{
                        .texture = rg_rt.diffuse_lighting,
                    };
                    params->specular_lighting = RGTextureView{
                        .texture = rg_rt.specular_lighting,
                    };
                    params->denoised_diffuse_lighting = RGTextureView{
                        .texture = rg_rt.denoised_diffuse_lighting,
                    };
                    params->denoised_specular_lighting = RGTextureView{
                        .texture = rg_rt.denoised_specular_lighting,
                    };
                    {
                        RG_EVENT_SCOPE(raytracing_graph, MOER_TEXT("RT.Denoiser"));
                        raytracing_graph.AddPass(
                            MOER_TEXT("RT.NRD"),
                            params,
                            s_rg_graphics_compute_pass,
                            [nrd = nrd_interface.get(), denoiser](CommandList& graph_cmd_list, RGContext) {
                                nrd->Denoise(graph_cmd_list, denoiser, MOER_TEXT("Radiance Denoising"));
                            }
                        );
                    }
                }
            }
#endif

            {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.Composition", "Frame");
                RG_EVENT_SCOPE(raytracing_graph, MOER_TEXT("RT.Composition"));
                composition_pass->AddPass(raytracing_graph, rg_rt, *rt_ctx);
            }

            if (uses_post_process_output) {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.PostProcess", "Frame");
                RG_EVENT_SCOPE(raytracing_graph, MOER_TEXT("RT.PostProcess"));
                antialias_pass->AddPass(
                    raytracing_graph,
                    rg_rt,
                    *rt_ctx,
                    aa_params,
                    b_feedback_valid,
                    RTRHI(rt_ctx->frame_rt.hdr_color),
                    RTRHI(rt_ctx->frame_rt.resolved_color)
                );

                tone_mapping_pass->AddPasses(
                    raytracing_graph,
                    rg_rt,
                    *rt_ctx,
                    tone_params,
                    RTRHI(rt_ctx->frame_rt.resolved_color),
                    RTRHI(rt_ctx->frame_rt.ldr_color)
                );
            } else {
                b_feedback_valid = false;
            }

            {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.Visualize", "Frame");
                RG_EVENT_SCOPE(raytracing_graph, MOER_TEXT("RT.Visualize"));
                visualize_pass->AddPass(raytracing_graph, rg_rt, *rt_ctx, visualize_config);
            }

            if (b_final_show_texture && material_textures.contains(selected_material_texture_name)) {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.ShowTexture", "Frame");
                RG_EVENT_SCOPE(raytracing_graph, MOER_TEXT("RT.ShowTexture"));
                TextureRef        texture          = material_textures[selected_material_texture_name].tex;
                RGTexture* selected_texture = ImportExternalRTTextureIfValid(
                    raytracing_graph, MOER_TEXT("RT.selected_material_texture"), texture
                );
                auto* params             = raytracing_graph.Alloc<RGRaytracingShowTextureParams>();
                params->selected_texture = RTWholeTextureView(selected_texture);
                params->ldr_color        = RTWholeTextureView(rg_rt.ldr_color);
                const uint selected_texture_handle = material_textures[selected_material_texture_name].hdl;
                ShowTextureParams show_texture_params{};
                show_texture_params.dst_dim      = render_resolution;
                show_texture_params.bdls_handle  = selected_texture_handle;
                show_texture_params.mip_level    = mip_level;
                show_texture_params.use_bindless = b_use_bindless;
                raytracing_graph.AddPass(
                    MOER_TEXT("RT.ShowTexture"),
                    params,
                    ERGPassFlags::Graphics,
                    [shader_utils = &sd_utils,
                     bdls = bindless_array,
                     show_texture_params,
                     texture,
                     dst_texture = RTRHI(rt_ctx->frame_rt.ldr_color)](CommandList& graph_cmd_list, RGContext) {
                        shader_utils->ShowTexture(
                            graph_cmd_list,
                            bdls,
                            show_texture_params,
                            texture,
                            dst_texture
                        );
                    }
                );
            }

            RGTexture* final_color_rg = b_final_show_texture ? rg_rt.ldr_color : rg_rt.debug_color;
            {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.ResolveOutput", "Frame");
                final_color     = RTRHI(b_final_show_texture ? rt_ctx->frame_rt.ldr_color : rt_ctx->frame_rt.debug_color);
                present_output  = final_color;
            }

            RGTexture* gui_output_rg = nullptr;
            if (!editor_config->play_mode_enabled) {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.EditorOutput", "Frame");
                if (hooks.on_publish_scene_output) {
                    TRACE_SCOPE_CAT("Raytracing.GraphBuild.PublishSceneOutput", "Frame");
                    hooks.on_publish_scene_output(final_color);
                }
                if (hooks.on_render_gui) {
                    TRACE_SCOPE_CAT("Raytracing.GraphBuild.GUI", "Frame");
                    RG_EVENT_SCOPE(raytracing_graph, MOER_TEXT("RT.GUI"));

                    gui_rendered_in_graph = true;
                    present_output        = combine_output;
                    gui_output_rg = raytracing_graph.ImportTexture(
                        MOER_TEXT("RT.combine_output"), combine_output, EQueueType::Graphics
                    );

                    auto* clear_params   = raytracing_graph.Alloc<RGRaytracingGuiClearParams>();
                    clear_params->output = RTWholeTextureView(gui_output_rg);
                    raytracing_graph.AddPass(
                        MOER_TEXT("RT.GUI.Clear"),
                        clear_params,
                        ERGPassFlags::Graphics | ERGPassFlags::Serial,
                        [gui_output = combine_output](CommandList& graph_cmd_list, RGContext) {
                            graph_cmd_list.ClearResource(gui_output->GetView(), float4(0.f, 0.f, 0.f, 1.f));
                        }
                    );

                    auto* gui_params          = raytracing_graph.Alloc<RGRaytracingGuiRenderParams>();
                    gui_params->scene_output  = RTWholeTextureView(final_color_rg);
                    gui_params->output        = RTWholeTextureView(gui_output_rg);
                    raytracing_graph.AddPass(
                        MOER_TEXT("RT.GUI.Render"),
                        gui_params,
                        ERGPassFlags::Graphics | ERGPassFlags::Serial,
                        [render_gui = hooks.on_render_gui, gui_output = combine_output](
                            CommandList& graph_cmd_list,
                            RGContext
                        ) {
                            render_gui(graph_cmd_list, gui_output);
                        }
                    );
                }
            }
            {
                TRACE_SCOPE_CAT("Raytracing.PreFrameSubmit", "Frame");
                if (!cmd_list.IsEmpty()) {
                    pre_frame_cmd_lists.emplace_back(std::move(cmd_list));
                    cmd_list = CommandList(EQueueType::Graphics);
                }
                if (!pre_frame_cmd_lists.empty()) {
                    pulse_main_thread(MOER_TEXT("PreFrameSubmitBegin"));
                    RHIExecutor::Get().Submit(std::move(pre_frame_cmd_lists), ERHIExecSubmitFlags::None);
                    pre_frame_cmd_lists.clear();
                    pulse_main_thread(MOER_TEXT("PreFrameSubmitEnd"));
                }
            }
            {
                TRACE_SCOPE_CAT("Raytracing.GraphBuild.ExportTextures", "Frame");
                raytracing_graph.ExportTexture(rg_rt.debug_color, ETextureState::SAMPLED, EQueueType::Graphics);
                if (uses_post_process_output || b_final_show_texture) {
                    raytracing_graph.ExportTexture(rg_rt.ldr_color, ETextureState::SAMPLED, EQueueType::Graphics);
                }
                if (gui_output_rg) {
                    raytracing_graph.ExportTexture(gui_output_rg, ETextureState::TRANSFER_SRC, EQueueType::Graphics);
                }
            }
            pulse_main_thread(MOER_TEXT("GraphDispatchBegin"));
            {
                TRACE_SCOPE_CAT("Raytracing.GraphDispatch", "Frame");
                raytracing_graph.Dispatch();
            }
            pulse_main_thread(MOER_TEXT("GraphDispatchEnd"));
            advance_rt_frame             = true;
            advance_post_process_history = uses_post_process_output;
            advance_rt_scene             = rt_scene != nullptr;
            post_process_frame_time      = camera.GetDeltaTime();
            // copy normal to output
            //  cmd_list.CopyFrom(out_direct_lighting->GetView(),
            //  scene_color->GetView());

            //////////////////////////////////////////////////////////////////////////
            // handle export
            //////////////////////////////////////////////////////////////////////////

            if (b_export) {
                TRACE_SCOPE_CAT("Raytracing.Export", "Frame");
                FenceRef export_fence = device.CreateFence();
                cmd_list.Signal(export_fence, 1);
                Array<CommandList> export_cmd_lists{};
                export_cmd_lists.emplace_back(std::move(cmd_list));
                RHIExecutor::Get().Submit(std::move(export_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
                cmd_list = CommandList(EQueueType::Graphics);
                export_fence->Wait(1);
                // Executor-managed fence already guarantees GPU completion.
                DumpTextureToFile(
                    ui_config.export_cfg,
                    rt_ctx->frame_rt,
                    device,
                    gfx_queue,
                    exported_file_path,
                    Printf(MOER_TEXT("{}"), time)
                );
                b_export = false;
            }
        }

        // rt_scene->MarkModified(0);
        // cmd_list.UpdateRaytracingScene(rt_scene);
        if (!final_color) {
            TRACE_SCOPE_CAT("Raytracing.Frame.ResolveOutput", "Frame");
            final_color = RTRHI(b_final_show_texture ? rt_ctx->frame_rt.ldr_color : rt_ctx->frame_rt.debug_color);
            present_output = final_color;
        }

        if (!gui_rendered_in_graph && !editor_config->play_mode_enabled) {
            TRACE_SCOPE_CAT("Raytracing.Frame.EditorOutput", "Frame");
            if (hooks.on_publish_scene_output) {
                TRACE_SCOPE_CAT("Raytracing.Frame.PublishSceneOutput", "Frame");
                hooks.on_publish_scene_output(final_color);
            }
            if (hooks.on_render_gui) {
                TRACE_SCOPE_CAT("Raytracing.Frame.RenderGUI", "Frame");
                cmd_list.Barriers(
                    {BarrierCreateInfo::Transition(
                        combine_output->GetView(),
                        MakeBarrierState(ETextureState::TRANSFER_SRC, EPassType::Copy),
                        MakeBarrierState(ETextureState::TRANSFER_DST, EPassType::Copy)
                    )},
                    EQueueType::Graphics,
                    EQueueType::Graphics,
                    ETrackedStateUpdateMode::Update
                );
                cmd_list.ClearResource(combine_output->GetView(), float4(0.f, 0.f, 0.f, 1.f));
                cmd_list.Barriers(
                    {BarrierCreateInfo::Transition(
                        combine_output->GetView(),
                        MakeBarrierState(ETextureState::TRANSFER_DST, EPassType::Copy),
                        MakeBarrierState(ETextureState::RENDER_TARGET, EPassType::Graphics)
                    )},
                    EQueueType::Graphics,
                    EQueueType::Graphics,
                    ETrackedStateUpdateMode::Update
                );
                hooks.on_render_gui(cmd_list, combine_output);
                cmd_list.Barriers(
                    {BarrierCreateInfo::Transition(
                        combine_output->GetView(),
                        MakeBarrierState(ETextureState::RENDER_TARGET, EPassType::Graphics),
                        MakeBarrierState(ETextureState::TRANSFER_SRC, EPassType::Copy)
                    )},
                    EQueueType::Graphics,
                    EQueueType::Graphics,
                    ETrackedStateUpdateMode::Update
                );
                present_output = combine_output;
            }
        }

        bool should_close_now = false;
        {
            TRACE_SCOPE_CAT("Raytracing.Frame.CheckClose", "Frame");
            should_close_now = WindowContext::ShouldClose(WindowContext::GetMainWindow());
        }
        if (should_close_now) {
            // Avoid extra present work in the closing frame to reduce shutdown deadlock risk.
            skip_present = true;
        }

        time++;
        {
            TRACE_SCOPE_CAT("Raytracing.Frame.PresentSubmit", "Frame");
            RHIPresentRequest present_request = presentation_surface->CreatePresentRequest(present_output);
            cmd_list.Signal(timeline, time).DeleteResources().TickFrame();
            Array<CommandList> frame_cmd_lists = std::move(pre_frame_cmd_lists);
            frame_cmd_lists.emplace_back(std::move(cmd_list));
            pulse_main_thread(MOER_TEXT("PresentSubmitBegin"));
            RHIExecutor::Get().Submit(
                std::move(frame_cmd_lists),
                ERHIExecSubmitFlags::FlushGPU,
                skip_present ? nullptr : &present_request
            );
        }
        if (advance_rt_frame) {
            TRACE_SCOPE_CAT("Raytracing.Frame.AdvanceRTFrame", "Frame");
            rt_ctx->AdvanceFrame();
            if (advance_post_process_history) {
                TRACE_SCOPE_CAT("Raytracing.Frame.AdvancePostProcess", "Frame");
                tone_mapping_pass->AdvanceFrame(post_process_frame_time);
                antialias_pass->AdvanceFrame();
                b_feedback_valid = true;
            }
        }
        if (advance_rt_scene && rt_scene) {
            TRACE_SCOPE_CAT("Raytracing.Frame.AdvanceRTScene", "Frame");
            rt_scene->AdvanceFrame();
        }
        pulse_main_thread(MOER_TEXT("PresentSubmitEnd"));
        cmd_list = CommandList(EQueueType::Graphics);

        if (!skip_present && !editor_config->play_mode_enabled && hooks.on_present_windows) {
            TRACE_SCOPE_CAT("Raytracing.Frame.PresentWindows", "Frame");
            hooks.on_present_windows();
            pulse_main_thread(MOER_TEXT("PresentWindowEnd"));
        }
        if (should_close_now) {
            break;
        }
        if (hooks.on_is_need_reload) {
            TRACE_SCOPE_CAT("Raytracing.Frame.CheckReload", "Frame");
            if (hooks.on_is_need_reload()) {
                break;
            }
        }
    }

    if (time > 0) {
        timeline->Wait(time);
        // Executor-managed timeline already guarantees GPU completion.
    }
    thread_heartbeat.Unregister(main_thread_heartbeat);
    const auto& allocated_buf = rt_ctx->GetAllocatedBdlsBuf();
    for (auto& buf : allocated_buf) {
        bindless_array->UnbindBuffer(buf);
    }

    const auto& allocated_tex = rt_ctx->GetAllocatedBdlsTex();
    for (auto& tex : allocated_tex) {
        bindless_array->UnbindTexture(tex);
    }

    for (auto& callback : on_free_buffer_callbacks) {
        callback(0);
    }

    ReleaseResources();

    if (hooks.on_unregister_renderer_config_section) {
        hooks.on_unregister_renderer_config_section("Raytracing", "Settings");
        hooks.on_unregister_renderer_config_section("Raytracing", "Material Texture");
    }
}

void RaytracingRenderer::DumpTextureToFile(
    ExportConfig&          _config,
    FrameResources&        _frame_rt,
    RenderDevice&          _device,
    CommandQueue&          _gfx_queue,
    std::filesystem::path& _exported_file_path,
    StringView             _suffix
) {

    CommandList cmd_list{};

    auto dequantentize_half = [](short _h, bool _gamma_correct = true) {
        unsigned int s  = unsigned(_h & 0x8000) << 16;
        int          em = _h & 0x7fff;

        // bias exponent and pad mantissa with 0; 112 is relative exponent
        // bias (127-15)
        int r = (em + (112 << 10)) << 13;

        // denormal: flush to zero
        r = (em < (1 << 10)) ? 0 : r;

        // infinity/NaN; note that we preserve NaN payload as a byproduct of
        // unifying inf/nan cases 112 is an exponent bias fixup; since we
        // already applied it once, applying it twice converts 31 to 255
        r += (em >= (31 << 10)) ? (112 << 23) : 0;

        FloatBits u;
        u.ui = s | r;
        if (_gamma_correct) {
            u.f = u.f <= 0.0031308f ? 12.92f * u.f : 1.055f * std::pow(u.f, 1.f / 2.4f) - 0.055f;
        }
        return u.f;
    };

    auto dequantentize_byte_to_srgb = [](unsigned char _b) {
        float c = _b / 255.f;
        c       = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;

        // to byte
        return (unsigned char)(c * 255.f);
    };

    size_t            size;
    Array<Moer::byte> copy_back_data;
    String            file_name = MOER_TEXT("screenshot_");
    bool              hdr       = false;

    uint3 resolution = _frame_rt.ldr_color->GetExtent();
    switch (_config.output_texture) {
        case EOT_LDR: {
            size = sizeof(uint) * resolution.x * resolution.y;
            copy_back_data.resize(size);
            cmd_list.CopyFrom(_frame_rt.ldr_color->GetView(), copy_back_data);
            file_name += _suffix;
            file_name += MOER_TEXT(".png");
            break;
        }
        case EOT_HDR: {
            size = sizeof(float2) * resolution.x * resolution.y;
            copy_back_data.resize(size);
            cmd_list.CopyFrom(_frame_rt.resolved_color->GetView(), copy_back_data);
            file_name += _suffix;
            file_name += MOER_TEXT(".exr");
            hdr = true;
            break;
        }
        default:
            size = 0;
    }
    if (size != 0) {
        FenceRef export_fence = _device.CreateFence();
        cmd_list.Signal(export_fence, 1);
        Array<CommandList> export_cmd_lists{};
        export_cmd_lists.emplace_back(std::move(cmd_list));
        RHIExecutor::Get().Submit(std::move(export_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
        export_fence->Wait(1);
        // Executor-managed fence already guarantees GPU completion.
        if (hdr) {
            // export to exr
            // cast r16g16b16a16_sfloat to r32g32b32a32_sfloat
            assert(
                _frame_rt.resolved_color->GetFormat() == PF_R16G16B16A16_SFLOAT &&
                "resolved color format must be r16g16b16a16_sfloat"
            );
        } else {
            // ldr format must be r8g8b8a8_unorm
            assert(
                _frame_rt.ldr_color->GetFormat() == PF_R8G8B8A8_UNORM && "ldr format must be r8g8b8a8_unorm"
            );
        }
        _config.b_export = false;
        // cast r8g8b8a8_unorm to srgb
        LambdaTask::Create([=,
                            r_copy_back_data(std::move(copy_back_data)),
                            dequantentize_byte_to_srgb(std::move(dequantentize_byte_to_srgb)),
                            dequantentize_half(std::move(dequantentize_half)),
                            &_config]() {
            // free copy back data
            auto copy_back_data = std::move(r_copy_back_data);
            if (hdr) {
                uint          range_cnt = 8;
                uint          range     = copy_back_data.size() / range_cnt;
                Array<float4> copy_back_data_f4(copy_back_data.size() / 8);
                ParallelFor(range_cnt, [&](uint _idx) {
                    size_t start = range * _idx;
                    size_t end   = range * (_idx + 1);
                    for (size_t i = start; i < copy_back_data.size() && i < end; i += 8) {
                        float4* data   = &copy_back_data_f4[i / 8];
                        short*  data_s = (short*)&copy_back_data[i];
                        data->x        = dequantentize_half(data_s[0]);
                        data->y        = dequantentize_half(data_s[1]);
                        data->z        = dequantentize_half(data_s[2]);
                        data->w        = dequantentize_half(data_s[3]);
                    }
                });
                const std::filesystem::path output_path =
                    _exported_file_path / std::filesystem::path(file_name.c_str());
                const auto       native_path = output_path.native();
                const Utf8String output_path_utf8 =
                    PlatformToUtf8(StringView(native_path.data(), native_path.size()));
                stbi_write_hdr(
                    output_path_utf8.c_str(), resolution.x, resolution.y, 4, (float*)copy_back_data_f4.data()
                );
            } else {
                // parallel to 4 threads
                uint range_cnt = 8;
                uint range     = copy_back_data.size() / range_cnt;
                ParallelFor(range_cnt, [&](uint _idx) {
                    size_t start = range * _idx;
                    size_t end   = range * (_idx + 1);
                    for (size_t i = start; i < copy_back_data.size() && i < end; i += 4) {
                        copy_back_data[i] = (Moer::byte)dequantentize_byte_to_srgb(ubyte(copy_back_data[i]));
                        copy_back_data[i + 1] =
                            (Moer::byte)dequantentize_byte_to_srgb(ubyte(copy_back_data[i + 1]));
                        copy_back_data[i + 2] =
                            (Moer::byte)dequantentize_byte_to_srgb(ubyte(copy_back_data[i + 2]));
                        copy_back_data[i + 3] =
                            (Moer::byte)dequantentize_byte_to_srgb(ubyte(copy_back_data[i + 3]));
                    }
                });
                const std::filesystem::path output_path =
                    _exported_file_path / std::filesystem::path(file_name.c_str());
                const auto       native_path = output_path.native();
                const Utf8String output_path_utf8 =
                    PlatformToUtf8(StringView(native_path.data(), native_path.size()));
                stbi_write_png(
                    output_path_utf8.c_str(),
                    resolution.x,
                    resolution.y,
                    4,
                    (void*)copy_back_data.data(),
                    4 * resolution.x
                );
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }).Dispatch();
    }
}

} // namespace Moer::Render::Raytracing
