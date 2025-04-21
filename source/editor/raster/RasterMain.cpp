#include "RasterMain.h"

// Runtime
#include "config/ConfigManager.h"
#include "loader/LoaderInterface.h"
#include "rhi/RHI.h"
#include "scene/CameraManager.h"
// #include "shader/GeometryPassPsoManager.h"
#include "shader/ShaderResourceManager.h"
#include "window/WindowContext.h"

// Editor
#include "AaPass.h"
#include "AoPass.h"
#include "GeometryPass.h"
#include "LightingPass.h"
#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"
#include "ShadowDepthPass.h"
#include "SsrPass.h"
#include "common/UiCombinePass.h"
#include "ui/raster_ui/RasterUI.h"

namespace Moer::Render::Raster {

void RasterMain(SharedPtr<EditorUI> editor_ui) {

    // Get a lot of things
    auto&            device              = RenderDevice::Get();
    auto&            manager             = ShaderManager::Get();
    Scene            scene               = {};
    BindlessArrayRef bindless_array      = scene.GetBindlessArray();
    auto&            gfx_queue           = device.GetCommandQueue(EQueueType::Graphics);
    auto&            copy_queue          = device.GetCopyQueue();
    auto             copy_queue_timeline = copy_queue.GetFenceHandle();
    CommandList      cmd_list            = {};

    // Initialize Swapchain
    auto resolution = editor_ui->GetResolution(); // TODO: 是否要从WindowContext中获取resolution?

    auto sc_info = SwapchainCreateInfo{
        .window_handle    = (uintptr_t)WindowContext::GetMainWindow(),
        .size             = {resolution.x, resolution.y},
        .back_buffer_sz   = 2,
        .preferred_format = PF_R8G8B8A8_SRGB
    };
    auto sc = device.CreateSwapchain(sc_info);

    // MARK: Scene
    Resource::LoaderInterface::LoadSceneFromFileAsync(editor_ui->GetConfig().scene_path, &scene);
    auto&& scope_exit_reset_async_load_info = OnScopeExit([&] { Scene::ResetAsyncLoadInfo(); });

    // TODO: combine RasterMain and RaytracingMain common part (above code)
    RasterContext raster_context(device, manager, bindless_array, cmd_list, scene, resolution);

    raster_context.CreateFrameBuffers();
    raster_context.AllocateFrameBuffers();
    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    editor_ui->m_raster_ui.RegisterFrameBuffers(raster_context.GetDisplayableFrameBuffersView());

    // MARK: Passes

    ShadowDepthPass shadow_depth_pass(raster_context);
    GeometryPass    geometry_pass(raster_context);
    LightingPass    lighting_pass(raster_context);
    AoPass          ao_pass(raster_context);
    SsrPass         ssr_pass(raster_context);
    AaPass          aa_pass(raster_context);

    UiCombinePass ui_combine_pass(manager);

    cmd_list.UpdateBindlessArray(bindless_array);
    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    // MARK: UI

    FenceRef timeline   = device.CreateFence();
    uint64   time       = 0;
    bool     first_load = true;

    editor_ui->SetShowSubUI(true);

    // MARK: Main Loop
    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        WindowContext::Tick();
        editor_ui->TickUI();
        if (time > 2) { timeline->Wait(time - 2); }
        const RasterConfig& ui_config = editor_ui->m_raster_ui.GetConfig();

        // MARK: Window Resizing
        int w_width, w_height;
        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w_width, &w_height);
        if (w_width == 0 || w_height == 0) {
            std::this_thread::yield();
            editor_ui->RenderGUI(cmd_list, raster_context.textures.output.tex);
            continue;
        }
        if (w_width != resolution.x || w_height != resolution.y) {
            resolution.x = uint32(w_width);
            resolution.y = uint32(w_height);
            gfx_queue.Sync();
            sc->Sync();
            sc_info.size = {resolution.x, resolution.y};
            sc->Recreate(sc_info);

            raster_context.FreeFrameBuffers();
            raster_context.CreateFrameBuffers();
            raster_context.AllocateFrameBuffers();

            editor_ui->m_raster_ui.RegisterFrameBuffers(raster_context.GetDisplayableFrameBuffersView());
        }
        TextureRef final_output = raster_context.textures.output.tex;
        if (Scene::GetCurrentSceneLoadInfo().Get() && Scene::GetCurrentSceneLoadInfo()->IsReady()) {
            // MARK: First Load
            if (first_load) {
                first_load = false;

                raster_context.LoadSceneData();

                uint last_io_change_timeline = copy_queue_timeline->GetValue();
                gfx_queue.Execute(cmd_list.Submit().Wait(copy_queue_timeline, last_io_change_timeline));
                gfx_queue.Sync();
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
            camera->Tick(editor_ui->GetSceneColorAspectRatio());

            // Shadow Depth Pass
            shadow_depth_pass.Process(raster_context, ui_config, camera);

            // Geometry Pass
            geometry_pass.Process(raster_context, ui_config, camera);

            // Lighting Pass
            uint lighting_output = lighting_pass.Process(raster_context, ui_config, camera);

            // Post Process Passes
            uint ao_output  = ao_pass.Process(raster_context, ui_config, lighting_output);
            uint ssr_output = ssr_pass.Process(raster_context, ui_config, camera, ao_output);
            uint _aa_output = aa_pass.Process(raster_context, ui_config, camera, ssr_output);

            // Ui Combine Pass
            final_output = ui_combine_pass.Process(
                raster_context.cmd_list,
                resolution,
                editor_ui->m_raster_ui.GetSelectedFrameBuffer(),
                raster_context.textures.ui_frame_buffer.tex,
                raster_context.textures.output.tex,
                editor_ui
            );
        }

        editor_ui->RenderGUI(cmd_list, final_output);

        time++;
        /***
        currently using a phony timeline (any timeline signaled by copy queue) to remove error message from validation layer caused by host synced copy operations
        we're not waiting for the copy queue to finish, because operations we wanted are synced on host side, we use this timeline just to notifiy the validation layer
        that we've done flushing copy queue resources
         */
        gfx_queue.Execute(cmd_list.Submit().Signal(timeline, time));
        gfx_queue.Present(sc, final_output);
        editor_ui->PresentWindows();

        if (editor_ui->IsNeedReload()) { break; }
    }
    gfx_queue.Sync();
    sc->Sync();
}

} // namespace Moer::Render::Raster