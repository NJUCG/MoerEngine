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
#include "shader/ShaderResourceManager.h"
#include "log/LogSystem.h"
#include "RenderThread.h"
#include "taskgraph/GraphTask.h"
#include "window/WindowContext.h"
#include "imgui.h"
#include "core/include/Core.h"

int main(int argc, const char** argv) {

    using namespace Moer::Render;
    using namespace Moer;
    std::filesystem::path path = argv[0];
    path.filename().string().find(".exe") != std::string::npos ? path = path.parent_path() : path = path;
    ConfigManager::GetInstance().Init(path);
    DeviceInitInfo info{.rhi_type = ERHIType::Vulkan, .name = "RHITest", .ray_tracing = true};
    RenderDevice::Init(std::move(info));
    auto& device = RenderDevice::Get();

    SurfaceInitInfo surface_info("Vulkan", 1280, 720, "RHITest", false);
    WindowContext::Init(surface_info);
    auto&& scope_exit = OnScopeExit([&] {
        WindowContext::ShutDown();
        RenderDevice::Dispose();
    });
    auto* window_handle = WindowContext::GetMainWindow();

    auto buf = device.CreateBuffer<float>(1024, EBufferUsageFlags::UNORDERED_ACCESS);
    auto sc  = device.CreateSwapchain(SwapchainCreateInfo{.window_handle = (uintptr_t)window_handle, .size = {1280, 720}, .back_buffer_sz = 2, .preferred_format = PF_R8G8B8A8_SRGB});

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

    ubyte*    pixels;
    int width, height;
    uint alignment = 4;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    uint32_t   upload_pitch = (width * 4 + alignment - 1u) & ~(alignment - 1u);
    uint32_t   upload_size  = height * upload_pitch;
    TextureRef font_tex     = device.CreateTexture(
        Extent2D(width, height),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::SAMPLED);

    cmd_list.CopyFrom(
        std::span<std::byte>((std::byte*)pixels, upload_size), font_tex);

    cmd_queue.Execute(cmd_list.Submit());
    cmd_queue.Sync();

    TextureView font_view = font_tex->GetView();
    font_view.extent = uint3(1280, 720, 1);
    while (WindowContext::ShouldClose(window_handle) == false) {
        WindowContext::Tick();
        cmd_queue.Present(sc, font_view);
    }
}