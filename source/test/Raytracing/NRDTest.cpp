#include <filesystem>
#include "Core.h"
#include "PixelFormat.h"
#include "config/ConfigManager.h"
#include "imgui.h"
#include "math/Matrix.h"
#include "misc/Traits.h"
#include "renderer/UIRenderer.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/Scene.h"
#include "scene/TransformManager.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"
#include "loader/LoaderInterface.h"
#include "misc/Timer.h"
#include "scene/CameraManager.h"
#include "scene/Material.h"
#include "scene/RenderableManager.h"
#include "scene/Scene.h"

#include "RTUI.h"

#include "rhi/extension//NrdExtension.h"

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
        uint   material_buffer_handle;
        uint   primitive_buffer_handle;
        uint   vtx_buffer_handle;
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
    DEFINE_SHADER_TEX(out_base_color_metalness);
    DEFINE_SHADER_TEX(out_direct_lighting);
    DEFINE_SHADER_TEX(out_emission);
    DEFINE_SHADER_TEX(out_diffuse);
    DEFINE_SHADER_TEX(out_specular);
    DEFINE_SHADER_TEX(out_view_z);
    DEFINE_SHADER_TEX(out_mv);
    DEFINE_SHADER_TEX(out_shadow_info);

    DEFINE_SHADER_TLAS(tlas);
    DEFINE_SHADER_CONSTANT_STRUCT(Param, param);

    DEFINE_SHADER_ARGS(param, rt_config, out_normal_roughness, out_base_color_metalness, out_direct_lighting, out_emission, out_diffuse, out_specular, out_view_z, out_mv, out_shadow_info, bdls, tlas);
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
    uint2           resolution = {1280, 720};
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
    auto&&           load_scene_scope = OnScopeExit([&] {
        Scene::ResetAsyncLoadInfo();
    });
    BindlessArrayRef bindless_array   = g_scene.GetBindlessArray();

    FenceRef copy_timeline = device.CreateFence();

    CommandList cmd_list;

    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

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

    bool   first_load = true;
    uint   instance_buffer_handle;
    uint   material_buffer_idx;
    uint   light_buffer_handle     = 0;
    uint64 last_io_change_timeline = 0;
    uint   view_buffer_handle      = 0;
    auto   copy_queue_timeline     = copy_queue.GetFenceHandle();

    //gbuffer bdls handle
    uint bdls_tex_handle_uv      = 0;
    uint bdls_tex_handle_normal  = 0;
    uint bdls_tex_handle_vbuffer = 0;
    uint bdls_tex_handle_depth   = 0;

    uint                         rt_vtx_handle      = 0;
    uint                         rt_prim_handle     = 0;
    uint                         rt_instance_handle = 0;
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

    TextureRef out_base_color_matalness = device.CreateTexture(
        "out_base_color_matalness",
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

    TextureRef denoised_diffuse  = device.CreateTexture("denoised_diffuse", Extent2D(resolution.x, resolution.y), PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
    TextureRef denoised_specular = device.CreateTexture("denoised_specular", Extent2D(resolution.x, resolution.y), PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);

    Array<TextureRef> test_bdls_texs(3);
    Array<uint>       test_bdls_handles(3);
    for (auto& tex : test_bdls_texs) {
        tex = device.CreateTexture(
            "test_bdls_tex",
            Extent2D(1, 1),
            PF_R8G8B8A8_SRGB,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);
    }

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

        out_base_color_matalness = device.CreateTexture(
            "out_base_color_matalness",
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

        denoised_diffuse  = device.CreateTexture("denoised_diffuse", Extent2D(resolution.x, resolution.y), PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        denoised_specular = device.CreateTexture("denoised_specular", Extent2D(resolution.x, resolution.y), PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
    };

    RTUI rt_ui{gui};

    Timer timer;
    timer.Start();
    uint64 last_time = 0ull;

    // nrd extension
    auto* nrd_ext       = device.LoadExtension<Ext::NRDExtension>();
    auto  nrd_interface = nrd_ext->CreateInterface(3, resolution.x, resolution.y);
    nrd_interface->UseDenoiser(nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR);

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

        if (time - last_time > 5000) {
            last_time = time;
            LOG_INFO("FPS {}, Time elapsed {} ms", 1000.f / frame_time, frame_time);
        }
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

            // nrd recreation
            nrd_interface = nrd_ext->RecreateInterface(std::move(nrd_interface), resolution.x, resolution.y);
        }

        if (Scene::GetCurrentSceneLoadInfo().Get() && Scene::GetCurrentSceneLoadInfo()->IsReady()) {
            if (first_load) {
                instance_buffer_handle = bindless_array->AllocateBuffer(g_scene.GetBuffer(EGpuSceneResource::InstanceInfo)->GetView());
                material_buffer_idx    = bindless_array->AllocateBuffer(g_scene.GetBuffer(EGpuSceneResource::MaterialInfo)->GetView());
                rt_vtx_handle          = bindless_array->AllocateBuffer(g_scene.GetBuffer(EGpuSceneResource::RTVertex)->GetView());
                rt_prim_handle         = bindless_array->AllocateBuffer(g_scene.GetBuffer(EGpuSceneResource::RTPrimitive)->GetView());
                rt_instance_handle     = bindless_array->AllocateBuffer(g_scene.GetBuffer(EGpuSceneResource::RTInstance)->GetView());
                view_buffer_handle     = bindless_array->AllocateBuffer(rt_view_param_buffer->GetView());
                light_buffer_handle    = bindless_array->AllocateBuffer(g_scene.GetBuffer(EGpuSceneResource::LightInfo)->GetView());
                first_load             = false;

                cmd_list.UpdateBindlessArray(bindless_array);
                last_io_change_timeline = copy_queue_timeline->GetValue();
                // gfx_queue.Wait({uint64(copy_queue_timeline.Get()), copy_timeline->GetValue()});
                // gfx_queue.Sync();

                rt_geometries.reserve(g_scene.GetEntityCount());
                Array<AccelerationStructureBuildParam> build_params;
                build_params.reserve(g_scene.GetEntityCount());
                auto vertex_buffer = g_scene.GetBuffer(EGpuSceneResource::RTVertex);

                auto index_buffer = g_scene.GetBuffer(EGpuSceneResource::RTIndex);
                g_scene.ForEach([&](Entity _entity) {
                    auto&                  mesh = RenderableManager::Get().GetRTMeshInfo(_entity);
                    RaytracingGeometryInfo rt_geo_info{};
                    rt_geo_info.build_flags      = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
                    rt_geo_info.vertex_format    = PF_R32G32B32_SFLOAT;
                    rt_geo_info.vertex_buffer    = vertex_buffer;
                    rt_geo_info.index_buffer     = index_buffer;
                    rt_geo_info.index_type       = IET_UINT32;
                    rt_geo_info.max_vertex_count = mesh.vertex_count;
                    rt_geo_info.primitive_count  = mesh.primitive_count;
                    rt_geo_info.segments.emplace_back(mesh.vertex_offset, mesh.vertex_count, sizeof(RTVertex), mesh.primitive_offset, mesh.primitive_count);

                    RaytracingGeometryRef blas = device.CreateRaytracingGeometry(rt_geo_info);
                    rt_geometries.push_back(blas);

                    auto prim = RenderableManager::Get().GetRenderPrimitive(_entity);

                    auto& instance     = rt_scene->AddInstance();
                    instance.geom      = blas;
                    instance.transform = TransformManager::Get().Get(_entity).GetMatrix3x4();

                    LOG_INFO("instance transform w {} {} {} ", instance.transform.r0.w, instance.transform.r1.w, instance.transform.r2.w);

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

                // cmd_list.UpdateRaytracingScene(rt_scene);
                gfx_queue.Execute(cmd_list.Submit().Wait(copy_queue_timeline, last_io_change_timeline));
                gfx_queue.Sync();
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

            rt_config_param.world2view_prev = camera->GetViewMatrix();
            rt_config_param.world2clip_prev = camera->GetProjectionMatrix() * camera->GetViewMatrix();

            camera->Tick();

            rt_view_param.view2world = camera->GetToWorldMatrix();
            rt_view_param.world2view = camera->GetViewMatrix();
            rt_view_param.frustum    = camera->GetFrustum();
            rt_view_param.near_far   = float2(camera->GetNearClip(), camera->GetFarClip());
            rt_view_param.rect       = uint2(resolution.x, resolution.y);
            rt_view_param.inv_rect   = float2(1.f / resolution.x, 1.f / resolution.y);
            rt_view_param.jitter     = float2(0, 0);
            rt_view_param.dir        = camera->GetDirection();
            rt_view_param.orthomode  = 0;

            const RTUI::Config& rt_ui_config = rt_ui.GetConfig();

            rt_config_param.view2world = camera->GetToWorldMatrix();
            rt_config_param.view2clip  = camera->GetProjectionMatrix();
            rt_config_param.world2view = camera->GetViewMatrix();
            rt_config_param.world2clip = camera->GetProjectionMatrix() * camera->GetViewMatrix();

            rt_config_param.tan_pixel_angular_radius = tanf(Angle::DegreeToRadian(camera->GetFov()));
            rt_config_param.tan_sun_angular_radius   = tanf(Angle::DegreeToRadian(rt_ui_config.sun_angular_diameter * 0.5f));
            rt_config_param.bounce_num               = rt_ui_config.max_bounce;
            float3 sun_dir                           = Normalizef(rt_ui_config.sun_direction);
            rt_config_param.sun_direction_gexposure  = float4(sun_dir, rt_ui_config.exposure);
            cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)&rt_config_param, sizeof(RTConfigParam)), rt_config_param_buffer->GetView());

            cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)&rt_view_param, sizeof(RTViewParam)), rt_view_param_buffer->GetView());
            TestInlineRTShader::Param param;
            param.global_param_handle     = view_buffer_handle;
            param.material_buffer_handle  = material_buffer_idx;
            param.instance_buffer_handle  = rt_instance_handle;
            param.primitive_buffer_handle = rt_prim_handle;
            param.vtx_buffer_handle       = rt_vtx_handle;
            param.light_buffer_handle     = light_buffer_handle;
            param.rect                    = uint2(resolution.x, resolution.y);
            param.inv_rect                = float2(1.f / resolution.x, 1.f / resolution.y);
            param.jitter                  = float2(0, 0);
            param.frame_idx               = time;

            cmd_list.Compute(rt_shader,
                             param,
                             rt_config_param_buffer,
                             out_normal_roughness,
                             out_base_color_matalness,
                             out_direct_lighting,
                             out_emission,
                             out_diffuse,
                             out_specular,
                             out_view_z,
                             out_mv,
                             out_shadow_info,
                             bindless_array,
                             rt_scene)
                .Dispatch(uint3((resolution.x + 15) >> 4, (resolution.y + 15) >> 4, 1), "PathTracing");

            // denoise
            nrd_interface->Begin();
            nrd_interface->SetInput(Ext::NRDInterface::EInResource::MOTION_VECTOR, out_mv);
            nrd_interface->SetInput(Ext::NRDInterface::EInResource::NORMAL_ROUGHNESS, out_normal_roughness);
            nrd_interface->SetInput(Ext::NRDInterface::EInResource::VIEW_Z, out_view_z);
            nrd_interface->SetInput(Ext::NRDInterface::EInResource::BASECOLOR_METALNESS, out_base_color_matalness);
            nrd_interface->SetInput(Ext::NRDInterface::EInResource::IN_DIFFUSE, out_direct_lighting);
            nrd_interface->SetInput(Ext::NRDInterface::EInResource::IN_SPECULAR, out_specular);
            nrd_interface->SetOutput(Ext::NRDInterface::EOutResource::OUT_DIFFUSE, denoised_diffuse);
            nrd_interface->SetOutput(Ext::NRDInterface::EOutResource::OUT_SPECULAR, denoised_specular);
            nrd_interface->UpdateCommonSettings(
                time,
                Vector2ui(resolution.x, resolution.y),
                Vector2f(0.f, 0.f),
                camera->GetViewMatrix(),
                camera->GetProjectionMatrix());
            nrd_interface->Denoise(cmd_list);

            //copy normal to output
            // cmd_list.CopyFrom(out_direct_lighting->GetView(), scene_color->GetView());
        }
        // rt_scene->MarkModified(0);
        // cmd_list.UpdateRaytracingScene(rt_scene);
        Sampler linear_sampler{SF_LINEAR, SAM_CLAMP_TO_BORDER};

        if (time >= 3) {
            bindless_array->FreeTexture(test_bdls_handles[time % 3]);
        }
        test_bdls_handles[time % 3] = bindless_array->AllocateTexture(test_bdls_texs[time % 3], linear_sampler);
        cmd_list.UpdateBindlessArray(bindless_array);
        if (rt_ui.IsSeperateWindow() && rt_ui.GetWindowFrameBuffer().GetTexture()) {
            auto frame_buffer = rt_ui.GetWindowFrameBuffer();
            auto scene_res    = rt_ui.GetSceneColorResolution();
            auto scene_pos    = rt_ui.GetSceneColorPos();
            cmd_list.Gfx(sample_tex, denoised_specular, linear_sampler).Draw("SampleTexture", Rect2D(scene_pos.x, scene_pos.y, scene_res.x, scene_res.y), {}, 3, {SingleDrawParam(3, 1, 0, 0, 0)}, ColorAttachment(frame_buffer.GetTexture()));
        } else {
            float2 f_res  = float2(resolution.x, resolution.y);
            float2 min_xy = rt_ui.GetSceneColorPos() / f_res;
            float2 max_xy = (rt_ui.GetSceneColorPos() + rt_ui.GetSceneColorResolution()) / f_res;
            cmd_list.Gfx(combine_ui, denoised_specular, ui_frame_buffer, linear_sampler, CombineUIPipeline::Param{min_xy, max_xy})
                .Draw("CombineUI",
                      Rect2D(0, 0, resolution.x, resolution.y),
                      {},
                      3,
                      {SingleDrawParam(3, 1, 0, 0, 0)},
                      ColorAttachment(output));
        }
        gui.RenderGUI(cmd_list, output);

        time++;
        gfx_queue.Execute(cmd_list.Submit().Signal(timeline, time));
        gfx_queue.Present(sc, output);
        gui.PresentWindows();
    }
    gfx_queue.Sync();
}