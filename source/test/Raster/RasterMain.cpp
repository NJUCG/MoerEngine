#include "Core.h"
#include "PixelFormat.h"
#include "RenderThread.h"
#include "config/ConfigManager.h"
#include "core/include/Core.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/LogSystem.h"
#include "math/Constant.h"
#include "math/Matrix.h"
#include "misc/MMemory.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "modules/render/source/rhi/RHIImpl.h"
#include "modules/resource/include/loader/LoaderInterface.h"
#include "renderer/UIRenderer.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "scene/CameraManager.h"
#include "scene/Material.h"
#include "scene/Scene.h"
#include "shader/Shader.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"
#include <filesystem>
#include <stb_image.h>

#include "RasterResource.h"
#include "RasterUI.h"

#include "AaPass.h"
#include "AoPass.h"
#include "GeometryPass.h"
#include "LightingPass.h"
#include "SsrPass.h"
#include "UiCombinePass.h"

// MARK: Main Function
int main(int argc, const char** argv) {

    using namespace Moer::Render;
    using namespace Moer;

    // TODO:
    std::filesystem::path path = argv[0];
    path.filename().string().find(".exe") != std::string::npos ? path = path.parent_path() : path = path;
    ConfigManager::GetInstance().Init(path);
    TaskSystem::Init();
    const auto& rhi_config_as_json = ConfigManager::GetInstance().GetRHIConfigAsJSON();

    DeviceInitInfo info{.type = ERHIType::Vulkan, .name = "Raster", .config_as_json = rhi_config_as_json};
    RenderDevice::Init(std::move(info));

    auto& device  = RenderDevice::Get();
    auto& manager = ShaderManager::Get();

    uint2           resolution = {1280, 720};
    SurfaceInitInfo surface_info("Vulkan", resolution.x, resolution.y, "Raster", false);
    WindowContext::Init(surface_info);

    auto&& scope_exit_window_context_and_etc = OnScopeExit([&] {
        GeometryPassPsoManager::ShutDown();
        ShaderManager::ShutDown();
        WindowContext::ShutDown();
        RenderDevice::Dispose();
        TaskSystem::ShutDown();
    });

    Moer::Render::UIRenderer gui(device);

    auto* window_handle = WindowContext::GetMainWindow();

    SwapchainCreateInfo sc_info{
        .window_handle    = (uintptr_t)window_handle,
        .size             = {resolution.x, resolution.y},
        .back_buffer_sz   = 2,
        .preferred_format = PF_R8G8B8A8_SRGB
    };
    auto             sc             = device.CreateSwapchain(sc_info);
    Scene            scene          = {};
    BindlessArrayRef bindless_array = scene.GetBindlessArray();
    auto&            gfx_queue      = device.GetCommandQueue(EQueueType::Graphics);
    auto&            copy_queue     = device.GetCopyQueue();

    auto        copy_queue_timeline = copy_queue.GetFenceHandle();
    CommandList cmd_list{};

    // MARK: Raster Context

    RasterContext raster_context(device, manager, bindless_array, cmd_list, scene, resolution);

    raster_context.CreateFrameBuffers();
    raster_context.AllocateFrameBuffers();

    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    // MARK: Passes

    GeometryPass  geometry_pass(raster_context);
    LightingPass  lighting_pass(raster_context);
    AoPass        ao_pass(raster_context);
    SsrPass       ssr_pass(raster_context);
    AaPass        aa_pass(raster_context);
    UiCombinePass ui_combine_pass(raster_context);

    cmd_list.UpdateBindlessArray(bindless_array);
    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    // MARK: Scene

    Resource::LoaderInterface::LoadSceneFromFileAsync(ConfigManager::GetInstance().GetScenePath(), &scene);
    auto&& scope_exit_reset_async_load_info = OnScopeExit([&] { Scene::ResetAsyncLoadInfo(); });

    // MARK: UI

    RasterUI raster_ui(gui, raster_context.GetDisplayableFrameBuffersView());

    FenceRef timeline   = device.CreateFence();
    uint64   time       = 0;
    bool     first_load = true;

    // MARK: Main Loop
    while (WindowContext::ShouldClose(window_handle) == false) {
        WindowContext::Tick();
        gui.BeginGUIFrame();
        { raster_ui.TickUI(); }
        gui.EndGUIFrame();
        if (time > 2) { timeline->Wait(time - 2); }

        const RasterUI::Config& ui_config = raster_ui.GetConfig();

        // MARK: Window Resizing
        int w_width, w_height;
        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w_width, &w_height);
        if (w_width == 0 || w_height == 0) {
            std::this_thread::yield();
            continue;
        }
        if (w_width != resolution.x || w_height != resolution.y) {
            resolution.x = uint32(w_width);
            resolution.y = uint32(w_height);
            gfx_queue.Sync();
            sc_info.size = {resolution.x, resolution.y};
            sc->Recreate(sc_info);

            raster_context.FreeFrameBuffers();
            raster_context.CreateFrameBuffers();
            raster_context.AllocateFrameBuffers();

            raster_ui.RegisterFrameBuffers(raster_context.GetDisplayableFrameBuffersView());
        }

        TextureRef final_output = raster_context.textures.output.tex;
        if (Scene::GetCurrentSceneLoadInfo().Get() && Scene::GetCurrentSceneLoadInfo()->IsReady()) {
            // MARK: First Load
            if (first_load) {
                first_load = false;

                raster_context.LoadSceneData();

                uint last_io_change_timeline = copy_queue_timeline->GetValue();
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
            camera->Tick(raster_ui.GetSceneColorAspectRatio());

            // Geometry Pass
            geometry_pass.Process(raster_context, ui_config, camera);

            // Lighting Pass
            uint lighting_output = lighting_pass.Process(raster_context, ui_config, camera);

            // Post Process Passes
            uint ao_output  = ao_pass.Process(raster_context, ui_config, lighting_output);
            uint ssr_output = ssr_pass.Process(raster_context, ui_config, camera, ao_output);
            uint _aa_output = aa_pass.Process(raster_context, ui_config, camera, ssr_output);

            // Ui Combine Pass
            final_output = ui_combine_pass.Process(raster_context, ui_config, raster_ui);
        }

        gui.RenderGUI(cmd_list, final_output);

        time++;
        /***
        currently using a phony timeline (any timeline signaled by copy queue) to remove error message from validation layer caused by host synced copy operations
        we're not waiting for the copy queue to finish, because operations we wanted are synced on host side, we use this timeline just to notifiy the validation layer
        that we've done flushing copy queue resources
         */
        gfx_queue.Execute(cmd_list.Submit().Signal(timeline, time).Wait(copy_queue_timeline, 0));
        gfx_queue.Present(sc, final_output);
    }
    gfx_queue.Sync();

    return 0;
}