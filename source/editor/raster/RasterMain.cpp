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
#include "RtaoPass.h"
#include "ShadowDepthPass.h"
#include "SsrPass.h"
#include "common/UiCombinePass.h"
#include "ui/raster_ui/RasterUI.h"

namespace Moer::Render::Raster {

/**
 * Raster渲染方法TODO Lists
 * 
 * TODO: 着色，LightingPass，目前还比较初步
 * TODO: IBL
 * TODO: 阴影，ShadowDepthPass和LightingPass
 *       1. CSM中，ShadowMap的mipmap好像有问题，貌似目前并没有构建，导致效果不好，需要构建一下mipmap
 *       2. CSM层间混合
 *       3. 多种采样方法支持（目前是NoFiltering，可以考虑添加不同精度的PCF）
 *       4. 剔除。目前把场景绘制CSM层数遍，性能开销巨大
 *       5. VSM支持
 * TODO: SSR，SsrPass
 *       1. SSR的效果还不够好，会出现断层（用jitter修复后仍有一些问题），可以考虑换一个新的SSR算法
 *       2. 考虑使用HiZ来加速ssr
 *       3. 对Glossy材质的支持
 *       4. 性能优化
 * TODO: 抗锯齿，AaPass，目前SMAA T2x还有一些问题，效果不明显，可能是velocity buffer寄了
 * TODO: 环境光遮蔽，AoPass，可以在SSAO之外多加一些环境光遮蔽算法，比如SSDO、GTAO
 * TODO: 其他后处理Pass，或许可以考虑从RT搬过来用233
 */
void RasterMain(SharedPtr<EditorUI> editor_ui) {

    // Get a lot of things
    auto&            device         = RenderDevice::Get();
    auto&            manager        = ShaderManager::Get();
    Scene            scene          = {};
    BindlessArrayRef bindless_array = scene.GetBindlessArray();
    auto&            gfx_queue      = device.GetCommandQueue(EQueueType::Graphics);
    CommandList      cmd_list       = {};

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
    RtaoPass        rtao_pass(raster_context);
    SsrPass         ssr_pass(raster_context);
    AaPass          aa_pass(raster_context);

    UiCombinePass ui_combine_pass(manager);

    cmd_list.UpdateBindlessArray(bindless_array);
    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    // MARK: UI

    FenceRef timeline            = device.CreateFence();
    uint64   time                = 0;
    uint     max_frame_in_flight = ConfigManager::GetInstance().GetConfig().engine.rhi.max_frame_in_flight;
    bool     first_load          = true;

    // FIXME: move it to another place
    Array<RaytracingGeometryRef> rt_geometries;

    editor_ui->SetShowSubUI(true);

    // MARK: Main Loop
    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        WindowContext::Tick();
        editor_ui->TickUI();
        if (time >= max_frame_in_flight) { timeline->Wait(time - max_frame_in_flight); }
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
            sc_info.size = {resolution.x, resolution.y};
            sc->Sync();
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

                RasterTool::InitRaytracingScene(raster_context, rt_geometries);

                gfx_queue.Execute(cmd_list.Submit());
                gfx_queue.Sync();
            }

            RasterTool::UpdateRaytracingScene(raster_context);

            // MARK: Camera

            auto camera_entity = scene.GetCameras()[0];
            auto camera        = CameraManager::Get().Get(camera_entity);

            // Jitter Camera for SMAA T2x
            static uint8_t smaa_current_frame_index = 0;
            if (ui_config.aa_mode == EAaMode::SMAA_T2X) {
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
            // - Ambient Occlusion
            uint ao_output_ambient_only = 0;
            uint ao_output              = [&]() -> uint {
                if (ui_config.ao_mode == EAoMode::RTAO) {
                    auto output = rtao_pass.Process(raster_context, ui_config, camera, time, lighting_output);
                    ao_output_ambient_only = output.ambient_only_output; // ambient only texture的handle
                    return output.ao_output;                             // 实际输出的handle
                } else {
                    return ao_pass.Process(raster_context, ui_config, lighting_output);
                }
            }();
            // - Screen Space Reflection
            uint ssr_output = ssr_pass.Process(raster_context, ui_config, camera, ao_output);
            // - Anti-aliasing
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

        raster_context.rt_scene->AdvanceFrame();

        time++;
        /***
        currently using a phony timeline (any timeline signaled by copy queue) to remove error message from validation layer caused by host synced copy operations
        we're not waiting for the copy queue to finish, because operations we wanted are synced on host side, we use this timeline just to notifiy the validation layer
        that we've done flushing copy queue resources
         */
        gfx_queue.Execute(cmd_list.Submit().Signal(timeline, time).DeleteResources());
        gfx_queue.Present(sc, final_output);
        editor_ui->PresentWindows();

        if (editor_ui->IsNeedReload()) { break; }
    }
    timeline->Wait(time);
    gfx_queue.Sync();

    raster_context.FreeFrameBuffers();

    cmd_list.UpdateBindlessArray(bindless_array);
    gfx_queue.Execute(cmd_list.Submit().DeleteResources());
    gfx_queue.Sync();
    sc->Sync();
}

} // namespace Moer::Render::Raster