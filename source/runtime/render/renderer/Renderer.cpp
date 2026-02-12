#pragma once

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

Renderer::Renderer(uint2& _resolution, const SharedPtr<EditorConfig> _config, const EngineHooks& hooks) :
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

        scene.LoadSceneFromFileAsync(_config->scene_path);
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
    ReleaseResources(); // 这里也需要释放，否则退出Engine时会崩溃 (Why?)
}

void Renderer::ReleaseResources() {
    timeline->Wait(time);
    gfx_queue.Sync();

    cmd_list.UpdateBindlessArray(bindless_array);
    gfx_queue.Execute(cmd_list.Submit().DeleteResources());
    gfx_queue.Sync();
    swapchain->Sync();

    scene.Reset();
}

Renderer::EWindowState Renderer::TickWindowContext(const EngineHooks& hooks) {
    WindowContext::Tick();
    if (time >= max_frame_in_flight) {
        timeline->Wait(time - max_frame_in_flight);
    }

    int w_width, w_height;
    WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w_width, &w_height);
    if (w_width == 0 || w_height == 0) {
        return EWindowState::Hiding; // 跳过Tick()

    } else if (w_width != resolution.x || w_height != resolution.y) {
        resolution.x = uint32(w_width);
        resolution.y = uint32(w_height);

        gfx_queue.Sync();
        swapchain_createinfo.size = {resolution.x, resolution.y};
        swapchain->Sync();
        swapchain->Recreate(swapchain_createinfo);

        return EWindowState::SizeChanged; // 继续执行Tick()

    } else {
        return EWindowState::Default; // 继续执行Tick()
    }
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