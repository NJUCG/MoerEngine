#include "RaytracingMain.h"

// Runtime
#include "loader/LoaderInterface.h"
#include "misc/Timer.h"
#include "rhi/RHI.h"
#include "rhi/extension//NrdExtension.h"
#include "scene/CameraManager.h"
#include "scene/EntityManager.h"
#include "scene/Material.h"
#include "scene/MaterialInstance.h"
#include "scene/RenderableManager.h"
#include "scene/light/DirectionalLightComponent.h"
#include "scene/light/LightComponent.h"
#include "scene/light/LightComponentManager.h"
#include "shader/GeometryPassPsoManager.h"
#include "shader/ShaderResourceManager.h"
#include "window/WindowContext.h"

// Editor
#include "AntiAliasPass.h"
#include "CompositionPass.h"
#include "Config.h" // TODO: merge it with raster
#include "GBufferPass.h"
#include "LightingPass.h"
#include "PreprocessLightPass.h"
#include "RTResource.h"
#include "ShaderUtils.h"
#include "ToneMappingPass.h"
#include "VisualizePass.h"
#include "common/UiCombinePass.h"
#include "ui/raytracing_ui/RaytracingUI.h"

// 3rd party
#include <imgui.h>
#include <stb/stb_image_write.h>

namespace Moer::Render::Raytracing {

union FloatBits {
    float        f;
    unsigned int ui;
};
struct RTViewParam {

    Matrix4x4f view2world;
    Matrix4x4f world2view;
    float4     frustum;
    float2     near_far;
    uint2      rect;
    float2     inv_rect;
    float2     jitter;
    float3     dir;
    float      orthomode;
};

struct RTConfigParam {
    Matrix4x4f view2world;
    Matrix4x4f view2clip;
    Matrix4x4f world2view;
    Matrix4x4f world2view_prev;
    Matrix4x4f world2clip;
    Matrix4x4f world2clip_prev;
    float4     nrd_hit_dist_params_dummy;
    float4     sun_direction_gexposure;
    float4     camera_origin_gmipbias;
    float4     view_direction_gorthomode;
    // float4 HairBaseColorOverride; // w is alpha or blend factor
    // float2 HairBetasOverride;
    float2   window_size;
    float2   inv_window_size;
    float2   output_size;
    float2   inv_output_size;
    float2   render_size;
    float2   inv_render_size;
    float2   rect_size;
    float2   inv_rect_size;
    float2   rect_size_prev;
    float2   jitter;
    float    emission_intensity;
    float    separator;
    float    roughness_override;
    float    metalness_override;
    float    unit_to_meters_multiplier;
    float    indirect_diffuse;
    float    indirect_specular;
    float    tan_sun_angular_radius;
    float    tan_pixel_angular_radius;
    float    debug;
    float    transparent;
    float    prev_frame_confidence;
    float    min_probability;
    float    unproject;
    float    aperture;
    float    focal_distance;
    float    focal_length;
    uint32_t denoiser_type;
    uint32_t on_screen;
    uint32_t frame_index;
    uint32_t forced_material;
    uint32_t use_normalmap;
    uint32_t b_worldspace_motion;
    uint32_t tracing_mode;
    uint32_t sample_num;
    uint32_t bounce_num;
    uint32_t taa;
    uint32_t resolve;
    uint32_t psr;
    uint32_t validation;
    uint32_t trim_lobe;
    // uint32_t highlight_ahs;
    // uint32_t ahs_dynamic_mip;

    // Ambient
    float ambient_max_accumulated_frames_num;
    float ambient;
};

class TestInlineRTShader : public ComputePipeline {
public:
    struct Param {
        uint   instance_buffer_handle;
        uint   geometry_buffer_handle;
        uint   material_buffer_handle;
        uint   global_param_handle;
        uint   light_buffer_handle;
        uint2  rect;
        float2 inv_rect;
        float2 jitter;
        uint   frame_idx;
    };
    DEFINE_COMPUTE_PIPELINE_CLASS(TestInlineRTShader);

    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_BUFFER(rt_config);
    DEFINE_SHADER_TEX(out_normal_roughness);
    DEFINE_SHADER_TEX(out_basecolor_metalness);
    DEFINE_SHADER_TEX(out_direct_lighting);
    DEFINE_SHADER_TEX(out_emission);
    DEFINE_SHADER_TEX(out_diffuse);
    DEFINE_SHADER_TEX(out_specular);
    DEFINE_SHADER_TEX(out_view_z);
    DEFINE_SHADER_TEX(out_mv);
    DEFINE_SHADER_TEX(out_shadow_info);
    DEFINE_SHADER_TEX(out_scene_color);

