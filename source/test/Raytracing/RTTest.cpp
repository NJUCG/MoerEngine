#include <filesystem>
#include <vcruntime_string.h>
#include "Core.h"
#include "PixelFormat.h"
#include "config/ConfigManager.h"
#include "math/Matrix.h"
#include "misc/Traits.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/Shader.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "log/LogSystem.h"
#include "RenderThread.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"
#include "imgui.h"
#include "core/include/Core.h"

using namespace Moer::Render;
using namespace Moer;
class TestTrianglePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TestTrianglePipeline);
    DEFINE_SHADER_ARGS();
};

struct TestBindlessParam {
    float4 color;
    uint   texture_handle;
};
class TestTrianglePipelineConstColor : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TestTrianglePipelineConstColor);
    DEFINE_SHADER_CONSTANT_STRUCT(TestBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_TEX(texture);
    DEFINE_SHADER_SAMPLER(defaultSampler);
    DEFINE_SHADER_ARGS(defaultSampler, texture, bdls, param);
};

class TestTrianglePipelineBdls : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TestTrianglePipelineBdls);
    DEFINE_SHADER_ARGS();
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

    SwapchainCreateInfo sc_info{.window_handle = (uintptr_t)window_handle, .size = {resolution.x, resolution.y}, .back_buffer_sz = 2, .preferred_format = PF_R8G8B8A8_SRGB};
    auto                sc             = device.CreateSwapchain(sc_info);
    BindlessArrayRef    bindless_array = device.CreateBindlessArray();
    auto&               cmd_queue      = device.GetCommandQueue(EQueueType::Graphics);
    auto&               copy_queue     = device.GetCommandQueue(EQueueType::Copy);

    Array<uint> data(1024);
    for (uint i = 0; i < 1024; ++i) {
        data[i] = i;
    }

    FenceRef  copy_timeline     = device.CreateFence();
    BufferRef copy_queue_buffer = device.CreateBuffer<uint>(1024, EBufferUsageFlags::UNORDERED_ACCESS);
    {
        CommandList copy_cmd_list;
        copy_cmd_list.CopyFrom(std::span<byte>((byte*)data.data(), data.size() * sizeof(uint)), copy_queue_buffer->GetView());
        copy_queue.Execute(copy_cmd_list.Submit().Signal(copy_timeline, 1));
    }
    CommandList cmd_list;
    auto        buffer = device.CreateBuffer<uint>(1024, EBufferUsageFlags::UNORDERED_ACCESS);

    Array<uint> dst_data(1024);
    cmd_list.CopyFrom(copy_queue_buffer->GetView(), buffer->GetView());
    cmd_list.CopyFrom(buffer->GetView(), std::span<byte>((byte*)dst_data.data(), dst_data.size() * sizeof(uint)));
    cmd_queue.Execute(cmd_list.Submit().Wait(copy_timeline, 1));
    cmd_queue.Sync();

    ubyte*   pixels;
    int      width, height;
    uint     alignment = 4;
    ImGuiIO& io        = ImGui::GetIO();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    uint32_t upload_pitch = (width * 4 + alignment - 1u) & ~(alignment - 1u);
    uint32_t upload_size  = height * upload_pitch;

    TextureRef output = device.CreateTexture(
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT);
    cmd_queue.Execute(cmd_list.Submit());
    cmd_queue.Sync();

    struct Vertex {
        float3 pos;
        float2 uv;
    };
    Vertex vertices[] = {
        {{0.0f, -0.5f, 0.0f}, {0.5f, 1.0f}},
        {{-0.5f, 0.5f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f}},
    };
    uint    indices[] = {0, 1, 2};
    float4  color_red = {1, 1, 1, 1};
    Sampler sampler(SF_LINEAR, SAM_REPEAT);

    auto vertex_buffer = device.CreateBuffer<float>(3 * sizeof(Vertex) / sizeof(float), EBufferUsageFlags::VERTEX_BUFFER);
    auto index_buffer  = device.CreateBuffer<uint>(3, EBufferUsageFlags::INDEX_BUFFER);
    cmd_list.CopyFrom(std::span<byte>((byte*)vertices, sizeof(vertices)), vertex_buffer->GetView());
    cmd_list.CopyFrom(std::span<byte>((byte*)indices, sizeof(indices)), index_buffer->GetView());
    TextureRef red_tex = device.CreateTexture(
        Extent2D(1, 1),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::SAMPLED);

    byte red_data[4] = {byte(255), byte(0), byte(0), byte(255)};
    cmd_list.CopyFrom(std::span<byte>((byte*)&red_data, sizeof(red_data)), red_tex);
    uint bdls_tex_handle_red = bindless_array->AllocateTexture(red_tex, sampler);

    cmd_list.UpdateBindlessArray(bindless_array);
    cmd_queue.Execute(cmd_list.Submit());
    cmd_queue.Sync();

    VertexBuffer vb(vertex_buffer, 0);
    IndexBuffer  ib(index_buffer->GetView(), EIndexElementType::IET_UINT32);

    RaytracingGeometryInfo rt_geo_info{};
    rt_geo_info.build_flags      = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
    rt_geo_info.vertex_format    = PF_R32G32B32_SFLOAT;
    rt_geo_info.vertex_buffer    = vertex_buffer;
    rt_geo_info.index_buffer     = index_buffer;
    rt_geo_info.index_type       = IET_UINT32;
    rt_geo_info.max_vertex_count = vertex_buffer->GetByteSize() / sizeof(Vertex);
    rt_geo_info.segments.emplace_back(0, 3, sizeof(Vertex), 0, 1);

    RaytracingGeometryRef blas = device.CreateRaytracingGeometry(rt_geo_info);
    cmd_list.BuildAccelerationStructures({{blas, ERaytracingBuildMode::BUILD}});

    RaytracingSceneRef scene = device.CreateRaytracingScene();

    RaytracingMaterial  mat{};
    RaytracingInstance& rt_instance = scene->AddInstance();
    rt_instance.geom                = blas;
    rt_instance.transform           = Matrix3x4f(Matrix4x4f::Identity().r0, Matrix4x4f::Identity().r1, Matrix4x4f::Identity().r2);
    rt_instance.flag.need_create    = true;
    rt_instance.flag.need_update    = true;
    rt_instance.material_ref        = mat;

    rt_instance.visible_mask = RTVM_ALL;

    while (WindowContext::ShouldClose(window_handle) == false) {
        WindowContext::Tick();
        int w_width, w_height;

        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w_width, &w_height);
        if (w_width == 0 || w_height == 0) {
            std::this_thread::yield();
            continue;
        }
        if (w_width != resolution.x || w_height != resolution.y) {

            resolution = {uint32(w_width), uint32(w_height)};
            output     = device.CreateTexture(
                Extent2D(resolution.x, resolution.y),
                PF_R8G8B8A8_SRGB,
                ETextureUsageFlags::COLOR_ATTACHMENT);
            cmd_queue.Sync();
            sc_info.size = {resolution.x, resolution.y};
            sc->Recreate(sc_info);
        }
        TestBindlessParam param;
        param.color          = color_red;
        param.texture_handle = bdls_tex_handle_red;

        scene->MarkModified(0);
        cmd_list.UpdateRaytracingScene(scene);
        cmd_queue.Execute(cmd_list.Submit());
        cmd_queue.Present(sc, output);
    }
    cmd_queue.Sync();
}