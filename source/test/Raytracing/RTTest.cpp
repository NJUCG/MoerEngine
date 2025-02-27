#include <atomic>
#include <filesystem>
#include "Configs.h"
#include "Core.h"
#include "GBufferPass.h"
#include "LightingPass.h"
#include "PixelFormat.h"
#include "PreprocessLightPass.h"
#include "RTResource.h"
#include "VisualizePass.h"
#include "config/ConfigManager.h"
#include "contrib/Open3DGC/o3dgcTimer.h"
#include "imgui.h"
#include "math/Matrix.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "platform/Platform.h"
#include "renderer/UIRenderer.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/EntityManager.h"
#include "scene/Scene.h"
#include "scene/TransformManager.h"
#include "scene/light/DirectionalLightComponent.h"
#include "scene/light/LightComponent.h"
#include "scene/light/LightComponentManager.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"
#include "loader/LoaderInterface.h"
#include "misc/Timer.h"
#include "scene/CameraManager.h"
#include "scene/Material.h"
#include "scene/RenderableManager.h"
#include "scene/Scene.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"
#include "shaderheaders/shared/utils/ShaderParameters.h"

#include "RTUI.h"
#include "ShaderUtils.h"

using namespace Moer::Render;
using namespace Moer;

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

    DEFINE_SHADER_ARGS(param, rt_config, out_normal_roughness, out_basecolor_metalness, out_direct_lighting, out_emission, out_diffuse, out_specular, out_view_z, out_mv, out_shadow_info, out_scene_color, bdls, tlas);
};

class CombineUIPipeline : public RasterPipeline {

public:
    struct Param {
        float2 min_xy;
        float2 max_xy;
    };

    DEFINE_RASTER_PIPELINE_CLASS(CombineUIPipeline);
    DEFINE_SHADER_TEX(scene_color);
    DEFINE_SHADER_TEX(gui_color);
    DEFINE_SHADER_SAMPLER(linear_sampler);
    DEFINE_SHADER_CONSTANT_STRUCT(Param, scene_rect);

    DEFINE_SHADER_ARGS(scene_color, gui_color, linear_sampler, scene_rect);
};

class SampleTexturePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SampleTexturePipeline);
    DEFINE_SHADER_TEX(src_color);
    DEFINE_SHADER_SAMPLER(spl);

    DEFINE_SHADER_ARGS(src_color, spl);
};

static Box3D scene_bounding{};

