#include "RaytracingRenderer.h"

// Runtime
#include "PixelFormat.h"
#include "RaytracingConfig.h"
#include "config/ConfigManager.h"
#include "misc/BoundingBox.h"
#include "misc/Timer.h"
#include "renderer/common/RuntimeAssets.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "rhi/extension/NrdExtension.h"
#include "scene/GpuScene.h"
#include "scene/loader/LoaderInterface.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/TaskGraph.h"
#include "window/WindowContext.h"

// Editor
#include "AntiAliasPass.h"
#include "CompositionPass.h"
#include "Configs.h" // TODO: merge it with raster
#include "GBufferPass.h"
#include "LightingPass.h"
#include "PreprocessLightPass.h"
#include "RTResource.h"
#include "ShaderUtils.h"
#include "ToneMappingPass.h"
#include "VisualizePass.h"
#include "renderer/common/UiCombinePass.h"

// 3rd party
#include <atomic>
#include <imgui.h>
#include <stb/stb_image_write.h>

namespace Moer::Render::Raytracing {

union FloatBits {
    float        f;
    unsigned int ui;
};

static Box3D scene_bounding{};

RaytracingRenderer::RaytracingRenderer(
    uint2&                        _resolution,
    const SharedPtr<EditorConfig> _config,
    const EngineHooks&            _hooks,
    RuntimeAssets&                _runtime_assets
) :
    Renderer(_resolution, _config, _hooks),
    runtime_assets(_runtime_assets) {}

void RaytracingRenderer::Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    bool b_new_env_map = false;

    TextureRef         env_map{};
    Array<TextureView> env_mips;
    TextureRef         env_pdf{};
    Array<TextureView> env_pdf_mips;
    TextureWithHandle  env_tex_with_hdl;

    RaytracingSceneRef rt_scene = {}; // 在场景初始化完毕后，才可以被赋值

    ShaderUtils sd_utils(device, manager);

    ImportantSamplingParams is_params{};
    is_params.render_size = resolution;
    ImportanceSamplingContext is_ctx(is_params);

    bool first_load = true;

    UnorderedMap<std::string, TextureWithHandle> material_textures;

    float elapsed_time = 0.0f;

    TextureRef output = device.CreateTexture(
        Extent2D(resolution.x, resolution.y), swapchain->format, ETextureUsageFlags::COLOR_ATTACHMENT
    );

    TextureRef ui_frame_buffer = device.CreateTexture(
        "ui_frame_buffer",
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED
    );

    auto create_frame_buffers = [&](uint2 _new_extent) {
        output = device.CreateTexture(
            "output",
            Extent2D(_new_extent.x, _new_extent.y),
            swapchain->format,
            ETextureUsageFlags::COLOR_ATTACHMENT
        );

        ui_frame_buffer = device.CreateTexture(
            "ui_frame_buffer",
            Extent2D(_new_extent.x, _new_extent.y),
            PF_R8G8B8A8_SRGB,
            ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED
        );
    };

    Timer timer;
    timer.Start();
    uint64 last_time = 0ull;

    bool                  b_feedback_valid               = false;
    bool                  b_export                       = false;
    std::string           selected_material_texture_name = "";
    bool                  b_final_show_texture           = false;
    bool                  b_use_bindless                 = true;
    uint                  mip_level                      = 0;
    std::filesystem::path exported_file_path = ConfigManager::GetInstance().GetWorkspacePath() / "saved";
    if (!std::filesystem::exists(exported_file_path)) {
        std::filesystem::create_directory(exported_file_path);
    }
    //////////////////////////////////////////////////////////////////////////
    // passes
    //////////////////////////////////////////////////////////////////////////
    UniquePtr<PrepareLightPass> prepare_light_pass = MakeUnique<PrepareLightPass>(device, manager, scene);
    UniquePtr<GBufferPass>      g_buffer_pass      = MakeUnique<GBufferPass>(device, manager, scene);
    UniquePtr<LightingPass>     lighting_pass      = MakeUnique<LightingPass>(manager, scene);
    UniquePtr<CompositionPass>  composition_pass   = MakeUnique<CompositionPass>(device, manager, scene);
    UniquePtr<VisualizePass>    visualize_pass     = MakeUnique<VisualizePass>(device, manager);
    UniquePtr<RTContext>        rt_ctx             = MakeUnique<RTContext>(sd_utils, is_ctx, bindless_array);
    UniquePtr<ToneMappingPass>  tone_mapping_pass;
    UniquePtr<UiCombinePass>    ui_combine_pass = MakeUnique<UiCombinePass>(manager);

