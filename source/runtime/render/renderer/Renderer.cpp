#include "renderer/Renderer.h"

// Runtime
#include "renderer/common/RuntimeAssets.h"
#include "config/ConfigManager.h"
#include "misc/Timer.h"
#include "renderer/EditorConfig.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "scene/SceneGlobalEntry.h"
#include "shader/ShaderResourceManager.h"
#include "window/WindowContext.h"

#include "common/UiCombinePass.h"

namespace Moer::Render {

Renderer::Renderer(
    uint2&                        _resolution,
    const SharedPtr<EditorConfig> _config,
    const EngineHooks&            hooks,
    ::Moer::RuntimeAssets&        _runtime_assets
) :
    resolution(_resolution),
    device(RenderDevice::Get()),
    manager(ShaderManager::Get()),
    gfx_queue(device.GetCommandQueue(EQueueType::Graphics)),
    runtime_assets(_runtime_assets),
    scene(),
    cmd_list() {

    {
        presentation_surface = MakeUnique<PresentationSurface>(
            device,
            PresentationSurfaceDesc{
                .window            = *WindowContext::GetMainWindow(),
                .size              = {resolution.x, resolution.y},
                .back_buffer_count = 2,
                .preferred_format  = PF_R8G8B8A8_SRGB,
                .debug_name        = "Main Presentation Surface",
            }
        );
    }
    {
        bindless_array = scene.bindless_array();
        scene.LoadSceneFromFileAsync(_config->scene_path);

        SceneGlobalEntry::Get().BindScene(&scene);
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
    presentation_surface->Sync();
    device.WaitIdle();

    cmd_list.UpdateBindlessArray(bindless_array);
    const uint64 cleanup_signal_value = ++time;
    cmd_list.DeleteResources().Signal(timeline, cleanup_signal_value);
    Array<CommandList> cleanup_cmd_lists{};
    cleanup_cmd_lists.emplace_back(std::move(cmd_list));
    RHIExecutor::Get().Submit(std::move(cleanup_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
    cmd_list = CommandList(EQueueType::Graphics);
    timeline->Wait(cleanup_signal_value);
    gfx_queue.Sync();

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
        presentation_surface->Resize({resolution.x, resolution.y});

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

void Renderer::PumpAsyncLoads() {
    runtime_assets.SubmitPendingUploads();
    scene.AdoptPendingAsyncLoad();
}

} // namespace Moer::Render
