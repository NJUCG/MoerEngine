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

int main(int argc, const char** argv) {

    using namespace Moer::Render;
    using namespace Moer;
    std::filesystem::path path = argv[0];
    path.filename().string().find(".exe") != std::string::npos ? path = path.parent_path() : path = path;
    ConfigManager::GetInstance().Init(path);
    TaskSystem::Init();
    DeviceInitInfo info{.rhi_type = ERHIType::Vulkan, .name = "RHITest", .ray_tracing = true};
    RenderDevice::Init(std::move(info));
    auto&           device = RenderDevice::Get();
    ShaderManager   manager(device);
    uint2 resolution = {1280, 720};
    SurfaceInitInfo surface_info("Vulkan", resolution.x, resolution.y, "RHITest", false);
    WindowContext::Init(surface_info);
    auto&& scope_exit    = OnScopeExit([&] {
        WindowContext::ShutDown();
        RenderDevice::Dispose();
        TaskSystem::ShutDown();
    });
    auto*  window_handle = WindowContext::GetMainWindow();

    auto buf = device.CreateBuffer<float>(1024, EBufferUsageFlags::UNORDERED_ACCESS);
    SwapchainCreateInfo sc_info{.window_handle = (uintptr_t)window_handle, .size = {resolution.x, resolution.y}, .back_buffer_sz = 2, .preferred_format = PF_R8G8B8A8_SRGB};
    auto sc  = device.CreateSwapchain(sc_info);

    auto&       cmd_queue = device.GetCommandQueue(EQueueType::Graphics);
    CommandList cmd_list;
    auto        buffer = device.CreateBuffer<uint>(1024, EBufferUsageFlags::UNORDERED_ACCESS);
    Array<uint> data(1024);
    for (uint i = 0; i < 1024; ++i) {
        data[i] = i;
    }
    Array<uint> dst_data(1024);
    cmd_list.CopyFrom(std::span<byte>((byte*)data.data(), data.size() * sizeof(uint)), buffer->GetView());
    cmd_list.CopyFrom(buffer->GetView(), std::span<byte>((byte*)dst_data.data(), dst_data.size() * sizeof(uint)));

    ubyte*   pixels;
    int      width, height;
    uint     alignment = 4;
    ImGuiIO& io        = ImGui::GetIO();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    uint32_t   upload_pitch = (width * 4 + alignment - 1u) & ~(alignment - 1u);
    uint32_t   upload_size  = height * upload_pitch;
    TextureRef font_tex     = device.CreateTexture(
        Extent2D(width, height),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::SAMPLED);

    TextureRef output = device.CreateTexture(
        Extent2D(resolution.x, resolution.y),
        PF_R8G8B8A8_SRGB,
        ETextureUsageFlags::COLOR_ATTACHMENT);

    cmd_list.CopyFrom(
        std::span<std::byte>((std::byte*)pixels, upload_size), font_tex);

    cmd_queue.Execute(cmd_list.Submit());
    cmd_queue.Sync();

    VertexStream vertex_stream;
    vertex_stream.Emplace(
        {Moer::Render::VertexElement(PF_R32G32B32_SFLOAT),
         Moer::Render::VertexElement(PF_R32G32_SFLOAT)});
    GfxPsoCreateInfo pso_info(RHIRasterizeInfo::Preset(),
                              vertex_stream,
                              {RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_SRGB)},
                              RHIDepthStencilStateInfo::Preset());

    auto raster_pipeline = manager
                               .Raster()
                               .Vertex("test/BasicVertex.hlsl")
                               .Pixel("test/BasicFrag.hlsl")
                               .Build<TestTrianglePipeline>(std::move(pso_info));
    struct Vertex {
        float3 pos;
        float2 uv;
    };
    Vertex vertices[] = {
        {{0.0f, -0.5f, 0.0f}, {0.5f, 1.0f}},
        {{-0.5f, 0.5f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f}},
    };
    uint indices[]     = {0, 1, 2};
    auto vertex_buffer = device.CreateBuffer<float>(3 * sizeof(Vertex) / sizeof(float), EBufferUsageFlags::VERTEX_BUFFER);
    auto index_buffer  = device.CreateBuffer<uint>(3, EBufferUsageFlags::INDEX_BUFFER);
    cmd_list.CopyFrom(std::span<byte>((byte*)vertices, sizeof(vertices)), vertex_buffer->GetView());
    cmd_list.CopyFrom(std::span<byte>((byte*)indices, sizeof(indices)), index_buffer->GetView());
    cmd_queue.Execute(cmd_list.Submit());
    cmd_queue.Sync();

    VertexBuffer vb(vertex_buffer, 0);
    IndexBuffer  ib(index_buffer->GetView(), EIndexElementType::IET_UINT32);
    while (WindowContext::ShouldClose(window_handle) == false) {
        WindowContext::Tick();

        Array<MeshDrawData> draw_datas;
        draw_datas.emplace_back(
            std::span<VertexBuffer>(&vb, 1),
            ib,
            1,
            0);
        int width, height;
        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &width, &height);
        while (width == 0 || height == 0) {
            std::this_thread::yield();
        }
        if(width != resolution.x || height != resolution.y){
            resolution = {uint32(width), uint32(height)};
            output = device.CreateTexture(
                Extent2D(resolution.x, resolution.y),
                PF_R8G8B8A8_SRGB,
                ETextureUsageFlags::COLOR_ATTACHMENT);
            cmd_queue.Sync();
            sc_info.size = {resolution.x, resolution.y};
            sc->Recreate(sc_info);
        }
        
        cmd_list.Gfx(raster_pipeline)
            .Draw(Rect2D(0, 0, resolution.x, resolution.y), std::move(draw_datas), ColorAttachment(output));
        cmd_queue.Execute(cmd_list.Submit());
        cmd_queue.Present(sc, output);
    }
    cmd_queue.Sync();
}