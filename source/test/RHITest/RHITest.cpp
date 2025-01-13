#include <filesystem>
#include <stb_image.h>
// #include <vcruntime_string.h>
#include "Core.h"
#include "PixelFormat.h"
#include "config/ConfigManager.h"
#include "math/Constant.h"
#include "math/Matrix.h"
#include "misc/MMemory.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHI.h"
#include "modules/render/source/rhi/RHIImpl.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "scene/Scene.h"
#include "shader/Shader.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "log/LogSystem.h"
#include "RenderThread.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "core/include/Core.h"
#include "modules/resource/include/loader/LoaderInterface.h"
#include "renderer/UIRenderer.h"
#include "scene/CameraManager.h"
#include "scene/Material.h"
#include "scene/RenderableManager.h"
#include "utils/smaa/SmaaPrecomputedTextures.h"

#include "RHIUI.h"

using namespace Moer::Render;
using namespace Moer;

class TestTrianglePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TestTrianglePipeline);
    DEFINE_SHADER_BUFFER(buffer);
    DEFINE_SHADER_ARGS(buffer);
};

struct TestBindlessParam {
    float4     color;
    uint       texture_handle;
    uint       buffer_handle;
    uint       instance_buffer_handle;
    uint       geometry_data_handle;
    uint       geometry_instance_handle;
    Matrix4x4f camera_view_proj;
};

struct MaterialPassBindlessParam {
    uint material_type;
    uint light_buffer;
    uint material_buffer;
    uint v_buffer;
    uint g_buffer_normal;
    uint g_buffer_uv;
    uint g_buffer_depth;
    uint gbuffer_position;
    uint global_param_handle;
};

struct LightingData {
    Matrix4x4f inv_view_proj;
    uint       light_count;
    uint3      padding;
    float3     camera_position;
};

// MARK: Pipeline Structures

class TestTrianglePipelineConstColor : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TestTrianglePipelineConstColor);
    DEFINE_SHADER_CONSTANT_STRUCT(TestBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_TEX(texture);
    DEFINE_SHADER_SAMPLER(defaultSampler);
    DEFINE_SHADER_BUFFER(constants);
    DEFINE_SHADER_ARGS(defaultSampler, constants, texture, bdls, param);
};

class TestTrianglePipelineBdls : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TestTrianglePipelineBdls);
    DEFINE_SHADER_ARGS();
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

class MaterialShadingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(MaterialShadingPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(MaterialPassBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

struct AoPipelineBindlessParam {
    float2 inv_resolution;
    float  ssao_intensity;
    float  ssao_max_distance;
    uint   ssao_sample_count;
    uint   ssao_radius;
    uint   ao_mode;
    uint   input_image;
    uint   normal_tex;
    uint   position_tex;
    uint   noise_tex;// linear & repeat sampler
};

class AoPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(AoPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(AoPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

struct SsrPipelineBindlessParam {
    Matrix4x4f view_projection_matrix;
    float3     camera_position;
    float      near_clip;
    float2     resolution;
    float      far_clip;
    float      ssr_roughness_threshold;
    float      ssr_metallic_threshold;
    float      ssr_step_base;
    uint       ssr_sample_count;
    uint       ssr_is_enable_jitter;
    uint       ssr_is_force_ground_enable_ssr;
    uint       color_tex;
    uint       position_tex;
    uint       normal_tex;
    uint       depth_tex;
    uint       vbuffer;
    uint       gbuffer_uv;
    uint       material_buffer;
};
class SsrPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SsrPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SsrPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

// MARK: * AA Pipeline Struct

struct SmaaSharedPipelineBindlessParam {
    Matrix4x4f curr_inv_vp_and_prev_vp;// = previous_view_projection * current_inverse_view_projection
    float4     rt_metrics;             // float4(inv_resolution.xy, resolution.xy)
    uint       aa_mode;
    uint       color_tex;   // initial input image
    uint       position_tex;// position gbuffer
    uint       depth_tex;   // depth gbuffer
    uint       search_tex;
    uint       area_tex;
    uint       edges_tex;
    uint       blend_tex;
    uint       current_color_tex; // current output image (without temporal AA)
    uint       previous_color_tex;// previous output image (without temporal AA)
    uint       frame_index;
    uint       point_sampler;
    uint       linear_sampler;
    uint       padding[3];
};
class SmaaEdgeDetectionPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaEdgeDetectionPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};
class SmaaBlendingWeightPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaBlendingWeightPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};
class SmaaNeighborhoodBlendingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaNeighborhoodBlendingPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};
class SmaaT2xNeighborhoodBlendingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaT2xNeighborhoodBlendingPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};
class SmaaT2xResolvePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SmaaT2xResolvePipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SmaaSharedPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

