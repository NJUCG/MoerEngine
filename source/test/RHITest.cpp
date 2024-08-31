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
    auto* window_handle = WindowContext::GetMainWindow();

    auto buf = device.CreateBuffer<float>(1024, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST);
    auto sc  = device.CreateSwapchain(SwapchainCreateInfo{.window_handle = (uintptr_t)window_handle, .size = {1280, 720}, .back_buffer_sz = 2, .preferred_format = PF_R8G8B8A8_SRGB});
    auto img = device.CreateTexture({1280, 720}, PF_R8G8B8A8_SRGB, ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::TRANSFER_SRC);

    auto&       cmd_queue = device.GetCommandQueue(EQueueType::Graphics);
    CommandList cmd_list;
    auto buffer = device.CreateBuffer<uint>(1024, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |  EBufferUsageFlags::TRANSFER_SRC);
    Array<uint> data(1024);
    for (uint i = 0; i < 1024; ++i) {
        data[i] = i;
    }
    Array<uint> dst_data(1024);
    cmd_list.CopyFrom(std::span<byte>((byte*)data.data(), data.size() * sizeof(uint)), buffer->GetView());
    cmd_list.CopyFrom(buffer->GetView(), std::span<byte>((byte*)dst_data.data(), dst_data.size() * sizeof(uint)));

    cmd_queue.Execute(cmd_list.Submit());
    cmd_queue.Sync();
    while (WindowContext::ShouldClose(window_handle) == false) {
        WindowContext::Tick();
        cmd_queue.Present(sc, img->GetView());
    }
}