    rt_ctx->SetResolution(resolution);
    AntialiasPass::CreateInfo antialias_pass_info{
        .motion              = rt_ctx->frame_rt.motion,
        .feedback_color_ping = rt_ctx->frame_rt.feedback_color_ping,
        .feedback_color_pong = rt_ctx->frame_rt.feedback_color_pong,
        .resolved_color      = rt_ctx->frame_rt.resolved_color,
        .hdr_color           = rt_ctx->frame_rt.hdr_color
    };
    UniquePtr<AntialiasPass> antialias_pass =
        MakeUnique<AntialiasPass>(device, manager, scene, antialias_pass_info);

//////////////////////////////////////////////////////////////////////////
// NRD
//////////////////////////////////////////////////////////////////////////
#if WITH_NRD
    uint64 nrd_time      = 0ull;
    auto*  nrd_ext       = device.LoadExtension<Ext::NRDExtension>();
    auto   nrd_interface = nrd_ext->CreateInterface(max_frame_in_flight, resolution.x, resolution.y);
#endif

    VisualizeConfig visualize_config{};
    visualize_config.b_split        = false;
    visualize_config.split_ratio    = 0.5f;
    visualize_config.visualize_mode = EFC_DI;

    Array<std::function<void(uint)>> on_free_buffer_callbacks;

    auto add_on_free_buffer = [&](uint _buffer_handle) {
        on_free_buffer_callbacks.emplace_back([&, _buffer_handle](uint _timeline) {
            bindless_array->FreeBuffer(_buffer_handle);
        });
    };

