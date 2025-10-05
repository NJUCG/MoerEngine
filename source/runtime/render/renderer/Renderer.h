#pragma once

// Runtime
#include "config/ConfigManager.h"
#include "loader/LoaderInterface.h"
#include "renderer/EditorConfig.h"
#include "rhi/RHI.h"
#include "scene/CameraManager.h"
#include "shader/ShaderResourceManager.h"
#include "window/WindowContext.h"

#include "common/UiCombinePass.h"

namespace Moer::Render {

struct EngineHooks {
    // Common
    std::function<void(void)>                     on_tick_ui;
    std::function<void(CommandList&, TextureRef)> on_render_gui;
    std::function<void(void)>                     on_present_windows;
    std::function<bool(void)>                     on_is_need_reload;

    std::function<TextureRef(UiCombinePass*, CommandList&, TextureView, TextureView, TextureView)>
        on_ui_combine_pass;

    std::function<void(std::string, std::function<void(void)>)> on_register_ui_func;

    std::function<void(std::string)> on_unregister_ui_func;

    std::function<void(void)> on_show_config_sub_ui;

    // Raster
    std::function<void(const Array<TextureView>&)> on_raster_register_frame_buffers;
};

class RENDER_API Renderer {

public:
    enum class EWindowState {
        Default = 0,
        Hiding,
        SizeChanged,
        Num,
    };

    struct TickResult {
        CommandList&     cmd_list;
        SharedPtr<uint2> resolution;
        TextureView      input_image_texture;
        TextureView      input_ui_texture;
        TextureView      output_texture;
    };

public:
    Renderer(
        SharedPtr<uint2>                                          _resolution,
        const SharedPtr<EditorConfig>                             _config,
        const EngineHooks&                                        hooks,
        std::function<void(const std::filesystem::path&, Scene*)> _load_scene_async
    ) :
        resolution(_resolution),
        device(RenderDevice::Get()),
        manager(ShaderManager::Get()),
        gfx_queue(device.GetCommandQueue(EQueueType::Graphics)),
        scene(),
        cmd_list() {

        {
            swapchain_createinfo = SwapchainCreateInfo{
                .window_handle    = (uintptr_t)WindowContext::GetMainWindow(),
                .size             = {resolution->x, resolution->y},
                .back_buffer_sz   = 2,
                .preferred_format = PF_R8G8B8A8_SRGB
            };
            swapchain = device.CreateSwapchain(swapchain_createinfo);
        }
        bindless_array = scene.GetBindlessArray();
        {
            _load_scene_async(_config->scene_path, &scene);
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

    virtual ~Renderer() {
        // ReleaseResources(); // 应该在子类中调用，否则子类中的Pass资源无法被正常释放
    }

    void ReleaseResources() {
        timeline->Wait(time);
        gfx_queue.Sync();

        cmd_list.UpdateBindlessArray(bindless_array);
        gfx_queue.Execute(cmd_list.Submit().DeleteResources());
        gfx_queue.Sync();
        swapchain->Sync();

        Scene::ResetAsyncLoadInfo();
    }

    EWindowState TickWindowContext(const EngineHooks& hooks) {
        WindowContext::Tick();
        if (time >= max_frame_in_flight) {
            timeline->Wait(time - max_frame_in_flight);
        }

        int w_width, w_height;
        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w_width, &w_height);
        if (w_width == 0 || w_height == 0) {
            return EWindowState::Hiding; // 跳过Tick()

        } else if (w_width != resolution->x || w_height != resolution->y) {
            resolution->x = uint32(w_width);
            resolution->y = uint32(w_height);

            gfx_queue.Sync();
            swapchain_createinfo.size = {resolution->x, resolution->y};
            swapchain->Sync();
            swapchain->Recreate(swapchain_createinfo);

            return EWindowState::SizeChanged; // 继续执行Tick()

        } else {
            return EWindowState::Default; // 继续执行Tick()
        }
    }

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    virtual void Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) = 0;

    CommandList& GetCommandList() {
        return cmd_list;
    }

protected:
    RenderDevice&  device;
    ShaderManager& manager;
    CommandQueue&  gfx_queue;

    SharedPtr<uint2> resolution;
    SwapchainRef     swapchain;
    BindlessArrayRef bindless_array;

    SwapchainCreateInfo swapchain_createinfo;
    Scene               scene;
    CommandList         cmd_list;

    UniquePtr<UiCombinePass> ui_combine_pass;

    // Other vars
    FenceRef timeline;
    uint64   time;
    bool     first_load;
    uint     max_frame_in_flight;
};

} // namespace Moer::Render