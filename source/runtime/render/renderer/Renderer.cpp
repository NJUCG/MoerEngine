#include "renderer/Renderer.h"

// Runtime
#include "config/ConfigManager.h"
#include "misc/Timer.h"
#include "renderer/EditorConfig.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "shader/ShaderResourceManager.h"
#include "window/WindowContext.h"

#include "common/UiCombinePass.h"

namespace Moer::Render {

Renderer::Renderer(uint2 _resolution, const SharedPtr<EditorConfig> _config, const EngineHooks& hooks) :
    resolution(_resolution),
    device(RenderDevice::Get()),
    manager(ShaderManager::Get()),
    gfx_queue(device.GetCommandQueue(EQueueType::Graphics)),
    scene(),
    cmd_list() {

    {
        swapchain_createinfo = SwapchainCreateInfo{
            .window_handle    = (uintptr_t)WindowContext::GetMainWindow(),
            .size             = {resolution.x, resolution.y},
            .back_buffer_sz   = 2,
            .preferred_format = PF_R8G8B8A8_SRGB
        };
        swapchain = device.CreateSwapchain(swapchain_createinfo);
    }
    {
        bindless_array = scene.bindless_array();

        // FIXME: 异步版有bug，会在gfx_queue.Execute()卡死，并且会卡住整台机器一分钟
        // scene.LoadSceneFromFileAsync(_config->scene_path);
        scene.LoadSceneFromFile(_config->scene_path);
    }
    // Other vars
    {
        timeline            = device.CreateFence();
        time                = 0ull;
        first_load          = true;
        max_frame_in_flight = ConfigManager::GetInstance().GetConfig().engine.rhi.max_frame_in_flight;
    }
    {
        ui_combine_pass = MakeUnique<UiCombinePass>(manager);
    }
    // Show sub ui
    if (hooks.on_show_config_sub_ui) {
        hooks.on_show_config_sub_ui();
    }
}

Renderer::~Renderer() {
    // ReleaseResources(); // 在Renderer子类中释放
}

void Renderer::ReleaseResources() {
    if (released)
        return;
    released = true;

    timeline->Wait(time);
    gfx_queue.Sync();
    swapchain->Sync();
    device.WaitIdle();

    cmd_list.UpdateBindlessArray(bindless_array);
    gfx_queue.Execute(cmd_list.Submit().DeleteResources());
    gfx_queue.Sync();

    scene.Reset();
}

Renderer::WindowFrameState Renderer::TickWindowContext(uint2 current_resolution) {
    WindowContext::Tick();

    int w_width, w_height;
    WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w_width, &w_height);
    if (w_width == 0 || w_height == 0) {
        return WindowFrameState{EWindowState::Hiding, current_resolution};

    } else if (w_width != current_resolution.x || w_height != current_resolution.y) {
        return WindowFrameState{
            EWindowState::SizeChanged,
            uint2(static_cast<uint32>(w_width), static_cast<uint32>(w_height))
        };

    }

    return WindowFrameState{EWindowState::Default, current_resolution};
}

void Renderer::PrepareRenderFrame(const WindowFrameState& window_frame) {
    resolution = window_frame.resolution;

    if (time >= max_frame_in_flight) {
        timeline->Wait(time - max_frame_in_flight);
    }

    if (window_frame.state != EWindowState::SizeChanged) {
        return;
    }

    gfx_queue.Sync();
    swapchain_createinfo.size = {resolution.x, resolution.y};
    swapchain->Sync();
    swapchain->Recreate(swapchain_createinfo);
}

void Renderer::LogSceneLoadStatus(const EditorConfig& config) const {
    if (scene.IsStartLoading() == false) {
        // 没有找到场景，每隔一段时间在命令行打印提示信息，避免用户不知道发生了什么
        static LoopedTimer timer(2.0);
        if (timer.Tick()) { // 每隔1s触发一次
            LOG_WARNING(
                "Don't find scene or scene format isn't supported. Please load a valid scene. Latest "
                "attempted scene: {}",
                config.scene_path
            );
        }
    }
}

} // namespace Moer::Render