struct FxaaPrecomputePipelineBindlessParam {
    uint input_image;
};
class FxaaPrecomputePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(FxaaPrecomputePipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(FxaaPrecomputePipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

struct FxaaPipelineBindlessParam {
    uint   input_image;
    uint   fxaa_mode;
    float2 resolution;
    float2 inv_resolution;
};
class FxaaPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(FxaaPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(FxaaPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

// MARK: Main Function
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
    SurfaceInitInfo surface_info("Vulkan", resolution.x, resolution.y, "RHITest", false);
    WindowContext::Init(surface_info);

    auto&& scope_exit_window_context_and_etc = OnScopeExit([&] {
        WindowContext::ShutDown();
        RenderDevice::Dispose();
        TaskSystem::ShutDown();
    });

    Moer::Render::UIRenderer gui(device);

    auto* window_handle = WindowContext::GetMainWindow();

    auto                buf = device.CreateBuffer<float>(1024, EBufferUsageFlags::UNORDERED_ACCESS);
    SwapchainCreateInfo sc_info{.window_handle = (uintptr_t)window_handle, .size = {resolution.x, resolution.y}, .back_buffer_sz = 2, .preferred_format = PF_R8G8B8A8_SRGB};
    auto                sc             = device.CreateSwapchain(sc_info);
    Scene               scene          = {};
    BindlessArrayRef    bindless_array = scene.GetBindlessArray();
    auto&               gfx_queue      = device.GetCommandQueue(EQueueType::Graphics);
    auto&               copy_queue     = device.GetCopyQueue();

    Array<uint> data(1024);
    for (uint i = 0; i < 1024; ++i) {
        data[i] = i;
    }

    BufferRef copy_queue_buffer = device.CreateBuffer<uint>(1024, EBufferUsageFlags::UNORDERED_ACCESS);
    {
        CommandList copy_cmd_list;
        copy_cmd_list.CopyFrom(std::span<byte>((byte*)data.data(), data.size() * sizeof(uint)), copy_queue_buffer->GetView());
        auto evt = copy_queue.Execute(copy_cmd_list.Submit());
        copy_queue.Sync(evt.timeline);
    }
    CommandList cmd_list{};
    auto        buffer = device.CreateBuffer<uint>(1024, EBufferUsageFlags::UNORDERED_ACCESS);

    Array<uint> dst_data(1024);
    cmd_list.CopyFrom(copy_queue_buffer->GetView(), buffer->GetView());
    cmd_list.CopyFrom(buffer->GetView(), std::span<byte>((byte*)dst_data.data(), dst_data.size() * sizeof(uint)));
    auto copy_queue_timeline = copy_queue.GetFenceHandle();
    gfx_queue.Execute(cmd_list.Submit().Wait(copy_queue_timeline, 0));
    gfx_queue.Sync();

    // MARK: Textures & FrameBuf

    TextureRef
        vbuffer,
        normal,
        uv,
        position,
        pbr_shading_output,
        ao_output,
        ssr_output,
        antialiasing_temporal_texture_1,
        antialiasing_temporal_texture_2,
        antialiasing_output,
        output,
        ui_frame_buffer;
    DepthBufferRef             depth;
    StaticArray<TextureRef, 2> antialiasing_temporal_texture_34;

    Sampler sampler(SF_LINEAR, SAM_REPEAT);

    uint                 bdls_tex_handle_vbuffer                          = 0;
    uint                 bdls_tex_handle_normal                           = 0;
    uint                 bdls_tex_handle_uv                               = 0;
    uint                 bdls_tex_handle_position                         = 0;
    uint                 bdls_tex_handle_depth_with_nearest_sampler       = 0;
    uint                 bdls_tex_handle_depth_with_linear_sampler        = 0;
    uint                 bdls_tex_handle_pbr_shading_output               = 0;
    uint                 bdls_tex_handle_ao_output                        = 0;
    uint                 bdls_tex_handle_ssr_output                       = 0;
    uint                 bdls_tex_handle_antialiasing_temporal_texture_1  = 0;
    uint                 bdls_tex_handle_antialiasing_temporal_texture_2  = 0;
    StaticArray<uint, 2> bdls_tex_handle_antialiasing_temporal_texture_34 = StaticArray<uint, 2>{0, 0};
    uint                 bdls_tex_handle_antialiasing_output              = 0;
    uint                 bdls_tex_handle_output                           = 0;
    uint                 bdls_tex_handle_ui_frame_buffer                  = 0;

    Array<std::pair<TextureView, std::string>> frame_buffer_and_name_array;// for RHI UI

    // MARK: * Create FrameBuf
    auto create_frame_buffers = [&](uint2 _new_extent) {
        vbuffer = device.CreateTexture(
            "vbuffer",
            Extent2D(resolution.x, resolution.y),
            PF_R32_UINT,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

        normal = device.CreateTexture(
            "normal",
            Extent2D(resolution.x, resolution.y),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

        uv = device.CreateTexture(
            "uv",
            Extent2D(resolution.x, resolution.y),
            PF_R32G32_SFLOAT,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

        depth = device.CreateDepthBuffer(
            "depth",
            Extent2D(resolution.x, resolution.y),
            PF_D32_SFLOAT_S8_UINT,
            1,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT);

        position = device.CreateTexture(
            "position",
            Extent2D(resolution.x, resolution.y),
            PF_R32G32B32A32_SFLOAT,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

        pbr_shading_output = device.CreateTexture(
            "pbr_shading_output",
            Extent2D(resolution.x, resolution.y),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

        ao_output = device.CreateTexture(
            "ao_output",
            Extent2D(resolution.x, resolution.y),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

        ssr_output = device.CreateTexture(
            "ssr_output",
            Extent2D(resolution.x, resolution.y),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

        antialiasing_temporal_texture_1 = device.CreateTexture(
            "antialiasing_temporal_texture_1",
            Extent2D(resolution.x, resolution.y),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

        antialiasing_temporal_texture_2 = device.CreateTexture(
            "antialiasing_temporal_texture_2",
            Extent2D(resolution.x, resolution.y),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

        antialiasing_temporal_texture_34 = StaticArray<TextureRef, 2>{
            device.CreateTexture(
                "antialiasing_temporal_texture_3",
                Extent2D(resolution.x, resolution.y),
                PF_R8G8B8A8_UNORM,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT),
            device.CreateTexture(
                "antialiasing_temporal_texture_4",
                Extent2D(resolution.x, resolution.y),
                PF_R8G8B8A8_UNORM,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT)};

        antialiasing_output = device.CreateTexture(
            "antialiasing_output",
            Extent2D(resolution.x, resolution.y),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);

        ui_frame_buffer = device.CreateTexture(
            "ui_frame_buffer",
            Extent2D(resolution.x, resolution.y),
            PF_R8G8B8A8_SRGB,
            ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED);

        output = device.CreateTexture(
            "output",
            Extent2D(resolution.x, resolution.y),
            PF_R8G8B8A8_SRGB,
            ETextureUsageFlags::COLOR_ATTACHMENT);
    };

    // MARK: * Allocate FrameBuf
    auto allocate_frame_buffers = [&]() {
        bdls_tex_handle_vbuffer                             = bindless_array->AllocateTexture(vbuffer, sampler);
        bdls_tex_handle_normal                              = bindless_array->AllocateTexture(normal, sampler);
        bdls_tex_handle_uv                                  = bindless_array->AllocateTexture(uv, sampler);
        bdls_tex_handle_position                            = bindless_array->AllocateTexture(position, sampler);
        bdls_tex_handle_depth_with_nearest_sampler          = bindless_array->AllocateTexture(depth->GetView(), Sampler(SF_NEAREST, SAM_CLAMP_TO_EDGE));
        bdls_tex_handle_depth_with_linear_sampler           = bindless_array->AllocateTexture(depth->GetView(), Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE));
        bdls_tex_handle_pbr_shading_output                  = bindless_array->AllocateTexture(pbr_shading_output, sampler);
        bdls_tex_handle_ao_output                           = bindless_array->AllocateTexture(ao_output, sampler);
        bdls_tex_handle_ssr_output                          = bindless_array->AllocateTexture(ssr_output, sampler);
        bdls_tex_handle_antialiasing_temporal_texture_1     = bindless_array->AllocateTexture(antialiasing_temporal_texture_1, sampler);
        bdls_tex_handle_antialiasing_temporal_texture_2     = bindless_array->AllocateTexture(antialiasing_temporal_texture_2, sampler);
        bdls_tex_handle_antialiasing_temporal_texture_34[0] = bindless_array->AllocateTexture(antialiasing_temporal_texture_34[0], sampler);
        bdls_tex_handle_antialiasing_temporal_texture_34[1] = bindless_array->AllocateTexture(antialiasing_temporal_texture_34[1], sampler);
        bdls_tex_handle_antialiasing_output                 = bindless_array->AllocateTexture(antialiasing_output, sampler);
        bdls_tex_handle_ui_frame_buffer                     = bindless_array->AllocateTexture(ui_frame_buffer, sampler);
        bdls_tex_handle_output                              = bindless_array->AllocateTexture(output, sampler);

        cmd_list.UpdateBindlessArray(bindless_array);
    };

    // MARK: * Free FrameBuf
    auto free_frame_buffers = [&]() {
        bindless_array->FreeTexture(bdls_tex_handle_vbuffer);
        bindless_array->FreeTexture(bdls_tex_handle_normal);
        bindless_array->FreeTexture(bdls_tex_handle_uv);
        bindless_array->FreeTexture(bdls_tex_handle_position);
        bindless_array->FreeTexture(bdls_tex_handle_depth_with_nearest_sampler);
        bindless_array->FreeTexture(bdls_tex_handle_depth_with_linear_sampler);
        bindless_array->FreeTexture(bdls_tex_handle_pbr_shading_output);
        bindless_array->FreeTexture(bdls_tex_handle_ao_output);
        bindless_array->FreeTexture(bdls_tex_handle_ssr_output);
        bindless_array->FreeTexture(bdls_tex_handle_antialiasing_temporal_texture_1);
        bindless_array->FreeTexture(bdls_tex_handle_antialiasing_temporal_texture_2);
        bindless_array->FreeTexture(bdls_tex_handle_antialiasing_temporal_texture_34[0]);
        bindless_array->FreeTexture(bdls_tex_handle_antialiasing_temporal_texture_34[1]);
        bindless_array->FreeTexture(bdls_tex_handle_antialiasing_output);
        bindless_array->FreeTexture(bdls_tex_handle_ui_frame_buffer);
        bindless_array->FreeTexture(bdls_tex_handle_output);
    };

    // MARK: * Invoke FB Functions

    create_frame_buffers(resolution);
    allocate_frame_buffers();

    // MARK: * Displayable FramBuf

    // call this function like `RHIUI rhi_ui(renderer, create_frame_buffer_and_name_array(), ..)`
    //     or, `rhi_ui->RegisterFrameBuffers(create_frame_buffer_and_name_array(), ..);`
    auto create_frame_buffer_and_name_array = [&]() {
        assert(vbuffer != nullptr && "vbuffer is nullptr");
        return Array<std::pair<TextureView, std::string>>{
            {vbuffer->GetView(), "vbuffer"},
            {normal->GetView(), "normal"},
            {uv->GetView(), "uv"},
            {position->GetView(), "position"},
            {depth->GetView(), "depth"},
            {pbr_shading_output->GetView(), "pbr_shading_output"},
            {ao_output->GetView(), "ao_output"},
            {ssr_output->GetView(), "ssr_output"},
            {antialiasing_temporal_texture_1->GetView(), "antialiasing_temporal_texture_1"},
            {antialiasing_temporal_texture_2->GetView(), "antialiasing_temporal_texture_2"},
            {antialiasing_temporal_texture_34[0]->GetView(), "antialiasing_temporal_texture_34[0]"},
            {antialiasing_temporal_texture_34[1]->GetView(), "antialiasing_temporal_texture_34[1]"},
            {antialiasing_output->GetView(), "antialiasing_output"},
            {ui_frame_buffer->GetView(), "ui_frame_buffer"}
            // , {output->GetView(), "output"} // don't put output here, because it will be used as the final output
        };
    };
    uint rhi_ui_default_selected_frame_buffer_index = [&]() {// use a function to avoid reordering of frame_buffer_and_name_array
        const std::string default_selected_frame_buffer_name = "antialiasing_output";

        auto array = create_frame_buffer_and_name_array();
        for (uint i = 0; i < array.size(); ++i) {
            if (array[i].second == default_selected_frame_buffer_name) {
                return i;
            }
        }
        assert(false && "Invalid default selected frame buffer index");
    }();

    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    // MARK: Pipeline Variable

    auto raster_pipeline_constant_color = [&]() {
        VertexStream vertex_stream;
        //pos
        vertex_stream.EmplacePerVertex(
            {Moer::Render::VertexElement(PF_R32G32B32_SFLOAT)});
        //normal
        vertex_stream.EmplacePerVertex({Moer::Render::VertexElement(PF_R32_UINT)});
        //tangent
        vertex_stream.EmplacePerVertex({Moer::Render::VertexElement(PF_R32_UINT)});
        //uv
        vertex_stream.EmplacePerVertex({Moer::Render::VertexElement(PF_R32G32_SFLOAT)});

        GfxPsoCreateInfo pso_info(RHIRasterizeInfo::Preset(),
                                  vertex_stream,
                                  {RHIColorAttachmentInfo::Preset(PF_R32_UINT),
                                   RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM),
                                   RHIColorAttachmentInfo::Preset(PF_R32G32_SFLOAT),
                                   RHIColorAttachmentInfo::Preset(PF_R32G32B32A32_SFLOAT)},
                                  RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>(),
                                  PF_D32_SFLOAT_S8_UINT);

        return manager
            .Raster()
            .Vertex("test/BasicVertex.hlsl")
            .Pixel("test/BasicFragConstant.hlsl")
            .Build<TestTrianglePipelineConstColor>(std::move(pso_info));
    }();

    auto pbr_pipeline = [&]() {
        GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                              {},
                                              {RHIColorAttachmentInfo::Preset(pbr_shading_output->GetFormat())});

        return manager
            .Raster()
            .Vertex("test/PBRMaterialVertex.hlsl")
            .Pixel("test/PBRMaterialFrag.hlsl")
            .Build<MaterialShadingPipeline>(std::move(pso_full_screen_info));
    }();

    auto ao_pipeline = [&]() {
        GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                              {},
                                              {RHIColorAttachmentInfo::Preset(ao_output->GetFormat())});
        return manager
            .Raster()
            .Vertex("test/post_process/PostProcessFullScreenQuad.hlsl")
            .Pixel("test/post_process/Ao.hlsl")
            .Build<AoPipeline>(std::move(pso_full_screen_info));
    }();

    auto ssr_pipeline = [&]() {
        GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                              {},
                                              {RHIColorAttachmentInfo::Preset(ssr_output->GetFormat())});
        return manager
            .Raster()
            .Vertex("test/post_process/PostProcessFullScreenQuad.hlsl")
            .Pixel("test/post_process/Ssr.hlsl")
            .Build<SsrPipeline>(std::move(pso_full_screen_info));
    }();

    // MARK: * AA Pipeline

    // smaa
    auto smaa_edge_detection_pipeline = [&]() {
        GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                              {},
                                              {RHIColorAttachmentInfo::Preset(antialiasing_temporal_texture_1->GetFormat())});
        return manager
            .Raster()
            .Vertex("test/post_process/SmaaWrapper.hlsl", "SMAAEdgeDetectionVS_Wrapper")
            .Pixel("test/post_process/SmaaWrapper.hlsl", "SMAALumaEdgeDetectionPS_Wrapper")
            .Build<SmaaEdgeDetectionPipeline>(std::move(pso_full_screen_info));
    }();// IILE(Immediately Invoked Lambda Expression), usually for complex varaible initialization and avoid naming conflicts

    auto smaa_blending_weight_pipeline = [&]() {
        GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                              {},
                                              {RHIColorAttachmentInfo::Preset(antialiasing_temporal_texture_2->GetFormat())});
        return manager
            .Raster()
            .Vertex("test/post_process/SmaaWrapper.hlsl", "SMAABlendingWeightCalculationVS_Wrapper")
            .Pixel("test/post_process/SmaaWrapper.hlsl", "SMAABlendingWeightCalculationPS_Wrapper")
            .Build<SmaaBlendingWeightPipeline>(std::move(pso_full_screen_info));
    }();

    auto smaa_neighborhood_blending_pipeline = [&]() {
        GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                              {},
                                              {RHIColorAttachmentInfo::Preset(antialiasing_output->GetFormat())});
        return manager
            .Raster()
            .Vertex("test/post_process/SmaaWrapper.hlsl", "SMAANeighborhoodBlendingVS_Wrapper")
            .Pixel("test/post_process/SmaaWrapper.hlsl", "SMAANeighborhoodBlendingPS_Wrapper")
            .Build<SmaaNeighborhoodBlendingPipeline>(std::move(pso_full_screen_info));
    }();

    auto smaa_t2x_neighborhood_blending_pipeline = [&]() {
        GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                              {},
                                              {RHIColorAttachmentInfo::Preset(antialiasing_temporal_texture_34[0]->GetFormat())});
        return manager
            .Raster()
            .Vertex("test/post_process/SmaaWrapper.hlsl", "SMAANeighborhoodBlendingVS_Wrapper")
            .Pixel("test/post_process/SmaaWrapper.hlsl", "SMAANeighborhoodBlendingPS_Wrapper")
            .Build<SmaaT2xNeighborhoodBlendingPipeline>(std::move(pso_full_screen_info));
    }();

    auto smaa_t2x_resolve_pipeline = [&]() {
        GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                              {},
                                              {RHIColorAttachmentInfo::Preset(antialiasing_output->GetFormat())});
        return manager
            .Raster()
            .Vertex("test/post_process/SmaaWrapper.hlsl", "SMAAResolveVS_Wrapper")
            .Pixel("test/post_process/SmaaWrapper.hlsl", "SMAAResolvePS_Wrapper")
            .Build<SmaaT2xResolvePipeline>(std::move(pso_full_screen_info));
    }();

    // fxaa
    auto fxaa_precompute_pipeline = [&]() {
        GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                              {},
                                              {RHIColorAttachmentInfo::Preset(antialiasing_temporal_texture_1->GetFormat())});
        return manager
            .Raster()
            .Vertex("test/post_process/PostProcessFullScreenQuad.hlsl")
            .Pixel("test/post_process/FxaaPrecompute.hlsl")
            .Build<FxaaPrecomputePipeline>(std::move(pso_full_screen_info));
    }();

    auto fxaa_pipeline = [&]() {
        GfxPsoCreateInfo pso_full_screen_info(RHIRasterizeInfo::Preset(),
                                              {},
                                              {RHIColorAttachmentInfo::Preset(antialiasing_output->GetFormat())});
        return manager
            .Raster()
            .Vertex("test/post_process/PostProcessFullScreenQuad.hlsl")
            .Pixel("test/post_process/Fxaa.hlsl")
            .Build<FxaaPipeline>(std::move(pso_full_screen_info));
    }();

    // MARK: * UI Pipeline

    auto combine_ui_pipeline = [&]() {
        GfxPsoCreateInfo combine_pso_info(RHIRasterizeInfo::Preset(),
                                          {},
                                          {RHIColorAttachmentInfo::Preset(output->GetFormat())});
        return manager
            .Raster()
            .Vertex("CombineGuiVert.hlsl")
            .Pixel("CombineGuiFrag.hlsl")
            .Build<CombineUIPipeline>(std::move(combine_pso_info));
    }();

    auto sample_texture_pipeline = [&]() {
        GfxPsoCreateInfo sample_tex_pso_info(RHIRasterizeInfo::Preset(),
                                             {},
                                             {RHIColorAttachmentInfo::Preset(output->GetFormat())});
        return manager
            .Raster()
            .Vertex("framework/FullScreen.vert.hlsl")
            .Pixel("utils/CopyTexture.frag.hlsl")
            .Build<SampleTexturePipeline>(std::move(sample_tex_pso_info));
    }();

    float4     color_red = {1, 1, 1, 1};
    uint       instance_buffer_handle;
    uint       geom_data_buffer_handle;
    uint       geom_instance_buffer_handle;
    TextureRef red_tex = device.CreateTexture(
        Extent2D(1, 1),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::COLOR_ATTACHMENT);

    byte red_data[4] = {byte(255), byte(0), byte(0), byte(255)};
    cmd_list.CopyFrom(std::span<byte>((byte*)&red_data, sizeof(red_data)), red_tex);
    uint bdls_tex_handle_red = bindless_array->AllocateTexture(red_tex, sampler);

    BufferRef  red_buffer      = device.CreateBuffer<float>(4, EBufferUsageFlags::UNORDERED_ACCESS);
    float4     red_data_float4 = {1, 0, 0, 1};
    BufferView red_buffer_view(red_buffer, 0, 4, 4);
    cmd_list.CopyFrom(std::span<byte>((byte*)&red_data_float4, sizeof(red_data_float4)), red_buffer->GetView());
    uint bdls_buffer_handle_red = bindless_array->AllocateBuffer(red_buffer_view);

    // TODO: optimize code (maybe could refer to RTResource.h/cpp ?)

    // MARK: AO Pipeline Resource

    TextureRef noise_tex = device.CreateTexture(
        "noise_tex",
        Extent2D(4, 4),
        PF_R8_UNORM,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST);
    uint bdls_tex_handle_noise_with_linear_sampler = 0;

    {
        std::string filepath = (ConfigManager::GetInstance().GetEditorResourcePath() / "textures" / "noise_256x256.png").string();

        FILE* file = nullptr;
        fopen_s(&file, filepath.c_str(), "rb");
        int width, height, channels;
        if (file) {
            ubyte* data = stbi_load_from_file(file, &width, &height, &channels, 4);

            cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)data, width * height * 4), noise_tex);
            cmd_list.AddCallback([data]() {
                stbi_image_free(data);
            });

            bdls_tex_handle_noise_with_linear_sampler = bindless_array->AllocateTexture(noise_tex, Sampler(SF_LINEAR, SAM_REPEAT));
            // UpdateBindlessArray will be called with other resources
        } else {
            LOG_ERROR("Failed to load noise texture");
        }
    }

    // MARK: AA Pipeline Resource

    TextureRef smaa_area_tex = device.CreateTexture(
        "smaa_area_tex",
        Extent2D(SMAA_AREATEX_WIDTH, SMAA_AREATEX_HEIGHT),
        PF_R8G8_UNORM,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST);

    cmd_list.CopyFrom(
        std::span<byte>((byte*)&SmaaPrecomputedTextures::areaTexBytes,
                        sizeof(SmaaPrecomputedTextures::areaTexBytes)),
        smaa_area_tex);
    uint bdls_tex_handle_smaa_area_tex = bindless_array->AllocateTexture(smaa_area_tex, sampler);

    TextureRef smaa_search_tex = device.CreateTexture(
        "smaa_search_tex",
        Extent2D(SMAA_SEARCHTEX_WIDTH, SMAA_SEARCHTEX_HEIGHT),
        PF_R8_UNORM,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST);

    cmd_list.CopyFrom(
        std::span<byte>((byte*)&SmaaPrecomputedTextures::searchTexBytes,
                        sizeof(SmaaPrecomputedTextures::searchTexBytes)),
        smaa_search_tex);
    uint bdls_tex_handle_smaa_search_tex = bindless_array->AllocateTexture(smaa_search_tex, sampler);

    cmd_list.UpdateBindlessArray(bindless_array);
    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    Resource::LoaderInterface::LoadSceneFromFileAsync(ConfigManager::GetInstance().GetScenePath(), &scene);
    auto&& scope_exit_reset_async_load_info = OnScopeExit([&] {
        Scene::ResetAsyncLoadInfo();
    });

    FenceRef timeline   = device.CreateFence();
    uint64   time       = 0;
    bool     first_load = true;

    uint material_buffer_handle = 0;
    uint light_buffer_handle    = 0;
    uint lighting_data_handle   = 0;

    BufferRef lighting_buffer = device.CreateBuffer<byte>(1 * sizeof(LightingData), EBufferUsageFlags::UNORDERED_ACCESS);
    BufferRef camera_buffer   = device.CreateBuffer<byte>(sizeof(ViewParam), EBufferUsageFlags::CONSTANT_BUFFER);
    ViewParam view_param;
    RHIUI     rhi_ui(
        gui,
        create_frame_buffer_and_name_array(),
        rhi_ui_default_selected_frame_buffer_index);

    // MARK: Main Loop
    while (WindowContext::ShouldClose(window_handle) == false) {
        WindowContext::Tick();
        gui.BeginGUIFrame();
        {
            rhi_ui.TickUI();
        }
        gui.EndGUIFrame();
        if (time > 2) {
            timeline->Wait(time - 2);
        }

        const RHIUI::Config& ui_config = rhi_ui.GetConfig();

        // MARK: Window Resizing
        int w_width, w_height;
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

            free_frame_buffers();
            create_frame_buffers(resolution);
            allocate_frame_buffers();
            rhi_ui.RegisterFrameBuffers(create_frame_buffer_and_name_array(), rhi_ui_default_selected_frame_buffer_index);
        }

        uint last_io_change_timeline = 0;
        if (Scene::GetCurrentSceneLoadInfo().Get() && Scene::GetCurrentSceneLoadInfo()->IsReady()) {
            // MARK: First Load
            if (first_load) {
                instance_buffer_handle      = bindless_array->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::InstanceInfo)->GetView());
                geom_data_buffer_handle     = bindless_array->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::GeometryInfo)->GetView());
                geom_instance_buffer_handle = bindless_array->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::GeometryInstance)->GetView());

                material_buffer_handle = bindless_array->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::MaterialInfo)->GetView());
                light_buffer_handle    = bindless_array->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::LightInfo)->GetView());
                lighting_data_handle   = bindless_array->AllocateBuffer(lighting_buffer->GetView());

                // I moved AllocateTexture code to `allocate_frame_buffers()` to reuse when resolution changed.
                //     And `allocate_frame_buffers()` will be called after `create_frame_buffers()` immediately,
                //     because `allocate_frame_buffers()` doesn't depends on SceneLoadInfo.

                Array<ImportTexture> sampled_textures;
                sampled_textures.reserve((scene.GetGpuScene().material_textures.size()));

                for (auto& [name, tex] : scene.GetGpuScene().material_textures) {
                    sampled_textures.emplace_back(ImportTexture(tex->GetView(0, tex->GetNumMips()), ETextureState::SAMPLE));
                }

                cmd_list.ImportTextureFromQueue(EQueueType::Copy, std::move(sampled_textures));

                cmd_list.UpdateBindlessArray(bindless_array);
                last_io_change_timeline = copy_queue_timeline->GetValue();
                first_load              = false;

                gfx_queue.Execute(cmd_list.Submit().Wait(copy_queue_timeline, last_io_change_timeline));
            }

            // MARK: Camera

            auto camera_entity = scene.GetCameras()[0];
            auto camera        = CameraManager::Get().Get(camera_entity);

            // Jitter Camera for SMAA T2x
            static uint8_t smaa_current_frame_index = 0;
            if (ui_config.aa_mode == 4) {
                smaa_current_frame_index ^= 1;
                static StaticArray<float2, 2> smaa_jitter = {float2(0.25f, -0.25f), float2(-0.25f, 0.25f)};
                camera->SetJitterMatrix(smaa_jitter[smaa_current_frame_index]);
            }

            // use scene_color resolution instead of window resolution
            // TODO: fix the same issue in RTTest
            camera->Tick(rhi_ui.GetSceneColorAspectRatio());

            // MARK: GBuffer Pass
            Array<MeshDrawData> mesh_draw_datas{};
            // draw_datas.emplace_back(SingleDrawParam{uint(index_buffer->GetByteSize()/sizeof(uint)), 1, 0, 0, 0});

            uint instance_count = 0;
            // for (auto entity : scene.GetEntities()) {
            //     auto& mesh = RenderableManager::Get().GetMeshInfo(entity);
            //     draw_datas.emplace_back(SingleDrawParam{mesh->idx_count, 1, mesh->idx_offset, mesh->vtx_offset, instance_count++});
            // }
            std::span<const StaticArray<VertexBuffer, VETA_Num>> vertex_buffers = scene.GetVertexBufferViews();
            std::span<const IndexBuffer>                         index_buffers  = scene.GetIndexBufferViews();

            uint geom_idx = 0;
            scene.ForEach([&](Entity _entity) {
                auto& mesh = RenderableManager::Get().GetMeshInfo(_entity);

                const StaticArray<VertexBuffer, VETA_Num>& vertex_buffer = vertex_buffers[mesh->global_mesh_idx];
                auto&                                      mesh_draw_dat = mesh_draw_datas.emplace_back(
                    std::span<VertexBuffer>((VertexBuffer*)vertex_buffer.data(), 4),
                    index_buffers[mesh->global_mesh_idx]);
                for (uint i = 0; i < mesh->geometries.size(); i++) {
                    uint                idx              = geom_idx + i;
                    const MeshGeometry& geom             = *mesh->geometries[i];
                    uint                first_idx        = geom.local_idx_offset + mesh->idx_offset;
                    uint                first_vertex_idx = geom.local_vtx_offset + mesh->vtx_offset;
                    mesh_draw_dat.EmplaceDrawIndexed(first_idx, geom.local_idx_count, first_vertex_idx, idx);
                }
                geom_idx += mesh->geometries.size();
            });

            view_param.clip2view  = Transpose(camera->GetProjectionMatrixInv());
            view_param.view2clip  = Transpose(camera->GetProjectionMatrix());
            view_param.view2world = Transpose(camera->GetViewMatrixInv());
            view_param.world2view = Transpose(camera->GetViewMatrix());
            view_param.world2clip = Transpose(camera->GetViewProjectionMatrix());
            view_param.clip2world = Transpose(camera->GetViewProjectionMatrixInv());

            cmd_list.CopyFrom(std::span<byte>((byte*)&view_param, sizeof(view_param)), camera_buffer->GetView());

            TestBindlessParam param;
            param.color                    = color_red;
            param.texture_handle           = bdls_tex_handle_red;
            param.buffer_handle            = bdls_buffer_handle_red;
            param.instance_buffer_handle   = instance_buffer_handle;
            param.geometry_data_handle     = geom_data_buffer_handle;
            param.geometry_instance_handle = geom_instance_buffer_handle;
            param.camera_view_proj         = Transpose(camera->GetViewProjectionMatrix());
            cmd_list.Gfx(raster_pipeline_constant_color, sampler, camera_buffer, red_tex, bindless_array, param)
                .Draw(Rect2D(0, 0, resolution.x, resolution.y), std::move(mesh_draw_datas), DepthAttachment(depth->GetView().GetTexture()), ColorAttachment(vbuffer), ColorAttachment(normal), ColorAttachment(uv), ColorAttachment(position));

            // MARK: PBR Pass

            MaterialPassBindlessParam material_param;
            // material_param.material_buffer = bdls_buffer_handle_red;
            material_param.g_buffer_uv         = bdls_tex_handle_uv;
            material_param.g_buffer_normal     = bdls_tex_handle_normal;
            material_param.v_buffer            = bdls_tex_handle_vbuffer;
            material_param.g_buffer_depth      = bdls_tex_handle_depth_with_nearest_sampler;
            material_param.gbuffer_position    = bdls_tex_handle_position;
            material_param.global_param_handle = lighting_data_handle;
            material_param.light_buffer        = light_buffer_handle;

            LightingData lighting_data;
            lighting_data.inv_view_proj   = Transpose(camera->GetViewProjectionMatrixInv());
            lighting_data.light_count     = scene.GetLights().size();
            lighting_data.camera_position = camera->GetPosition();
            cmd_list.CopyFrom(std::span<byte>((byte*)&lighting_data, sizeof(lighting_data)), lighting_buffer->GetView());

            Moer::UnorderedSet<EMaterialType>                                   material_types = {EMaterialType::E_PBR_STANDARD};
            Moer::UnorderedMap<EMaterialType, Moer::Array<MaterialInstanceRef>> material_instances;
            material_param.material_buffer = material_buffer_handle;
            for (auto type : material_types) {
                Array<SingleDrawParam> full_screen_draw_datas;
                full_screen_draw_datas.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});
                material_param.material_type = uint(type);
                cmd_list.Gfx(pbr_pipeline, bindless_array, material_param)
                    .Draw("Lighting Pass", Rect2D(0, 0, resolution.x, resolution.y), std::move(full_screen_draw_datas), ColorAttachment(pbr_shading_output));
            };

            auto get_full_screen_draw_datas = [&]() {
                Array<SingleDrawParam> full_screen_draw_datas;
                full_screen_draw_datas.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});
                return full_screen_draw_datas;
            };

            /**
             * MARK: AO Pass
             * 
             * TODO: SSDO Support
             */
            uint ao_actual_input  = bdls_tex_handle_pbr_shading_output;
            uint ao_actual_output = bdls_tex_handle_pbr_shading_output;
            if (ui_config.ao_mode != 0) {
                ao_actual_output = bdls_tex_handle_ao_output;// update actual output when ao is enabled

                AoPipelineBindlessParam param;
                param.inv_resolution    = float2(1.0f) / float2(resolution);
                param.ssao_intensity    = ui_config.ssao_intensity;
                param.ssao_max_distance = ui_config.ssao_max_distance;
                param.ssao_sample_count = ui_config.ssao_sample_count;
                param.ssao_radius       = ui_config.ssao_radius;
                param.ao_mode           = ui_config.ao_mode;
                param.input_image       = ao_actual_input;
                param.normal_tex        = bdls_tex_handle_normal;
                param.position_tex      = bdls_tex_handle_position;
                param.noise_tex         = bdls_tex_handle_noise_with_linear_sampler;

                cmd_list
                    .Gfx(ao_pipeline, bindless_array, param)
                    .Draw(
                        "AO Pass",
                        Rect2D(0, 0, resolution.x, resolution.y),
                        std::move(get_full_screen_draw_datas()),
                        ColorAttachment(ao_output));
            }

            /**
             * MARK: SSR Pass
             * 
             * GUI Help:
             *   You can set `Roughness Threshold` and `Metallic Threshold` to control which material will be reflected.
             *   If you want all materials to be reflected, set `Roughness Threshold` to 0.0 and `Metallic Threshold` to 1.0.
             *   `Force Ground Enable SSR` will force the ground to be reflected, which is useful for testing.
             *   TODO: move the above text to gui
             * 
             * TODO: use HiZ buffer to accelerate SSR (now, a simple heuristic and binary search is used for SSR)
             * TODO: glossy ssr
             * TODO: performance optimization
             * TODO: fix some artifacts (jitter)
             */
            uint ssr_actual_input  = ao_actual_output;
            uint ssr_actual_output = ao_actual_output;
            if (ui_config.ssr_is_enable_ssr) {
                ssr_actual_output = bdls_tex_handle_ssr_output;// update actual output when ssr is enabled

                SsrPipelineBindlessParam param;
                param.view_projection_matrix         = Transpose(camera->GetViewProjectionMatrix());
                param.camera_position                = camera->GetPosition();
                param.near_clip                      = camera->GetNearClip();
                param.resolution                     = float2(resolution);
                param.far_clip                       = camera->GetFarClip();
                param.ssr_roughness_threshold        = ui_config.ssr_roughness_threshold;
                param.ssr_metallic_threshold         = ui_config.ssr_metallic_threshold;
                param.ssr_step_base                  = ui_config.ssr_step_base;
                param.ssr_sample_count               = ui_config.ssr_sample_count;
                param.ssr_is_enable_jitter           = ui_config.ssr_is_enable_jitter;
                param.ssr_is_force_ground_enable_ssr = ui_config.ssr_is_force_ground_enable_ssr;
                param.color_tex                      = ssr_actual_input;
                param.position_tex                   = bdls_tex_handle_position;
                param.normal_tex                     = bdls_tex_handle_normal;
                param.depth_tex                      = bdls_tex_handle_depth_with_linear_sampler;
                param.vbuffer                        = bdls_tex_handle_vbuffer;
                param.gbuffer_uv                     = bdls_tex_handle_uv;
                param.material_buffer                = material_buffer_handle;

                cmd_list
                    .Gfx(ssr_pipeline, bindless_array, param)
                    .Draw(
                        "SSR Pass",
                        Rect2D(0, 0, resolution.x, resolution.y),
                        std::move(get_full_screen_draw_datas()),
                        ColorAttachment(ssr_output));
            }

            /**
             * MARK: AA Passes
             * 
             * Use gui to switch antialiasing mode:
             * 0: FXAA Off                : 620+-fps
             * 1: FXAA Quality(Simplified): 612+-fps
             * 2: FXAA Quality            : 584+-fps
             * 3: SMAA 1x  (Preset High)  : 578+-fps [Default]
             * 4: SMAA T2x (Preset High)  : 
             * 
             * For FXAA (2 passes)
             *   Pass 1: precompute luma -> antialiasing_temporal_texture_1
             *   Pass 2: FXAA main pass  -> antiailiasing_output
             * 
             * For SMAA 1x (3 passes, details in shaders/test/post_process/SMAA.hlsl)
             *   Pass 1: Edge Detection              -> antialiasing_temporal_texture_1 (edgesTex)
             *   Pass 2: Blending Weight Calculation -> antialiasing_temporal_texture_2 (blendTex)
             *   Pass 3: Neighborhood Blending       -> antialiasing_output
             * 
             * For SMAA T2x (4 passes)
             *   Pass 1: Edge Detection              -> antialiasing_temporal_texture_1  (rg: edgesTex & ba: velocityTex)
             *   Pass 2: Blending Weight Calculation -> antialiasing_temporal_texture_2  (blendTex)
             *   Pass 3: Neighborhood Blending       -> antialiasing_temporal_texture_34 (double buffer) (currentColorTex & previousColorTex)
             *   Pass 4: Resolve                     -> antialiasing_output
             *   注：SMAA T2x需要启用Reprojection才可以防止ghosting。Reprojection需要一个velocityTex，在这里我直接将velocityTex写入edgesTex的后两个通道
             *   注2：实际上，因为目前帧数为500+fps，所以看不到ghosting；可以在主循环中sleep 0.1s并且设置shader中的SMAAReprojection为0来得到一个ghosting的结果
             * 
             * 关于不同抗锯齿模式的说明
             *   切换抗锯齿时，多余的Pass不会被执行，应该不会有额外的性能开销
             * 
             * 关于SMAA实现的一些说明
             *   SMAA是通过直接集成论文仓库中的代码实现的（https://github.com/iryoku/smaa）
             *   原始代码不兼容bindless rhi，所以我将仓库中原始的代码封装了一下，并从bindless rhi中提取出了texture和sampler
             *   这部分可能破坏rhi的一些封装，具体见下面的GetSamplerIdx函数，除了这一点外，c++部分没有其他不优雅的代码
             *   shader部分和bindless rhi的耦合性特别高，如果修改bindless框架的话，大概率shader也要一起修改
             * Imporant: 所以如果修改了bindless框架，然后画面黑屏的话，请先将抗锯齿设置为FXAA(aa_mode = 2)，可以快速解决问题
             * 
             * 关于SMAA T2x的说明
             *   1. T2x使用了Temporal Supersampling，需要让相机抖动。可以通过camera->SetJitteredMatrix()来设置JitteredMatrix，这个矩阵会作用在ViewMatrix上
             *   2. 目前SMAA T2x效果和SMAA 1x类似，没有明显优势；不确定是场景问题还是实现问题
             *   FIXME: fix jitter in SMAA T2x when SMAA_REPROJECTION is enabled
             */
            uint aa_actual_input = ssr_actual_output;
            {
                if (0 <= ui_config.aa_mode && ui_config.aa_mode <= 2) {// fxaa

                    FxaaPrecomputePipelineBindlessParam param_fxaa_precomputed;
                    param_fxaa_precomputed.input_image = aa_actual_input;

                    cmd_list
                        .Gfx(fxaa_precompute_pipeline, bindless_array, param_fxaa_precomputed)
                        .Draw(
                            "FXAA Precompute Pass",
                            Rect2D(0, 0, resolution.x, resolution.y),
                            std::move(get_full_screen_draw_datas()),
                            ColorAttachment(antialiasing_temporal_texture_1));

                    FxaaPipelineBindlessParam param_fxaa;
                    param_fxaa.input_image    = bdls_tex_handle_antialiasing_temporal_texture_1;
                    param_fxaa.fxaa_mode      = ui_config.aa_mode;
                    param_fxaa.resolution     = float2(resolution);
                    param_fxaa.inv_resolution = float2(1.0) / float2(resolution);

                    cmd_list
                        .Gfx(fxaa_pipeline, bindless_array, param_fxaa)
                        .Draw("FXAA Pass",
                              Rect2D(0, 0, resolution.x, resolution.y),
                              std::move(get_full_screen_draw_datas()),
                              ColorAttachment(antialiasing_output));

                } else if (3 <= ui_config.aa_mode && ui_config.aa_mode <= 4) {// smaa

                    // TODO: optimize the following code
                    //           以下是我会写出这段代码的原因：
                    //       SMAA官方提供了一段代码SMAA.hlsl，只需要一些简单的修改，就可以让我们快速将SMAA集成到MoerEngine中
                    //       但是SMAA.hlsl并不支持我们的bindless后的rhi，所以我修改了SMAA.hlsl，并试图提取出了独立的texture和sampler
                    //       因此，我需要texture index和sampler index
                    //       texture index直接使用bindless_array->AllocateTexture就可以解决
                    //       sampler index是通过VulkanDevice::GetSamplerIdx()得到的，但这个函数所在的头文件并不能被include（因为不位于include目录下）
                    //       那么为了获取sampler index，我们有两种方案 1. 复制GetSamplerIdx的实现；2. 通过AllocateTexture得到handle后再解压出sampler index
                    //       第一种方案可能导致代码不一致，而第二种方案过于不可控，而且我不确定是否有潜在的性能开销，所以我同时写了两种方案，并且写下了这段说明
                    //       目前使用第一种
                    //       有更好的方案的话，直接修改下面这段代码即可
                    auto GetSamplerIdx = [&](const Sampler& sampler) {
                        // method 1
                        uint filter  = uint(sampler.filter);
                        uint address = uint(sampler.address_mode);
                        uint compare = uint(sampler.compare_function);
                        return (uint(SF_Num) * uint(SAM_Num)) * compare + (uint(SF_Num)) * address + filter;
                        // method 2
                        // uint bdls_tex_handle = bindless_array->AllocateTexture(antialiasing_output, sampler);
                        // uint sampler_idx     = bdls_tex_handle & 0xff;
                        // return sampler_idx;
                    };

                    static Matrix4x4f current_view_proj     = Matrix4x4f::Identity();
                    static Matrix4x4f previous_view_proj    = Matrix4x4f::Identity();
                    static Matrix4x4f current_inv_view_proj = Matrix4x4f::Identity();

                    previous_view_proj    = current_view_proj;
                    current_view_proj     = camera->GetViewProjectionMatrix();
                    current_inv_view_proj = camera->GetViewProjectionMatrixInv();

                    auto smaa_shared_param = [&]() {
                        SmaaSharedPipelineBindlessParam param;
                        param.aa_mode                 = ui_config.aa_mode;
                        param.color_tex               = aa_actual_input;
                        param.position_tex            = bdls_tex_handle_position;
                        param.depth_tex               = bdls_tex_handle_depth_with_nearest_sampler;
                        param.search_tex              = bdls_tex_handle_smaa_search_tex;
                        param.area_tex                = bdls_tex_handle_smaa_area_tex;
                        param.edges_tex               = bdls_tex_handle_antialiasing_temporal_texture_1;
                        param.blend_tex               = bdls_tex_handle_antialiasing_temporal_texture_2;
                        param.current_color_tex       = bdls_tex_handle_antialiasing_temporal_texture_34[smaa_current_frame_index];
                        param.previous_color_tex      = bdls_tex_handle_antialiasing_temporal_texture_34[smaa_current_frame_index ^ 1];
                        param.frame_index             = smaa_current_frame_index;
                        param.point_sampler           = GetSamplerIdx(Sampler(SF_NEAREST, SAM_CLAMP_TO_EDGE));
                        param.linear_sampler          = GetSamplerIdx(Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE));
                        param.rt_metrics              = float4(1.0f / resolution.x, 1.0f / resolution.y, resolution.x, resolution.y);
                        param.curr_inv_vp_and_prev_vp = Transpose(previous_view_proj * current_inv_view_proj);
                        return param;
                    }();

                    cmd_list
                        .Gfx(smaa_edge_detection_pipeline, bindless_array, smaa_shared_param)
                        .Draw(
                            "SMAA Edge Detection Pass",
                            Rect2D(0, 0, resolution.x, resolution.y),
                            std::move(get_full_screen_draw_datas()),
                            ColorAttachment(antialiasing_temporal_texture_1));

                    cmd_list
                        .Gfx(smaa_blending_weight_pipeline, bindless_array, smaa_shared_param)
                        .Draw(
                            "SMAA Blending Weight Calculation Pass",
                            Rect2D(0, 0, resolution.x, resolution.y),
                            std::move(get_full_screen_draw_datas()),
                            ColorAttachment(antialiasing_temporal_texture_2));

                    if (ui_config.aa_mode == 3) {
                        cmd_list
                            .Gfx(smaa_neighborhood_blending_pipeline, bindless_array, smaa_shared_param)
                            .Draw(
                                "SMAA Neighborhood Blending Pass",
                                Rect2D(0, 0, resolution.x, resolution.y),
                                std::move(get_full_screen_draw_datas()),
                                ColorAttachment(antialiasing_output));
                    } else if (ui_config.aa_mode == 4) {
                        cmd_list
                            .Gfx(smaa_t2x_neighborhood_blending_pipeline, bindless_array, smaa_shared_param)
                            .Draw(
                                "SMAA T2x Neighborhood Blending Pass",
                                Rect2D(0, 0, resolution.x, resolution.y),
                                std::move(get_full_screen_draw_datas()),
                                ColorAttachment(antialiasing_temporal_texture_34[smaa_current_frame_index]));

                        cmd_list
                            .Gfx(smaa_t2x_resolve_pipeline, bindless_array, smaa_shared_param)
                            .Draw(
                                "SMAA T2x Resolve Pass",
                                Rect2D(0, 0, resolution.x, resolution.y),
                                std::move(get_full_screen_draw_datas()),
                                ColorAttachment(antialiasing_output));
                    } else {
                        assert(false && "Invalid antialiasing mode");
                    }

                } else {
                    assert(false && "Invalid antialiasing mode");
                }
            }
        }

        auto final_output = rhi_ui.GetSelectedFrameBuffer();

        // MARK: UI Combine Pass
        if (rhi_ui.IsSeperateWindow() && rhi_ui.GetWindowFrameBuffer().GetTexture()) {
            auto frame_buffer = rhi_ui.GetWindowFrameBuffer();
            auto scene_res    = rhi_ui.GetSceneColorResolution();
            auto scene_pos    = rhi_ui.GetSceneColorPos();
            cmd_list
                .Gfx(
                    sample_texture_pipeline,
                    final_output,
                    Sampler(ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_CLAMP_TO_EDGE))
                .Draw(
                    "SampleTexture",
                    Rect2D(scene_pos.x, scene_pos.y, scene_res.x, scene_res.y),
                    {},
                    3,
                    {SingleDrawParam(3, 1, 0, 0, 0)},
                    ColorAttachment(frame_buffer.GetTexture()));
        } else {
            float2 f_res  = float2(resolution.x, resolution.y);
            float2 min_xy = rhi_ui.GetSceneColorPos() / f_res;
            float2 max_xy = (rhi_ui.GetSceneColorPos() + rhi_ui.GetSceneColorResolution()) / f_res;
            cmd_list
                .Gfx(
                    combine_ui_pipeline,
                    final_output,
                    ui_frame_buffer,
                    Sampler(ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_CLAMP_TO_EDGE),// linear_sampler
                    CombineUIPipeline::Param{min_xy, max_xy})
                .Draw("Combine UI Pass",
                      Rect2D(0, 0, resolution.x, resolution.y),
                      {},
                      3,
                      {SingleDrawParam(3, 1, 0, 0, 0)},
                      ColorAttachment(output));
        }

        // cmd_list.Gfx(raster_pipeline, red_buffer)
        //     .Draw(Rect2D(0, 0, 1, 1), vb_span, ib, std::move(draw_datas), ColorAttachment(red_tex));

        // //float color with time sine
        // color_red[0] = 0.5f + 0.5f * sinf(time * 0.1f);
        // color_red[2] = 0.5f + 0.5f * cosf(time * 0.1f);
        // cmd_list.CopyFrom(std::span<byte>((byte*)&color_red, sizeof(color_red)), red_buffer_view);

        gui.RenderGUI(cmd_list, output);

        // cmd_list.Barriers(ReadTexture(red_tex, ETextureState::SAMPLE));
        time++;
        /***
        currently using a phony timeline (any timeline signaled by copy queue) to remove error message from validation layer caused by host synced copy operations
        we're not waiting for the copy queue to finish, because operations we wanted are synced on host side, we use this timeline just to notifiy the validation layer
        that we've done flushing copy queue resources
         */
        gfx_queue.Execute(cmd_list.Submit().Signal(timeline, time).Wait(copy_queue_timeline, 0));
        gfx_queue.Present(sc, output);
    }
    gfx_queue.Sync();
}