    auto add_on_free_texture = [&](uint _texture_handle) {
        on_free_buffer_callbacks.emplace_back([&, _texture_handle](uint _timeline) {
            bindless_array->FreeTexture(_texture_handle);
        });
    };

    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {

        RaytracingConfig& ui_config = editor_config->raytracing_config;

        LogSceneLoadStatus(*editor_config);

        auto window_state = TickWindowContext(hooks);

        if (window_state == EWindowState::Hiding) {
            std::this_thread::yield();

            // hook: RenderGUI
            if (hooks.on_render_gui) {
                hooks.on_render_gui(cmd_list, output);
            }

            continue;

        } else if (window_state == EWindowState::SizeChanged) {
            create_frame_buffers(resolution);

            rt_ctx->SetResolution(resolution);

            is_ctx.~ImportanceSamplingContext();
            is_params.render_size = resolution;
            new (&is_ctx) ImportanceSamplingContext(is_params);

#if WITH_NRD
            nrd_interface = nrd_ext->RecreateInterface(std::move(nrd_interface), resolution.x, resolution.y);
#endif

            antialias_pass_info.motion              = rt_ctx->frame_rt.motion;
            antialias_pass_info.feedback_color_ping = rt_ctx->frame_rt.feedback_color_ping;
            antialias_pass_info.feedback_color_pong = rt_ctx->frame_rt.feedback_color_pong;
            antialias_pass_info.resolved_color      = rt_ctx->frame_rt.resolved_color;
            antialias_pass_info.hdr_color           = rt_ctx->frame_rt.hdr_color;
            antialias_pass   = MakeUnique<AntialiasPass>(device, manager, scene, antialias_pass_info);
            b_feedback_valid = false;

        } else if (window_state == EWindowState::Default) {
            // do nothing

        } else {
            assert(false);
        }

        if (hooks.on_tick_ui) {
            hooks.on_tick_ui();
        }

        timer.Stop();
        auto frame_time = timer.ElapsedMilliseconds();
        timer.Start();

        if (scene.IsReady() && runtime_assets.IsReady()) {
            // load scene
            if (first_load) {
                first_load = false;

                b_new_env_map = true;
                // calculate bounding box
                scene_bounding.min = float3(0.f);
                scene_bounding.max = float3(0.f);

                // 遍历所有有 CRenderable 和 CTransform 的 entity，合并它们的 AABB
                scene.r().view<ecs::CRenderable, ecs::CTransform>().each(
                    [&](auto entity_id, const auto& c_renderable, const auto& c_transform) {
                        if (c_transform.d_aabb.IsValid()) {
                            scene_bounding.Expand(c_transform.d_aabb);
                        }
                    }
                );

                // new_cell size
                float3 extent = scene_bounding.max - scene_bounding.min;

                float max_extent                = std::max(std::max(extent.x, extent.y), extent.z);
                float cell_size                 = max_extent * 2 / is_ctx.GetGridConfig().grid_size.x;
                ui_config.grid_config.cell_size = cell_size;

                rt_scene = scene.GetGpuSceneRes().rt_scene;

                rt_ctx->SetBindlessHandles(scene.GetGpuSceneRes());
                rt_ctx->SetRaytracingScene(rt_scene);
                rt_ctx->FillLowDiscrepancySequence(cmd_list);

                cmd_list.UpdateBindlessArray(bindless_array);

                if (hooks.on_register_ui_func) {

                    auto& _gfx_queue = gfx_queue;

                    hooks.on_register_ui_func(
                        "Display MaterialTexture",
                        [&material_textures,
                         &selected_material_texture_name,
                         &b_use_bindless,
                         &b_final_show_texture,
                         &mip_level,
                         &_gfx_queue]() {
                            ImGui::Checkbox("Show Final Texture", &b_final_show_texture);
                            ImGui::SliderInt("Mip Level", (int*)&mip_level, 0, 12);
                            ImGui::Checkbox("Use Bindless", &b_use_bindless);
                            if (ImGui::TreeNode("MaterialTexture")) {
                                for (auto& [name, tex] : material_textures) {
                                    // selectable
                                    if (ImGui::Selectable(
                                            name.data(), selected_material_texture_name == name
                                        )) {
                                        selected_material_texture_name = name;
                                    }
                                }
                                ImGui::TreePop();
                            }

                            //Pass profiling
                            auto entrys = _gfx_queue.GetProfilerEntry();
                            if (!entrys.cpu_entries.empty()) {
                                ImGui::Text("CPU Time:");
                                for (auto& [name, time] : entrys.cpu_entries) {
                                    if (name.ends_with("Percentage")) {
                                        ImGui::Text("%s: %.3f%%", name.c_str(), time * 100);

                                    } else
                                        ImGui::Text("%s: %.3f ms", name.c_str(), time);
                                }
                            }
                            if (!entrys.gpu_entries.empty()) {
                                ImGui::Text("GPU Time:");
                                for (auto& [name, time] : entrys.gpu_entries) {
                                    ImGui::Text("%s: %.3f ms", name.c_str(), time);
                                }
                            }
                        }
                    );
                }

                // cmd_list.UpdateRaytracingScene(rt_scene);
                auto copy_timeline = device.GetCopyQueue().GetFenceHandle();
                gfx_queue.Execute(cmd_list.Submit());
                gfx_queue.Sync();
            }

            if (b_new_env_map) {
                auto src_env_map = runtime_assets.GetDefaultEnvMap();
                env_map          = device.CreateTexture(
                    src_env_map->GetName(),
                    Extent3D(src_env_map->GetExtent()),
                    PF_R16G16B16A16_SFLOAT,
                    ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                    src_env_map->GetNumMips()
                );
                sd_utils.SampleTextureCS(
                    cmd_list, src_env_map->GetView(0, 1), env_map->GetView(0, 1), src_env_map->GetFormat()
                );

                Sampler sampler{SF_CUBIC, SAM_REPEAT};
                env_tex_with_hdl.tex = env_map;
                env_tex_with_hdl.hdl =
                    bindless_array->AllocateTexture(env_map->GetView(0, env_map->GetNumMips()), sampler);

                for (int i = 0; i < env_map->GetNumMips(); ++i) {
                    env_mips.push_back(env_map->GetView(i));
                }
                sd_utils.GenerateMips(cmd_list, env_mips);
                b_new_env_map = false;

                rt_ctx->LoadDefaultResources(runtime_assets);
                rt_ctx->CreateEnvMapResources(env_tex_with_hdl, cmd_list);
            }

            // TODO: 统一update代码
            // 疑问：为什么这段 UpdateRaytracingScene 的代码必须写在这个位置，不能挪到前面去？
            if (scene.IsReady()) {
                for (size_t i = 0; i < rt_scene->GetInstanceCount(); i++) {
                    auto& instance = rt_scene->GetInstance(i);
                    rt_scene->MarkModified(instance.instance_id);
                }
                cmd_list.UpdateRaytracingScene(rt_scene);
            }

            if (!tone_mapping_pass) {
                ToneMappingPass::CreateInfo info{};
                info.color_lut    = rt_ctx->default_res.black_tex;
                tone_mapping_pass = MakeUnique<ToneMappingPass>(device, manager, scene, info);
            }

            auto& camera = scene.GetMainCamera().camera;

            // prepare frame
            {
                rt_ctx->FillLowDiscrepancySequence(cmd_list);
                camera.Tick(editor_config);
            }

            // update light direction from ui data
            {
                b_export = ui_config.export_cfg.b_export;

                auto light_entity = scene.GetMainDirectionalLightEntity();
                if (light_entity != entt::null && scene.r().valid(light_entity) &&
                    scene.r().all_of<ecs::CLightDirectional, ecs::CTransform>(light_entity)) {
                    // 更新方向光的颜色和强度
                    auto& c_light_dir     = scene.r().get<ecs::CLightDirectional>(light_entity);
                    c_light_dir.color     = float3(0.9f, 0.65f, 0.4f);
                    c_light_dir.intensity = ui_config.exposure;

                    // 更新方向光的旋转（从默认方向 float3(0.f, 0.f, -1.f) 到目标方向）
                    auto& c_transform    = scene.r().get<ecs::CTransform>(light_entity);
                    c_transform.rotation = Quaternion(float3(0.f, 0.f, -1.f), -ui_config.sun_direction);
                    c_transform.is_dirty = true;
                    // FIXME: ECS更新框架还没写完，这里代码无法生效
                }
            }

            ToneMappingPass::Params tone_params{};
            AntialiasPass::Params   aa_params{};
            // fill ui data
            {
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

            is_ctx.TickFrame(time);
            visualize_config.visualize_mode = ui_config.final_color;

            uint num_emissive_meshes, num_emissive_triangles;
            prepare_light_pass->CountEmissiveInstances(num_emissive_meshes, num_emissive_triangles);

            static constexpr uint s_mesh_alloc_chunk      = 128;
            static constexpr uint s_triangle_alloc_chunk  = 1024;
            static constexpr uint s_primitive_alloc_chunk = 128;
            rt_ctx->CreateBuffersIfNeeded(
                (num_emissive_meshes + s_mesh_alloc_chunk - 1) & ~(s_mesh_alloc_chunk - 1),
                (num_emissive_triangles + s_triangle_alloc_chunk - 1) & ~(s_triangle_alloc_chunk - 1),
                (scene.GetCpuScene().GetLightCount() + s_primitive_alloc_chunk - 1) &
                    ~(s_primitive_alloc_chunk - 1)
            );
            cmd_list.UpdateBindlessArray(bindless_array);

            rt_ctx->Tick(camera, antialias_pass->GetPixelOffset());

            prepare_light_pass->Process(cmd_list, *rt_ctx);

            g_buffer_pass->Process(cmd_list, *rt_ctx);
            lighting_pass->Process(cmd_list, *rt_ctx);

//////////////////////////////////////////////////////////////////////////
// NRD
//////////////////////////////////////////////////////////////////////////
#if WITH_NRD
            {
                bool          b_current_frame = rt_ctx->b_current_frame;
                const auto&   frame_rt        = rt_ctx->frame_rt;
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
                    // denoise
                    nrd_interface->Begin();
                    nrd_interface->UpdateCommonSettings(
                        nrd_time++,
                        Vector2ui(resolution.x, resolution.y),
                        antialias_pass->GetPixelOffset(),
                        Transpose(camera.GetViewMatrix()),
                        Transpose(camera.GetProjectionMatrix())
                    );

                    // opaque
                    {
                        nrd_interface->SetInput(
                            Ext::NRDInterface::EResourceSlot::MOTION_VECTOR, rt_ctx->frame_rt.motion
                        );
                        nrd_interface->SetInput(
                            Ext::NRDInterface::EResourceSlot::NORMAL_ROUGHNESS, frame_rt.normal_roughness
                        );
                        nrd_interface->SetInput(
                            Ext::NRDInterface::EResourceSlot::VIEW_Z,
                            b_current_frame ? frame_rt.view_depth : frame_rt.prev_view_depth
                        );
                        // nrd_interface->SetInput(Ext::NRDInterface::EResourceSlot::BASECOLOR_METALNESS,
                        // b_current_frame ? frame_rt.diffuse_albedo :
                        // frame_rt.prev_diffuse_albedo);
                        nrd_interface->SetInput(
                            Ext::NRDInterface::EResourceSlot::IN_DIFFUSE, frame_rt.diffuse_lighting
                        );
                        nrd_interface->SetInput(
                            Ext::NRDInterface::EResourceSlot::IN_SPECULAR, frame_rt.specular_lighting
                        );
                        nrd_interface->SetOutput(
                            Ext::NRDInterface::EResourceSlot::OUT_DIFFUSE, frame_rt.denoised_diffuse_lighting
                        );
                        nrd_interface->SetOutput(
                            Ext::NRDInterface::EResourceSlot::OUT_SPECULAR,
                            frame_rt.denoised_specular_lighting
                        );

                        nrd_interface->Denoise(cmd_list, denoiser, "Radiance Denoising");
                    }
                }
            }
#endif

            composition_pass->Process(cmd_list, *rt_ctx);
            antialias_pass->Process(
                cmd_list,
                *rt_ctx,
                aa_params,
                b_feedback_valid,
                rt_ctx->frame_rt.hdr_color,
                rt_ctx->frame_rt.resolved_color
            );
            tone_mapping_pass->Process(
                cmd_list, *rt_ctx, tone_params, rt_ctx->frame_rt.resolved_color, rt_ctx->frame_rt.ldr_color
            );
            visualize_pass->Process(cmd_list, *rt_ctx, visualize_config, bindless_array);
            if (b_final_show_texture && material_textures.contains(selected_material_texture_name)) {
                TextureRef        texture = material_textures[selected_material_texture_name].tex;
                ShowTextureParams show_texture_params{};
                show_texture_params.dst_dim      = resolution;
                show_texture_params.bdls_handle  = material_textures[selected_material_texture_name].hdl;
                show_texture_params.mip_level    = mip_level;
                show_texture_params.use_bindless = b_use_bindless;
                sd_utils.ShowTexture(
                    cmd_list, bindless_array, show_texture_params, texture, rt_ctx->frame_rt.ldr_color
                );
            }
            // copy normal to output
            //  cmd_list.CopyFrom(out_direct_lighting->GetView(),
            //  scene_color->GetView());
            rt_ctx->AdvanceFrame();
            tone_mapping_pass->AdvanceFrame(camera.GetDeltaTime());
            antialias_pass->AdvanceFrame();
            b_feedback_valid = true;

            //////////////////////////////////////////////////////////////////////////
            // handle export
            //////////////////////////////////////////////////////////////////////////

            if (b_export) {
                gfx_queue.Execute(cmd_list.Submit().TickProfiling());
                gfx_queue.Sync();
                DumpTextureToFile(
                    ui_config.export_cfg,
                    rt_ctx->frame_rt,
                    device,
                    gfx_queue,
                    exported_file_path,
                    std::to_string(time)
                );
                b_export = false;
            }
        }

        // rt_scene->MarkModified(0);
        // cmd_list.UpdateRaytracingScene(rt_scene);
        Sampler    linear_sampler{SF_LINEAR, SAM_CLAMP_TO_BORDER};
        TextureRef final_color =
            b_final_show_texture ? rt_ctx->frame_rt.ldr_color : rt_ctx->frame_rt.debug_color;

        if (hooks.on_ui_combine_pass) {
            hooks.on_ui_combine_pass(ui_combine_pass.get(), cmd_list, final_color, ui_frame_buffer, output);
        }

        if (hooks.on_render_gui) {
            hooks.on_render_gui(cmd_list, output);
        }

        if (rt_scene) {
            rt_scene->AdvanceFrame();
        }

        time++;
        gfx_queue.Execute(cmd_list.Submit().Signal(timeline, time).DeleteResources().TickProfiling());
        gfx_queue.Present(swapchain, output);

        if (hooks.on_present_windows) {
            hooks.on_present_windows();
        }
        if (hooks.on_is_need_reload && hooks.on_is_need_reload()) {
            break;
        }
    }