int main(int argc, const char** argv) {

    using namespace Moer::Render;
    using namespace Moer;
    std::filesystem::path path = argv[0];
    path.filename().string().find(".exe") != std::string::npos ? path = path.parent_path() : path = path;
    ConfigManager::GetInstance().Init(path);
    TaskSystem::Init();
    const auto& rhi_config_as_json = ConfigManager::GetInstance().GetRHIConfigAsJSON();

    DeviceInitInfo info{
        .type           = ERHIType::Vulkan,
        .name           = "RHITest",
        .config_as_json = rhi_config_as_json};
    RenderDevice::Init(std::move(info));
    auto&           device = RenderDevice::Get();
    ShaderManager   manager(device);
    uint2           resolution = {1920, 1080};
    SurfaceInitInfo surface_info("Vulkan", resolution.x, resolution.y, "RaytracingTest", false);
    WindowContext::Init(surface_info);
    auto&& scope_exit    = OnScopeExit([&] {
        WindowContext::ShutDown();
        RenderDevice::Dispose();
        TaskSystem::ShutDown();
    });
    auto*  window_handle = WindowContext::GetMainWindow();

    SwapchainCreateInfo      sc_info{.window_handle = (uintptr_t)window_handle, .size = {resolution.x, resolution.y}, .back_buffer_sz = 3, .preferred_format = PF_R8G8B8A8_SRGB};
    auto                     sc         = device.CreateSwapchain(sc_info);
    auto&                    gfx_queue  = device.GetCommandQueue(EQueueType::Graphics);
    auto&                    copy_queue = device.GetCopyQueue();
    Moer::Render::UIRenderer gui(device);

    Scene g_scene{};
    Resource::LoaderInterface::LoadSceneFromFileAsync(ConfigManager::GetInstance().GetScenePath(), &g_scene);
    auto&&     load_scene_scope = OnScopeExit([&] {
        Scene::ResetAsyncLoadInfo();
    });
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
    Array<TextureView>
        env_pdf_mips;

    bool b_new_env_pdf = false;

    BindlessArrayRef bindless_array = g_scene.GetBindlessArray();

    CommandList cmd_list;

    struct Vertex {
        float3 pos;
        float3 normal;
        float3 tangent;
        float2 uv;
    };

    RTViewParam   rt_view_param{};
    RTConfigParam rt_config_param{};

    BufferRef rt_view_param_buffer   = device.CreateBuffer<Moer::byte>(sizeof(RTViewParam) * 1, EBufferUsageFlags::UNORDERED_ACCESS);
    BufferRef rt_config_param_buffer = device.CreateBuffer<Moer::byte>(sizeof(RTConfigParam) * 1, EBufferUsageFlags::CONSTANT_BUFFER);

    rt_view_param_buffer->SetName("rt_view_param_buffer");
    rt_config_param_buffer->SetName("rt_config_param_buffer");

    RaytracingSceneRef rt_scene  = device.CreateRaytracingScene();
    TestInlineRTShader rt_shader = manager.Compute<TestInlineRTShader>("hwrt/InlineRayTracing.hlsl");

    GfxPsoCreateInfo  combine_pso_info(RHIRasterizeInfo::Preset(),
                                       {},
                                       {RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_SRGB)});
    CombineUIPipeline combine_ui = manager
                                       .Raster()
                                       .Vertex("CombineGuiVert.hlsl")
                                       .Pixel("CombineGuiFrag.hlsl")
                                       .Build<CombineUIPipeline>(std::move(combine_pso_info));

    GfxPsoCreateInfo sample_tex_pso_info(RHIRasterizeInfo::Preset(),
                                         {},
                                         {RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_SRGB)});

    SampleTexturePipeline sample_tex = manager
                                           .Raster()
                                           .Vertex("framework/FullScreen.vert.hlsl")
                                           .Pixel("utils/CopyTexture.frag.hlsl")
                                           .Build<SampleTexturePipeline>(std::move(sample_tex_pso_info));

    ShaderUtils sd_utils(device, manager);

    ImportantSamplingParams is_params{};
    is_params.render_size = resolution;
    ImportanceSamplingContext
        is_ctx(is_params);

    auto copy_queue_timeline = copy_queue.GetFenceHandle();

    bool   first_load = true;
    uint   instance_buffer_handle;
    uint   geometry_buffer_handle;
    uint   geometry_instance_buffer_handle;
    uint   material_buffer_idx;
    uint   light_buffer_handle     = 0;
    uint64 last_io_change_timeline = 0;
    uint   view_buffer_handle      = 0;

    //gbuffer bdls handle
    uint bdls_tex_handle_uv      = 0;
    uint bdls_tex_handle_normal  = 0;
    uint bdls_tex_handle_vbuffer = 0;
    uint bdls_tex_handle_depth   = 0;

    Array<RaytracingGeometryRef> rt_geometries;

    auto   timeline = device.CreateFence();
    uint64 time     = 0ull;

    TextureRef output = device.CreateTexture(
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT);

    TextureRef ui_frame_buffer = device.CreateTexture(
        "ui_frame_buffer",
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);

    TextureRef out_normal_roughness = device.CreateTexture(
        "out_normal_roughness",
        Extent3D(resolution.x, resolution.y),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);

    TextureRef out_basecolor_metalness = device.CreateTexture(
        "out_basecolor_metalness",
        Extent3D(resolution.x, resolution.y),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);

    TextureRef out_direct_lighting = device.CreateTexture(
        "out_direct_lighting",
        Extent2D(resolution.x, resolution.y),
        PF_B10G11R11_UFLOAT_PACK32,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);

    TextureRef scene_color = device.CreateTexture(
        "scene_color",
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);

    TextureRef out_emission    = device.CreateTexture("out_emission", Extent2D(resolution.x, resolution.y), PF_B10G11R11_UFLOAT_PACK32, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
    TextureRef out_diffuse     = device.CreateTexture("out_diffuse", Extent2D(resolution.x, resolution.y), PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
    TextureRef out_specular    = device.CreateTexture("out_specular", Extent2D(resolution.x, resolution.y), PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
    TextureRef out_view_z      = device.CreateTexture("out_view_z", Extent2D(resolution.x, resolution.y), PF_R32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
    TextureRef out_shadow_info = device.CreateTexture("out_shadow_info", Extent2D(resolution.x, resolution.y), PF_R32G32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
    TextureRef out_mv          = device.CreateTexture("out_mv", Extent2D(resolution.x, resolution.y), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);

    auto create_frame_buffers = [&](uint2 _new_extent) {
        output = device.CreateTexture(
            "output",
            Extent2D(_new_extent.x, _new_extent.y),
            PF_R8G8B8A8_SRGB,
            ETextureUsageFlags::COLOR_ATTACHMENT);

        out_normal_roughness = device.CreateTexture(
            "out_normal_roughness",
            Extent2D(_new_extent.x, _new_extent.y),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);

        out_basecolor_metalness = device.CreateTexture(
            "out_basecolor_metalness",
            Extent2D(_new_extent.x, _new_extent.y),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);

        out_direct_lighting = device.CreateTexture(
            "out_direct_lighting",
            Extent2D(_new_extent.x, _new_extent.y),
            PF_B10G11R11_UFLOAT_PACK32,
            ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);

        scene_color = device.CreateTexture(
            "scene_color",
            Extent2D(_new_extent.x, _new_extent.y),
            PF_R8G8B8A8_SRGB,
            ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);

        ui_frame_buffer = device.CreateTexture(
            "ui_frame_buffer",
            Extent2D(_new_extent.x, _new_extent.y),
            PF_R8G8B8A8_SRGB,
            ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);

        out_emission    = device.CreateTexture("out_emission", Extent2D(_new_extent.x, _new_extent.y), PF_B10G11R11_UFLOAT_PACK32, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        out_diffuse     = device.CreateTexture("out_diffuse", Extent2D(_new_extent.x, _new_extent.y), PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        out_specular    = device.CreateTexture("out_specular", Extent2D(_new_extent.x, _new_extent.y), PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        out_view_z      = device.CreateTexture("out_view_z", Extent2D(_new_extent.x, _new_extent.y), PF_R32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        out_shadow_info = device.CreateTexture("out_shadow_info", Extent2D(_new_extent.x, _new_extent.y), PF_R32G32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        out_mv          = device.CreateTexture("out_mv", Extent2D(_new_extent.x, _new_extent.y), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
    };

    RTUI rt_ui{gui};

    Timer timer;
    timer.Start();
    uint64 last_time = 0ull;

    //////////////////////////////////////////////////////////////////////////
    //passes
    //////////////////////////////////////////////////////////////////////////
    UniquePtr<PrepareLightPass> prepare_light_pass = MakeUnique<PrepareLightPass>(device, manager, g_scene);
    UniquePtr<GBufferPass>      g_buffer_pass      = MakeUnique<GBufferPass>(device, manager, g_scene);
    UniquePtr<LightingPass>     lighting_pass      = MakeUnique<LightingPass>(manager, g_scene);
    UniquePtr<VisualizePass>    visualize_pass     = MakeUnique<VisualizePass>(device, manager);
    UniquePtr<RTContext>        rt_ctx             = MakeUnique<RTContext>(sd_utils, is_ctx, bindless_array);
    rt_ctx->SetResolution(resolution);

    VisualizeConfig visualize_config{};
    visualize_config.b_split        = false;
    visualize_config.split_ratio    = 0.5f;
    visualize_config.visualize_mode = EFC_DI;

    while (WindowContext::ShouldClose(window_handle) == false) {
        WindowContext::Tick();
        gui.BeginGUIFrame();
        {
            rt_ui.TickUI();
        }
        gui.EndGUIFrame();
        int w_width, w_height;
        if (time >= 3) {
            timeline->Wait(time - 2);
        }
        timer.Stop();
        auto frame_time = timer.ElapsedMilliseconds();
        timer.Start();
        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w_width, &w_height);
        if (w_width == 0 || w_height == 0) {
            std::this_thread::yield();
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
                ETextureUsageFlags::COLOR_ATTACHMENT);

            create_frame_buffers(resolution);
            if (rt_ctx)
                rt_ctx->SetResolution(resolution);
            is_ctx.~ImportanceSamplingContext();
            is_params.render_size = resolution;
            new (&is_ctx) ImportanceSamplingContext(is_params);
        }

        if (Scene::GetCurrentSceneLoadInfo().Get() && Scene::GetCurrentSceneLoadInfo()->IsReady()) {

            //load scene
            if (first_load) {
                //calculate bounding box

                g_scene.ForEach([&](Entity _entity) {
                    auto& mesh = RenderableManager::Get().GetMeshInfo(_entity);
                    scene_bounding.Expand(mesh->bounding_box);
                });

                //new_cell size
                float3 extent = scene_bounding.max - scene_bounding.min;

                float max_extent                        = std::max(std::max(extent.x, extent.y), extent.z);
                float cell_size                         = max_extent * 2 / is_ctx.GetGridConfig().grid_size.x;
                rt_ui.GetConfig().grid_config.cell_size = cell_size;

                instance_buffer_handle          = bindless_array->AllocateBuffer(g_scene.GetBuffer(EGpuSceneResource::InstanceInfo)->GetView());
                geometry_buffer_handle          = bindless_array->AllocateBuffer(g_scene.GetBuffer(EGpuSceneResource::GeometryInfo)->GetView());
                geometry_instance_buffer_handle = bindless_array->AllocateBuffer(g_scene.GetBuffer(EGpuSceneResource::GeometryInstance)->GetView());
                material_buffer_idx             = bindless_array->AllocateBuffer(g_scene.GetBuffer(EGpuSceneResource::MaterialInfo)->GetView());
                view_buffer_handle              = bindless_array->AllocateBuffer(rt_view_param_buffer->GetView());
                light_buffer_handle             = bindless_array->AllocateBuffer(g_scene.GetBuffer(EGpuSceneResource::LightInfo)->GetView());

                rt_ctx->SetBindlessHandles(geometry_buffer_handle, instance_buffer_handle, material_buffer_idx);
                rt_ctx->SetRaytracingScene(rt_scene);
                rt_ctx->FillLowDiscrepancySequence(cmd_list);

                if (env_map) {
                }
                first_load = false;

                cmd_list.UpdateBindlessArray(bindless_array);
                last_io_change_timeline = copy_queue_timeline->GetValue();
                // gfx_queue.Wait({uint64(copy_queue_timeline.Get()), copy_timeline->GetValue()});
                // gfx_queue.Sync();

                rt_geometries.reserve(g_scene.GetEntityCount());
                Array<AccelerationStructureBuildParam> build_params;
                build_params.reserve(g_scene.GetEntityCount());

                g_scene.ForEach([&](Entity _entity) {
                    auto& mesh = RenderableManager::Get().GetMeshInfo(_entity);

                    const MeshBuffers&     mesh_buffers = *mesh->buffers;
                    RaytracingGeometryInfo rt_geo_info{};
                    rt_geo_info.build_flags      = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
                    rt_geo_info.vertex_format    = PF_R32G32B32_SFLOAT;
                    rt_geo_info.vertex_buffer    = mesh_buffers.vertex_buffer;
                    rt_geo_info.index_buffer     = mesh_buffers.index_buffer;
                    rt_geo_info.index_type       = IET_UINT32;
                    rt_geo_info.max_vertex_count = mesh->vtx_count;
                    rt_geo_info.primitive_count  = mesh->idx_count / 3;

                    for (uint i = 0; i < mesh->geometries.size(); i++) {
                        uint vtx_offset = mesh->vtx_offset + mesh->geometries[i]->local_vtx_offset;
                        uint vtx_count  = mesh->geometries[i]->local_vtx_count;
                        uint idx_offset = mesh->idx_offset + mesh->geometries[i]->local_idx_offset;
                        uint idx_count  = mesh->geometries[i]->local_idx_count;

                        rt_geo_info.segments.emplace_back(0, 0, vtx_offset, vtx_count, sizeof(float3), idx_offset / 3, idx_count / 3, RTGT_TRIANGLES, ERayTracingGeometryFlags::GEOMETRY_OPAQUE);
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

                Array<ImportTexture> sampled_textures;
                sampled_textures.reserve((g_scene.GetGpuScene().material_textures.size()));

                for (auto& [name, tex] : g_scene.GetGpuScene().material_textures) {
                    sampled_textures.emplace_back(ImportTexture(tex->GetView(0, tex->GetNumMips()), ETextureState::SAMPLE));
                }

                cmd_list.ImportTextureFromQueue(EQueueType::Copy, std::move(sampled_textures));
                cmd_list.UpdateRaytracingScene(rt_scene);

                // cmd_list.UpdateRaytracingScene(rt_scene);
                gfx_queue.Execute(cmd_list.Submit().Wait(copy_queue_timeline, last_io_change_timeline));
                gfx_queue.Sync();
                // rt_scene->AdvanceFrame();
                // cmd_list.UpdateRaytracingScene(rt_scene);
                // gfx_queue.Execute(cmd_list.Submit());
                // gfx_queue.Sync();
            }

            if (b_new_env_map && load_res_event->IsComplete()) {
                env_map = rt_res.GetDefaultEnvMap();

                Array<ImportTexture> import_textures;
                {
                    const auto& rt_res_textures = rt_res.GetTextures();
                    for (auto& [name, tex] : rt_res_textures) {
                        import_textures.emplace_back(ImportTexture(tex->GetView(0, tex->GetNumMips()), ETextureState::SAMPLE));
                    }
                    cmd_list.ImportTextureFromQueue(EQueueType::Copy, std::move(import_textures));
                    gfx_queue.Execute(cmd_list.Submit().Wait(copy_queue_timeline, copy_queue_timeline->GetValue()));
                    gfx_queue.Sync();
                }

                assert(env_map && "env map is null");
                Sampler sampler{SF_LINEAR, SAM_CLAMP_TO_BORDER};

                Moer::EnvironmentLightComponent* env_light = MoerNew(Moer::EnvironmentLightComponent)(float3(1.f), env_map->GetExtent().xy);
                env_light->bdls_handle                     = bindless_array->AllocateTexture(env_map->GetView(0, env_map->GetNumMips()), sampler);

                auto entity = EntityManager::Get().Create();
                LightComponentManager::Get().Put(entity, env_light);
                g_scene.AddLight(entity);

                for (int i = 0; i < env_map->GetNumMips(); ++i) {
                    env_mips.push_back(env_map->GetView(i));
                }
                sd_utils.GenerateMips(cmd_list, env_mips);
                b_new_env_pdf = true;
                b_new_env_map = false;
            }

            if (b_new_env_pdf && rt_ctx) {
                rt_ctx->CreateEnvMapResources(env_map, cmd_list);
                b_new_env_pdf = false;
            }

            if (Scene::GetCurrentSceneLoadInfo().Get() && Scene::GetCurrentSceneLoadInfo()->IsReady()) {

                for (size_t i = 0; i < g_scene.GetEntityCount(); i++) {
                    auto& instance = rt_scene->GetInstance(i);
                    // instance.transform = TransformManager::Get().Get(g_scene.GetEntities()[i]).GetMatrix3x4();
                    rt_scene->MarkModified(instance.instance_id);
                }
                cmd_list.UpdateRaytracingScene(rt_scene);
            }
            auto camera_entity = g_scene.GetCameras()[0];
            auto camera        = CameraManager::Get().Get(camera_entity);
            //prepare frame
            {

                rt_config_param.world2view_prev = Transpose(camera->GetViewMatrix());
                rt_config_param.world2clip_prev = Transpose(camera->GetProjectionMatrix() * camera->GetViewMatrix());

                rt_ctx->FillLowDiscrepancySequence(cmd_list);

                camera->Tick();

                rt_view_param.view2world = Transpose(camera->GetToWorldMatrix());
                rt_view_param.world2view = Transpose(camera->GetViewMatrix());
                rt_view_param.frustum    = camera->GetFrustum();
                rt_view_param.near_far   = float2(camera->GetNearClip(), camera->GetFarClip());
                rt_view_param.rect       = uint2(resolution.x, resolution.y);
                rt_view_param.inv_rect   = float2(1.f / resolution.x, 1.f / resolution.y);
                rt_view_param.jitter     = float2(0, 0);
                rt_view_param.dir        = camera->GetDirection();
                rt_view_param.orthomode  = 0;

                const RTUI::Config& rt_ui_config = rt_ui.GetConfig();

                rt_config_param.view2world = Transpose(camera->GetToWorldMatrix());
                rt_config_param.view2clip  = Transpose(camera->GetProjectionMatrix());
                rt_config_param.world2view = Transpose(camera->GetViewMatrix());
                rt_config_param.world2clip = Transpose(camera->GetViewProjectionMatrix());

                rt_config_param.tan_pixel_angular_radius = tanf(Angle::DegreeToRadian(camera->GetFov()));
                rt_config_param.tan_sun_angular_radius   = tanf(Angle::DegreeToRadian(rt_ui_config.sun_angular_diameter * 0.5f));
                rt_config_param.bounce_num               = rt_ui_config.max_bounce;
                float3 sun_dir                           = Normalizef(rt_ui_config.sun_direction);
                rt_config_param.sun_direction_gexposure  = float4(sun_dir, rt_ui_config.exposure);
                cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)&rt_config_param, sizeof(RTConfigParam)), rt_config_param_buffer->GetView());

                cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)&rt_view_param, sizeof(RTViewParam)), rt_view_param_buffer->GetView());
            }

            //update light direction from ui data
            {
                auto light_entity = g_scene.GetLights()[0];
                auto light        = LightComponentManager::Get().Get(light_entity);
                if (light->GetType() == ELightComponentType::DIRECTIONAL) {
                    DirectionalLightComponent* dir_light = static_cast<DirectionalLightComponent*>(light.Get());
                    dir_light->SetDirection(-rt_ui.GetConfig().sun_direction);
                    dir_light->SetIntensity(rt_ui.GetConfig().exposure);
                }
            }

            //fill ui data
            {
                auto        grid_cfg           = is_ctx.GetGridChangableConfig();
                const auto& ui_cfg             = rt_ui.GetConfig();
                grid_cfg.cell_size             = rt_ui.GetConfig().grid_config.cell_size;
                grid_cfg.center                = camera->GetPosition();
                auto grid_static_cfg           = is_ctx.GetGridConfig();
                grid_static_cfg.light_per_ceil = rt_ui.GetConfig().grid_config.light_per_ceil;
                grid_static_cfg.grid_mode      = rt_ui.GetConfig().grid_config.grid_mode;
                is_ctx.SetGridConfig(grid_static_cfg);
                is_ctx.SetChangeableGridConfig(grid_cfg);

                auto di_initial_sample_config                      = is_ctx.GetDIInitialSampleParams();
                auto di_temporal_resampling_config                 = is_ctx.GetDITemporalResampleParams();
                auto di_spatial_resampling_config                  = is_ctx.GetDISpatialResampleParams();
                di_initial_sample_config.local_light_sample_mode   = ui_cfg.restir_di_cfg.initial_sample_config.local_light_sample_mode;
                di_temporal_resampling_config.bias_correction_mode = ui_cfg.restir_di_cfg.temporal_resample_config.bias_correction;
                di_spatial_resampling_config.bias_correction_mode  = ui_cfg.restir_di_cfg.spatial_resample_config.bias_correction;

                is_ctx.SetReSTIRDIIInitialSampleParams(di_initial_sample_config);
                is_ctx.SetReSTIRDITemporalResampleParams(di_temporal_resampling_config);
                is_ctx.SetReSTIRDISpatialResampleParams(di_spatial_resampling_config);
            }

            is_ctx.TickFrame(time);
            visualize_config.visualize_mode = rt_ui.GetConfig().final_color;

            uint num_emissive_meshes, num_emissive_triangles;
            prepare_light_pass->CountEmissiveInstances(num_emissive_meshes, num_emissive_triangles);

            rt_ctx->CreateBuffersIfNeeded(num_emissive_meshes, num_emissive_triangles, g_scene.GetLights().size(), g_scene.GetGeometryInstances().size());
            cmd_list.UpdateBindlessArray(bindless_array);

            rt_ctx->Tick(camera);

            prepare_light_pass->Process(cmd_list, *rt_ctx);

            g_buffer_pass->Process(cmd_list, *rt_ctx);
            lighting_pass->Process(cmd_list, *rt_ctx);

            TestInlineRTShader::Param param;
            param.global_param_handle    = view_buffer_handle;
            param.material_buffer_handle = material_buffer_idx;
            param.instance_buffer_handle = instance_buffer_handle;
            param.geometry_buffer_handle = geometry_buffer_handle;
            param.light_buffer_handle    = light_buffer_handle;
            param.rect                   = uint2(resolution.x, resolution.y);
            param.inv_rect               = float2(1.f / resolution.x, 1.f / resolution.y);
            param.jitter                 = float2(0, 0);
            param.frame_idx              = time;

            // cmd_list.Compute(rt_shader,
            //                  param,
            //                  rt_config_param_buffer,
            //                  out_normal_roughness,
            //                  out_basecolor_metalness,
            //                  out_direct_lighting,
            //                  out_emission,
            //                  out_diffuse,
            //                  out_specular,
            //                  out_view_z,
            //                  out_mv,
            //                  out_shadow_info,
            //                  rt_ctx->frame_rt.scene_color,
            //                  bindless_array,
            //                  rt_scene->GetTlas())
            //     .Dispatch(uint3((resolution.x + 15) >> 4, (resolution.y + 15) >> 4, 1), "PathTracing");
            visualize_pass->Process(cmd_list, *rt_ctx, visualize_config, bindless_array);
            //copy normal to output
            // cmd_list.CopyFrom(out_direct_lighting->GetView(), scene_color->GetView());
            rt_ctx->AdvanceFrame();
        }

        // rt_scene->MarkModified(0);
        // cmd_list.UpdateRaytracingScene(rt_scene);
        Sampler linear_sampler{SF_LINEAR, SAM_CLAMP_TO_BORDER};

        if (rt_ui.IsSeperateWindow() && rt_ui.GetWindowFrameBuffer().GetTexture()) {
            auto frame_buffer = rt_ui.GetWindowFrameBuffer();
            auto scene_res    = rt_ui.GetSceneColorResolution();
            auto scene_pos    = rt_ui.GetSceneColorPos();
            cmd_list.Gfx(sample_tex, rt_ctx->frame_rt.debug_color, linear_sampler).Draw("SampleTexture", Rect2D(scene_pos.x, scene_pos.y, scene_res.x, scene_res.y), {}, 3, {SingleDrawParam(3, 1, 0, 0, 0)}, ColorAttachment(frame_buffer.GetTexture()));
        } else {
            float2 f_res  = float2(resolution.x, resolution.y);
            float2 min_xy = rt_ui.GetSceneColorPos() / f_res;
            float2 max_xy = (rt_ui.GetSceneColorPos() + rt_ui.GetSceneColorResolution()) / f_res;
            cmd_list.Gfx(combine_ui, rt_ctx->frame_rt.debug_color, ui_frame_buffer, linear_sampler, CombineUIPipeline::Param{min_xy, max_xy})
                .Draw("CombineUI",
                      Rect2D(0, 0, resolution.x, resolution.y),
                      {},
                      3,
                      {SingleDrawParam(3, 1, 0, 0, 0)},
                      ColorAttachment(output));
        }
        gui.RenderGUI(cmd_list, output);

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
        gui.PresentWindows();
    }
    gfx_queue.Sync();
}