    DEFINE_SHADER_TLAS(tlas);
    DEFINE_SHADER_CONSTANT_STRUCT(Param, param);

    DEFINE_SHADER_ARGS(
        param,
        rt_config,
        out_normal_roughness,
        out_basecolor_metalness,
        out_direct_lighting,
        out_emission,
        out_diffuse,
        out_specular,
        out_view_z,
        out_mv,
        out_shadow_info,
        out_scene_color,
        bdls,
        tlas
    );
};

static Box3D          scene_bounding{};
static constexpr uint max_frame_in_flight = 3;

void RaytracingMain(SharedPtr<EditorUI> editor_ui) {
    // Get a lot of things
    auto&            device              = RenderDevice::Get();
    auto&            manager             = ShaderManager::Get();
    Scene            scene               = {};
    BindlessArrayRef bindless_array      = scene.GetBindlessArray();
    auto&            gfx_queue           = device.GetCommandQueue(EQueueType::Graphics);
    auto&            copy_queue          = device.GetCopyQueue();
    auto             copy_queue_timeline = copy_queue.GetFenceHandle();
    CommandList      cmd_list            = {};

    // Initialize Swapchain
    auto resolution = editor_ui->GetResolution(); // TODO: 是否要从WindowContext中获取resolution?

    auto sc_info = SwapchainCreateInfo{
        .window_handle    = (uintptr_t)WindowContext::GetMainWindow(),
        .size             = {resolution.x, resolution.y},
        .back_buffer_sz   = 2,
        .preferred_format = PF_R8G8B8A8_SRGB
    };
    auto sc = device.CreateSwapchain(sc_info);

    // MARK: Scene
    Resource::LoaderInterface::LoadSceneFromFileAsync(ConfigManager::GetInstance().GetScenePath(), &scene);
    auto&& scope_exit_reset_async_load_info = OnScopeExit([&] { Scene::ResetAsyncLoadInfo(); });

    // TODO: combine RasterMain and RaytracingMain common part (above code)

    RTResource rt_res(ConfigManager::GetInstance().GetEditorResourcePath());
    bool       b_new_env_map = false;

    GraphEventRef load_res_event = LambdaTask::Create([&rt_res, &b_new_env_map]() {
                                       rt_res.LoadResources();
                                       //memory barrier
                                       std::atomic_thread_fence(std::memory_order_acq_rel);
                                       b_new_env_map = true;
                                   }).Dispatch();

    TextureRef         env_map{};
    Array<TextureView> env_mips;
    TextureRef         env_pdf{};
    Array<TextureView> env_pdf_mips;

    RTConfigParam rt_config_param{};

    BufferRef rt_config_param_buffer =
        device.CreateBuffer<Moer::byte>(sizeof(RTConfigParam) * 1, EBufferUsageFlags::CONSTANT_BUFFER);

    rt_config_param_buffer->SetName("rt_config_param_buffer");

    RaytracingSceneRef rt_scene = device.CreateRaytracingScene();

    GfxPsoCreateInfo combine_pso_info(
        RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_SRGB)}
    );
    CombineUIPipeline combine_ui = manager.Raster()
                                       .Vertex("CombineGuiVert.hlsl")
                                       .Pixel("CombineGuiFrag.hlsl")
                                       .Build<CombineUIPipeline>(std::move(combine_pso_info));

    GfxPsoCreateInfo sample_tex_pso_info(
        RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_SRGB)}
    );

    SampleTexturePipeline sample_tex = manager.Raster()
                                           .Vertex("framework/FullScreen.vert.hlsl")
                                           .Pixel("utils/CopyTexture.frag.hlsl")
                                           .Build<SampleTexturePipeline>(std::move(sample_tex_pso_info));

    ShaderUtils sd_utils(device, manager);

    ImportantSamplingParams is_params{};
    is_params.render_size = resolution;
    ImportanceSamplingContext is_ctx(is_params);

    bool   first_load = true;
    uint   instance_buffer_handle;
    uint   geometry_buffer_handle;
    uint   geometry_instance_buffer_handle;
    uint   material_buffer_idx;
    uint64 last_io_change_timeline = 0;
    uint   view_buffer_handle      = 0;

    //gbuffer bdls handle
    uint bdls_tex_handle_uv      = 0;
    uint bdls_tex_handle_normal  = 0;
    uint bdls_tex_handle_vbuffer = 0;
    uint bdls_tex_handle_depth   = 0;

    Array<RaytracingGeometryRef> rt_geometries;

    auto   timeline     = device.CreateFence();
    uint64 time         = 0ull;
    uint64 nrd_time     = 0ull;
    float  elapsed_time = 0.0f;

    TextureRef output = device.CreateTexture(
        Extent2D(resolution.x, resolution.y), PF_R8G8B8A8_SRGB, ETextureUsageFlags::COLOR_ATTACHMENT
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
            PF_R8G8B8A8_SRGB,
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
    //passes
    //////////////////////////////////////////////////////////////////////////
    UniquePtr<PrepareLightPass> prepare_light_pass = MakeUnique<PrepareLightPass>(device, manager, scene);
    UniquePtr<GBufferPass>      g_buffer_pass      = MakeUnique<GBufferPass>(device, manager, scene);
    UniquePtr<LightingPass>     lighting_pass      = MakeUnique<LightingPass>(manager, scene);
    UniquePtr<CompositionPass>  composition_pass   = MakeUnique<CompositionPass>(device, manager, scene);
    UniquePtr<VisualizePass>    visualize_pass     = MakeUnique<VisualizePass>(device, manager);
    UniquePtr<RTContext>        rt_ctx             = MakeUnique<RTContext>(sd_utils, is_ctx, bindless_array);
    UniquePtr<ToneMappingPass>  tone_mapping_pass;
    UniquePtr<UiCombinePass>    ui_combine_pass = MakeUnique<UiCombinePass>(manager, output->GetFormat());

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
    //NRD
    //////////////////////////////////////////////////////////////////////////
    auto* nrd_ext       = device.LoadExtension<Ext::NRDExtension>();
    auto  nrd_interface = nrd_ext->CreateInterface(max_frame_in_flight, resolution.x, resolution.y);

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

    editor_ui->SetShowSubUI(true);

    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        WindowContext::Tick();
        editor_ui->TickUI();
        int w_width, w_height;
        if (time >= max_frame_in_flight) { timeline->Wait(time - max_frame_in_flight); }
        RaytracingConfig& ui_config = editor_ui->m_raytracing_ui.GetEditableConfig();

        timer.Stop();
        auto frame_time = timer.ElapsedMilliseconds();
        timer.Start();

        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w_width, &w_height);
        if (w_width == 0 || w_height == 0) {
            std::this_thread::yield();
            editor_ui->RenderGUI(cmd_list, output);
            continue;
        }
        if (w_width != resolution.x || w_height != resolution.y) {

            resolution = {uint32(w_width), uint32(w_height)};

            gfx_queue.Sync();
            sc_info.size = {resolution.x, resolution.y};
            sc->Recreate(sc_info);

            output = device.CreateTexture(
                "output",
                Extent2D(resolution.x, resolution.y),
                PF_R8G8B8A8_SRGB,
                ETextureUsageFlags::COLOR_ATTACHMENT
            );

            create_frame_buffers(resolution);

            rt_ctx->SetResolution(resolution);

            is_ctx.~ImportanceSamplingContext();
            is_params.render_size = resolution;
            new (&is_ctx) ImportanceSamplingContext(is_params);
            nrd_interface = nrd_ext->RecreateInterface(std::move(nrd_interface), resolution.x, resolution.y);

            antialias_pass_info.motion              = rt_ctx->frame_rt.motion;
            antialias_pass_info.feedback_color_ping = rt_ctx->frame_rt.feedback_color_ping;
            antialias_pass_info.feedback_color_pong = rt_ctx->frame_rt.feedback_color_pong;
            antialias_pass_info.resolved_color      = rt_ctx->frame_rt.resolved_color;
            antialias_pass_info.hdr_color           = rt_ctx->frame_rt.hdr_color;
            antialias_pass   = MakeUnique<AntialiasPass>(device, manager, scene, antialias_pass_info);
            b_feedback_valid = false;
        }

        if (Scene::GetCurrentSceneLoadInfo().Get() && Scene::GetCurrentSceneLoadInfo()->IsReady() &&
            load_res_event->IsComplete()) {
            //load scene
            if (first_load) {
                //calculate bounding box

                scene.ForEach([&](Entity _entity) {
                    auto& mesh = RenderableManager::Get().GetMeshInfo(_entity);
                    scene_bounding.Expand(mesh->bounding_box);
                });

                //new_cell size
                float3 extent = scene_bounding.max - scene_bounding.min;

                float max_extent                = std::max(std::max(extent.x, extent.y), extent.z);
                float cell_size                 = max_extent * 2 / is_ctx.GetGridConfig().grid_size.x;
                ui_config.grid_config.cell_size = cell_size;

                instance_buffer_handle =
                    bindless_array->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::InstanceInfo)->GetView()
                    );
                geometry_buffer_handle =
                    bindless_array->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::GeometryInfo)->GetView()
                    );
                geometry_instance_buffer_handle = bindless_array->AllocateBuffer(
                    scene.GetBuffer(EGpuSceneResource::GeometryInstance)->GetView()
                );
                material_buffer_idx =
                    bindless_array->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::MaterialInfo)->GetView()
                    );

                add_on_free_buffer(instance_buffer_handle);
                add_on_free_buffer(geometry_buffer_handle);
                add_on_free_buffer(geometry_instance_buffer_handle);
                add_on_free_buffer(material_buffer_idx);

                rt_ctx->SetBindlessHandles(
                    geometry_buffer_handle, instance_buffer_handle, material_buffer_idx
                );
                rt_ctx->SetRaytracingScene(rt_scene);
                rt_ctx->FillLowDiscrepancySequence(cmd_list);

                first_load = false;

                cmd_list.UpdateBindlessArray(bindless_array);
                last_io_change_timeline = copy_queue_timeline->GetValue();
                // gfx_queue.Wait({uint64(copy_queue_timeline.Get()), copy_timeline->GetValue()});
                // gfx_queue.Sync();

                rt_geometries.reserve(scene.GetEntityCount());
                Array<AccelerationStructureBuildParam> build_params;
                build_params.reserve(scene.GetEntityCount());

                scene.ForEach([&](Entity _entity) {
                    auto&                          mesh = RenderableManager::Get().GetMeshInfo(_entity);
                    std::span<MaterialInstanceRef> materials =
                        RenderableManager::Get().GetMaterialInstances(_entity);
                    RaytracingGeometryInfo rt_geo_info{};
                    rt_geo_info.build_flags   = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
                    rt_geo_info.vertex_format = PF_R32G32B32_SFLOAT;
                    rt_geo_info.index_type    = IET_UINT32;

                    for (uint i = 0; i < mesh->geometries.size(); i++) {
                        uint vtx_offset = mesh->geometries[i]->local_vtx_offset;
                        uint vtx_count  = mesh->geometries[i]->local_vtx_count;
                        uint idx_offset = mesh->geometries[i]->local_idx_offset;
                        uint idx_count  = mesh->geometries[i]->local_idx_count;
                        auto vtx_buffer = mesh->geometries[i]->mesh_buffers->vertex_buffer;
                        auto idx_buffer = mesh->geometries[i]->mesh_buffers->index_buffer;

                        //evaluate material
                        MaterialInstanceRef mat_instance = materials[i];
                        mat_instance->GetMaterial()->GetType();

                        rt_geo_info.segments.emplace_back(
                            0,                                         // vertex_offset
                            0,                                         // index_offset
                            vtx_offset,                                // first_vertex
                            vtx_count,                                 // vertex_count
                            sizeof(float3),                            // vertex_stride
                            idx_offset / 3,                            // first_primitive
                            idx_count / 3,                             // primitive_count
                            vtx_buffer,                                // vertex_buffer
                            idx_buffer,                                // index_buffer
                            RTGT_TRIANGLES,                            // type
                            ERayTracingGeometryFlags::GEOMETRY_OPAQUE, // flags
                            false,                                     // b_force_opaque
                            false,                                     // b_cull_back_face
                            false                                      // b_flip_face
                        );
                    }

                    RaytracingGeometryRef blas = device.CreateRaytracingGeometry(rt_geo_info);
                    rt_geometries.push_back(blas);

                    auto& instance     = rt_scene->AddInstance();
                    instance.geom      = blas;
                    instance.transform = TransformManager::Get().Get(_entity).GetMatrix3x4();

                    instance.flag.need_create = true;
                    instance.custom_index     = instance.instance_id;
                    instance.visible_mask     = RTVM_ALL;
                    rt_scene->MarkModified(instance.instance_id);
                    build_params.push_back({blas, ERaytracingBuildMode::BUILD});
                });

                cmd_list.BuildAccelerationStructures(std::move(build_params));

                last_io_change_timeline = copy_queue_timeline->GetValue();

                {
                    static bool first_load = true;
                    if (first_load) {
                        first_load = false;
                        Array<ImportTexture> sampled_textures;
                        sampled_textures.reserve((scene.GetGpuScene().material_textures.size()));

                        for (auto& [name, tex] : scene.GetGpuScene().material_textures) {
                            sampled_textures.emplace_back(ImportTexture(
                                tex.texture->GetView(0, tex.texture->GetNumMips()), ETextureState::SAMPLE
                            ));
                        }

                        cmd_list.ImportTextureFromQueue(EQueueType::Copy, std::move(sampled_textures));
                    }
                    cmd_list.UpdateRaytracingScene(rt_scene);
                }

                editor_ui->RegisterUIFunc(
                    "Display MaterialTexture",
                    [&scene,
                     &selected_material_texture_name,
                     &b_use_bindless,
                     &b_final_show_texture,
                     &mip_level]() {
                        ImGui::Checkbox("Show Final Texture", &b_final_show_texture);
                        ImGui::SliderInt("Mip Level", (int*)&mip_level, 0, 12);
                        ImGui::Checkbox("Use Bindless", &b_use_bindless);
                        if (ImGui::TreeNode("MaterialTexture")) {
                            for (auto& [name, tex] : scene.GetGpuScene().material_textures) {
                                //selectable
                                if (ImGui::Selectable(name.data(), selected_material_texture_name == name)) {
                                    selected_material_texture_name = name;
                                }
                            }
                            ImGui::TreePop();
                        }
                    }
                );

                // cmd_list.UpdateRaytracingScene(rt_scene);
                gfx_queue.Execute(cmd_list.Submit().Wait(copy_queue_timeline, last_io_change_timeline));
                gfx_queue.Sync();
            }

            if (b_new_env_map) {

                {
                    static bool first_load = true;
                    if (first_load) {
                        first_load = false;
                        Array<ImportTexture> import_textures;
                        const auto&          rt_res_textures = rt_res.GetTextures();
                        for (auto& [name, tex] : rt_res_textures) {
                            import_textures.emplace_back(
                                ImportTexture(tex->GetView(0, tex->GetNumMips()), ETextureState::SAMPLE)
                            );
                        }
                        cmd_list.ImportTextureFromQueue(EQueueType::Copy, std::move(import_textures));
                    }
                    gfx_queue.Execute(
                        cmd_list.Submit().Wait(copy_queue_timeline, copy_queue_timeline->GetValue())
                    );
                    gfx_queue.Sync();
                }
                env_map                                  = rt_res.GetDefaultEnvMap();
                auto                             cur_env = scene.GetCurrentEnvMap();
                Moer::EnvironmentLightComponent* env_light =
                    MoerNew(Moer::EnvironmentLightComponent)(float3(1.f), env_map->GetExtent().xy);

                Sampler sampler{SF_CUBIC, SAM_REPEAT};
                env_light->bdls_handle =
                    bindless_array->AllocateTexture(env_map->GetView(0, env_map->GetNumMips()), sampler);
                add_on_free_texture(env_light->bdls_handle);

                Entity env_entity;

                //replace entity
                if (cur_env.texture != nullptr) {
                    assert(env_map && "env map is null");
                    env_entity = cur_env.entity;
                    LightComponentManager::Get().Put(env_entity, env_light);

                } else {
                    env_entity = EntityManager::Get().Create();
                    LightComponentManager::Get().Put(env_entity, env_light);

                    scene.AddLight(env_entity);
                }
                EnvMapResource env_tex{env_map, env_light->bdls_handle, env_entity};
                scene.SetCurrentEnvMap(env_tex);

                env_map = scene.GetCurrentEnvMap().texture;

                for (int i = 0; i < env_map->GetNumMips(); ++i) { env_mips.push_back(env_map->GetView(i)); }
                sd_utils.GenerateMips(cmd_list, env_mips);
                b_new_env_map = false;

                rt_ctx->LoadDefaultResources(rt_res);
                rt_ctx->CreateEnvMapResources(scene.GetCurrentEnvMap(), cmd_list);
            }

            if (Scene::GetCurrentSceneLoadInfo().Get() && Scene::GetCurrentSceneLoadInfo()->IsReady()) {
                for (size_t i = 0; i < scene.GetEntityCount(); i++) {
                    auto& instance = rt_scene->GetInstance(i);
                    // instance.transform = TransformManager::Get().Get(scene.GetEntities()[i]).GetMatrix3x4();
                    rt_scene->MarkModified(instance.instance_id);
                }
                cmd_list.UpdateRaytracingScene(rt_scene);
            }
            if (!tone_mapping_pass) {
                ToneMappingPass::CreateInfo info{};
                info.color_lut    = rt_ctx->default_res.black_tex;
                tone_mapping_pass = MakeUnique<ToneMappingPass>(device, manager, scene, info);
            }
            auto camera_entity = scene.GetCameras()[0];
            auto camera        = CameraManager::Get().Get(camera_entity);
            //prepare frame
            {
                rt_config_param.world2view_prev = Transpose(camera->GetViewMatrix());
                rt_config_param.world2clip_prev =
                    Transpose(camera->GetProjectionMatrix() * camera->GetViewMatrix());

                rt_ctx->FillLowDiscrepancySequence(cmd_list);
                camera->Tick();

                rt_config_param.view2world = Transpose(camera->GetToWorldMatrix());
                rt_config_param.view2clip  = Transpose(camera->GetProjectionMatrix());
                rt_config_param.world2view = Transpose(camera->GetViewMatrix());
                rt_config_param.world2clip = Transpose(camera->GetViewProjectionMatrix());

                rt_config_param.tan_pixel_angular_radius = tanf(Angle::DegreeToRadian(camera->GetFov()));
                rt_config_param.tan_sun_angular_radius =
                    tanf(Angle::DegreeToRadian(ui_config.sun_angular_diameter * 0.5f));
                rt_config_param.bounce_num              = ui_config.max_bounce;
                float3 sun_dir                          = Normalizef(ui_config.sun_direction);
                rt_config_param.sun_direction_gexposure = float4(sun_dir, ui_config.exposure);
                cmd_list.CopyFrom(
                    std::span<Moer::byte>((Moer::byte*)&rt_config_param, sizeof(RTConfigParam)),
                    rt_config_param_buffer->GetView()
                );
            }

            //update light direction from ui data
            {
                b_export = ui_config.export_cfg.b_export;

                auto light_entity = scene.GetLights()[0];
                auto light        = LightComponentManager::Get().Get(light_entity);
                if (light->GetType() == ELightComponentType::DIRECTIONAL) {
                    DirectionalLightComponent* dir_light =
                        static_cast<DirectionalLightComponent*>(light.Get());
                    dir_light->SetDirection(-ui_config.sun_direction);
                    dir_light->SetIntensity(ui_config.exposure);
                    dir_light->SetColor(float3(0.9f, 0.65f, 0.4f));
                }
            }
            ToneMappingPass::Params tone_params{};
            AntialiasPass::Params   aa_params{};
            //fill ui data
            {
                auto        grid_cfg           = is_ctx.GetGridChangableConfig();
                const auto& ui_cfg             = ui_config;
                grid_cfg.cell_size             = ui_config.grid_config.cell_size;
                grid_cfg.center                = camera->GetPosition();
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

                const auto& aa_cfg             = ui_config.aa_cfg;
                aa_params.clamping_factor      = aa_cfg.clamping_factor;
                aa_params.new_frame_weight     = aa_cfg.new_frame_weight;
                aa_params.max_radiance         = aa_cfg.max_radiance;
                aa_params.enable_history_clamp = aa_cfg.enable_history_clamping;

                antialias_pass->SetJitter(aa_cfg.jitter_mode);
            }

            is_ctx.TickFrame(time);
            visualize_config.visualize_mode = ui_config.final_color;

            uint num_emissive_meshes, num_emissive_triangles;
            prepare_light_pass->CountEmissiveInstances(num_emissive_meshes, num_emissive_triangles);

            rt_ctx->CreateBuffersIfNeeded(
                num_emissive_meshes,
                num_emissive_triangles,
                scene.GetLights().size(),
                scene.GetGeometryInstances().size()
            );
            cmd_list.UpdateBindlessArray(bindless_array);

            rt_ctx->Tick(camera, antialias_pass->GetPixelOffset());

            prepare_light_pass->Process(cmd_list, *rt_ctx);

            g_buffer_pass->Process(cmd_list, *rt_ctx);
            lighting_pass->Process(cmd_list, *rt_ctx);

            //////////////////////////////////////////////////////////////////////////
            //NRD
            //////////////////////////////////////////////////////////////////////////
            {
                bool          b_current_frame = rt_ctx->b_current_frame;
                const auto&   frame_rt        = rt_ctx->frame_rt;
                nrd::Denoiser denoiser        = nrd::Denoiser::MAX_NUM;
                switch (ui_config.denoiser_cfg.denoiser_type) {
                    case s_denoiser_mode_reblur: denoiser = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
                    case s_denoiser_mode_relax: denoiser = nrd::Denoiser::RELAX_DIFFUSE_SPECULAR; break;
                }

                if (denoiser != nrd::Denoiser::MAX_NUM) {
                    // denoise
                    nrd_interface->Begin();
                    nrd_interface->UpdateCommonSettings(
                        nrd_time++,
                        Vector2ui(resolution.x, resolution.y),
                        antialias_pass->GetPixelOffset(),
                        Transpose(camera->GetViewMatrix()),
                        Transpose(camera->GetProjectionMatrix())
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
                        // nrd_interface->SetInput(Ext::NRDInterface::EResourceSlot::BASECOLOR_METALNESS, b_current_frame ? frame_rt.diffuse_albedo : frame_rt.prev_diffuse_albedo);
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
            if (b_final_show_texture &&
                scene.GetGpuScene().material_textures.contains(selected_material_texture_name)) {
                TextureRef texture =
                    scene.GetGpuScene().material_textures[selected_material_texture_name].texture;
                ShowTextureParams show_texture_params{};
                show_texture_params.dst_dim = resolution;
                show_texture_params.bdls_handle =
                    scene.GetGpuScene().material_textures[selected_material_texture_name].bindless_handle;
                show_texture_params.mip_level    = mip_level;
                show_texture_params.use_bindless = b_use_bindless;
                sd_utils.ShowTexture(
                    cmd_list, bindless_array, show_texture_params, texture, rt_ctx->frame_rt.ldr_color
                );
            }
            //copy normal to output
            // cmd_list.CopyFrom(out_direct_lighting->GetView(), scene_color->GetView());
            rt_ctx->AdvanceFrame();
            tone_mapping_pass->AdvanceFrame(camera->GetDeletaTime());
            antialias_pass->AdvanceFrame();
            b_feedback_valid = true;

            //////////////////////////////////////////////////////////////////////////
            //handle export
            //////////////////////////////////////////////////////////////////////////
            {
                auto dequantentize_half = [](short _h, bool _gamma_correct = true) {
                    unsigned int s  = unsigned(_h & 0x8000) << 16;
                    int          em = _h & 0x7fff;

                    // bias exponent and pad mantissa with 0; 112 is relative exponent bias (127-15)
                    int r = (em + (112 << 10)) << 13;

                    // denormal: flush to zero
                    r = (em < (1 << 10)) ? 0 : r;

                    // infinity/NaN; note that we preserve NaN payload as a byproduct of unifying inf/nan cases
                    // 112 is an exponent bias fixup; since we already applied it once, applying it twice converts 31 to 255
                    r += (em >= (31 << 10)) ? (112 << 23) : 0;

                    FloatBits u;
                    u.ui = s | r;
                    if (_gamma_correct) {
                        u.f = u.f <= 0.0031308f ? 12.92f * u.f : 1.055f * std::pow(u.f, 1.f / 2.4f) - 0.055f;
                    }
                    return u.f;
                };

                auto dequantentize_byte = [](unsigned char _b) { return _b / 255.f; };

                auto dequantentize_byte_to_srgb = [](unsigned char _b) {
                    float c = _b / 255.f;
                    c       = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;

                    //to byte
                    return (unsigned char)(c * 255.f);
                };

                if (b_export) {

                    size_t            size;
                    Array<Moer::byte> copy_back_data;
                    std::string       file_name = "exported_";
                    bool              hdr       = false;

                    switch (ui_config.export_cfg.output_texture) {
                        case EOT_LDR: {
                            size = sizeof(uint) * resolution.x * resolution.y;
                            copy_back_data.resize(size);
                            cmd_list.CopyFrom(rt_ctx->frame_rt.ldr_color->GetView(), copy_back_data);
                            file_name += std::to_string(time) + ".png";
                            break;
                        }
                        case EOT_HDR: {
                            size = sizeof(float2) * resolution.x * resolution.y;
                            copy_back_data.resize(size);
                            cmd_list.CopyFrom(rt_ctx->frame_rt.resolved_color->GetView(), copy_back_data);
                            file_name += std::to_string(time) + ".exr";
                            hdr = true;
                            break;
                        }
                        default: size = 0;
                    }
                    if (size != 0) {
                        gfx_queue.Execute(
                            cmd_list.Submit().Wait(copy_queue_timeline, copy_queue_timeline->GetValue())
                        );
                        gfx_queue.Sync();

                        if (hdr) {
                            //export to exr
                            //cast r16g16b16a16_sfloat to r32g32b32a32_sfloat
                            assert(
                                rt_ctx->frame_rt.resolved_color->GetFormat() == PF_R16G16B16A16_SFLOAT &&
                                "resolved color format must be r16g16b16a16_sfloat"
                            );
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
                                (exported_file_path / file_name).generic_string().data(),
                                resolution.x,
                                resolution.y,
                                4,
                                (float*)copy_back_data_f4.data()
                            );
                        } else {
                            //ldr format must be r8g8b8a8_unorm
                            assert(
                                rt_ctx->frame_rt.ldr_color->GetFormat() == PF_R8G8B8A8_UNORM &&
                                "ldr format must be r8g8b8a8_unorm"
                            );
                            //cast r8g8b8a8_unorm to srgb
                            //parallel to 4 threads
                            uint range_cnt = 8;
                            uint range     = copy_back_data.size() / range_cnt;
                            ParallelFor(range_cnt, [&](uint _idx) {
                                size_t start = range * _idx;
                                size_t end   = range * (_idx + 1);
                                for (size_t i = start; i < copy_back_data.size() && i < end; i += 4) {
                                    copy_back_data[i] =
                                        (Moer::byte)dequantentize_byte_to_srgb(ubyte(copy_back_data[i]));
                                    copy_back_data[i + 1] =
                                        (Moer::byte)dequantentize_byte_to_srgb(ubyte(copy_back_data[i + 1]));
                                    copy_back_data[i + 2] =
                                        (Moer::byte)dequantentize_byte_to_srgb(ubyte(copy_back_data[i + 2]));
                                    copy_back_data[i + 3] =
                                        (Moer::byte)dequantentize_byte_to_srgb(ubyte(copy_back_data[i + 3]));
                                }
                            });
                            stbi_write_png(
                                (exported_file_path / file_name).generic_string().data(),
                                resolution.x,
                                resolution.y,
                                4,
                                (void*)copy_back_data.data(),
                                4 * resolution.x
                            );
                            // stbi_write_bmp((exported_file_path / file_name).generic_string().data(), resolution.x, resolution.y, 4, (void*)copy_back_data.data());
                        }
                    }

                    b_export                      = false;
                    ui_config.export_cfg.b_export = false;
                }
            }
        }

        // rt_scene->MarkModified(0);
        // cmd_list.UpdateRaytracingScene(rt_scene);
        Sampler    linear_sampler{SF_LINEAR, SAM_CLAMP_TO_BORDER};
        TextureRef final_color =
            b_final_show_texture ? rt_ctx->frame_rt.ldr_color : rt_ctx->frame_rt.debug_color;

        ui_combine_pass->Process(cmd_list, resolution, final_color, ui_frame_buffer, output, editor_ui);

        editor_ui->RenderGUI(cmd_list, output);

        // {
        //     for (uint i = 0; i < env_map->GetNumMips(); i += 5) {
        //         BuildMipsParam param{};
        //         param.num_mip_levels = std::min(5u, env_map->GetNumMips() - i);
        //         param.src_mip_level  = i;
        //         param.src_size       = uint2(env_map->GetExtent().x >> (i + 1), env_map->GetExtent().y >> (i + 1));
        //         cmd_list.Compute(sd_generate_mips, std::span<TextureView>(env_mips.data(), env_mips.size()), param).Dispatch(uint3(env_map->GetExtent().x, env_map->GetExtent().y, 1));
        //     }
        // }
        rt_scene->AdvanceFrame();

        time++;
        gfx_queue.Execute(cmd_list.Submit().Signal(timeline, time));
        gfx_queue.Present(sc, output);
        editor_ui->PresentWindows();

        if (editor_ui->GetConfig().selected_render_method != ERenderMethod::Raytracing) { break; }
    }

    const auto& allocated_buf = rt_ctx->GetAllocatedBdlsBuf();
    for (auto& buf : allocated_buf) { bindless_array->FreeBuffer(buf); }

    const auto& allocated_tex = rt_ctx->GetAllocatedBdlsTex();
    for (auto& tex : allocated_tex) { bindless_array->FreeTexture(tex); }

    for (auto& callback : on_free_buffer_callbacks) { callback(copy_queue_timeline->GetValue()); }

    cmd_list.UpdateBindlessArray(bindless_array);
    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    editor_ui->UnregisterUIFunc("Display MaterialTexture");
}

} // namespace Moer::Render::Raytracing