    const auto& allocated_buf = rt_ctx->GetAllocatedBdlsBuf();
    for (auto& buf : allocated_buf) {
        bindless_array->FreeBuffer(buf);
    }

    const auto& allocated_tex = rt_ctx->GetAllocatedBdlsTex();
    for (auto& tex : allocated_tex) {
        bindless_array->FreeTexture(tex);
    }

    for (auto& callback : on_free_buffer_callbacks) {
        callback(0);
    }

    ReleaseResources();

    if (hooks.on_unregister_ui_func) {
        hooks.on_unregister_ui_func("Display MaterialTexture");
    }
}

void RaytracingRenderer::DumpTextureToFile(
    ExportConfig&          _config,
    FrameResources&        _frame_rt,
    RenderDevice&          _device,
    CommandQueue&          _gfx_queue,
    std::filesystem::path& _exported_file_path,
    std::string_view       _suffix
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
    std::string       file_name = "screenshot_";
    bool              hdr       = false;

    uint3 resolution = _frame_rt.ldr_color->GetExtent();
    switch (_config.output_texture) {
        case EOT_LDR: {
            size = sizeof(uint) * resolution.x * resolution.y;
            copy_back_data.resize(size);
            cmd_list.CopyFrom(_frame_rt.ldr_color->GetView(), copy_back_data);
            file_name += _suffix;
            file_name += ".png";
            break;
        }
        case EOT_HDR: {
            size = sizeof(float2) * resolution.x * resolution.y;
            copy_back_data.resize(size);
            cmd_list.CopyFrom(_frame_rt.resolved_color->GetView(), copy_back_data);
            file_name += _suffix;
            file_name += ".exr";
            hdr = true;
            break;
        }
        default:
            size = 0;
    }
    if (size != 0) {
        _gfx_queue.Execute(cmd_list.Submit().TickProfiling());
        _gfx_queue.Sync();
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
                stbi_write_hdr(
                    (_exported_file_path / file_name).generic_string().data(),
                    resolution.x,
                    resolution.y,
                    4,
                    (float*)copy_back_data_f4.data()
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
                stbi_write_png(
                    (_exported_file_path / file_name).generic_string().data(),
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