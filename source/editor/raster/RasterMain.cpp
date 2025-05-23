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
//#include "imgui.h"
#include "misc/Timer.h"
#include "ui/raster_ui/RasterUI.h"
#include <stb/stb_image_write.h>

namespace Moer::Render::Raster {
//class FillTexturePipeline : public ComputePipeline {
//public:
//    struct Arg {
//        uint  frame_cnt;
//        float time;
//        uint2 _pad;
//    };
//    DEFINE_COMPUTE_PIPELINE_CLASS(FillTexturePipeline);
//    DEFINE_SHADER_CONSTANT_STRUCT(Arg, args);
//    DEFINE_SHADER_TEX(tex);
//
//    DEFINE_SHADER_ARGS(args, tex);
//};

auto dequantentize_byte_to_srgb(unsigned char _b) {
    float c = _b / 255.f;
    c       = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;

    // to byte
    return (unsigned char)(c * 255.f);
};

/**
 * Raster渲染方法TODO Lists
 *
 * TODO: 着色，LightingPass，目前还比较初步
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
        .back_buffer_sz   = 3,
        .preferred_format = PF_R8G8B8A8_UNORM
    };
    auto sc = device.CreateSwapchain(sc_info);

    // MARK: Scene
    //Resource::LoaderInterface::LoadSceneFromFileAsync(editor_ui->GetConfig().scene_path, &scene);
    //auto&& scope_exit_reset_async_load_info = OnScopeExit([&] { Scene::ResetAsyncLoadInfo(); });

     //TODO: combine RasterMain and RaytracingMain common part (above code)
    RasterContext raster_context(device, manager, bindless_array, cmd_list, scene, resolution);

    raster_context.CreateFrameBuffers();
    raster_context.AllocateFrameBuffers();

    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    // pix capture(by gui) crash on bindlessarray related buffer. TODO

    editor_ui->m_raster_ui.RegisterFrameBuffers(raster_context.GetDisplayableFrameBuffersView());



    //auto  fill_pso = ShaderManager::Get().Compute<FillTexturePipeline>("test/FillTexture.hlsl");
    Timer timer;
    timer.Start();
    //size_t            size = sizeof(uint) * resolution.x * resolution.y;
    //Array<Moer::byte> copy_back_data[5];
    //for (int i = 0; i < 5; ++i) copy_back_data[i].resize(size);
    //bool b_save_screenshot = false;
    //uint starttime         = 999999999;

    //// MARK: Passes

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
        //LOG_INFO("RasterMain Loop---------------------------------");
        WindowContext::Tick();
        editor_ui->TickUI();
        //if (time > 2) { timeline->Wait(time - 2); }
        const RasterConfig& ui_config = editor_ui->m_raster_ui.GetConfig();

        // MARK: Window Resizing
        int w_width, w_height;
        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w_width, &w_height);
        if (w_width == 0 || w_height == 0) {
            std::this_thread::yield();
            //ImGui::UpdatePlatformWindows();

            editor_ui->RenderGUI(cmd_list, raster_context.textures.output.tex);
            continue;
        }
        if (w_width != resolution.x || w_height != resolution.y) {
            resolution.x = uint32(w_width);
            resolution.y = uint32(w_height);
            //gfx_queue.Sync();
            sc->Sync();
            sc_info.size = {resolution.x, resolution.y};
            sc->Recreate(sc_info);

            // !not recommend, due to texture row alignment
            //size = sizeof(uint) * resolution.x * resolution.y;
            //for (int i = 0; i < 5; ++i) copy_back_data[i].resize(size);

            raster_context.FreeFrameBuffers();
            raster_context.resolution = resolution;
            raster_context.CreateFrameBuffers();
            raster_context.AllocateFrameBuffers();
            editor_ui->m_raster_ui.RegisterFrameBuffers(raster_context.GetDisplayableFrameBuffersView());
        }
        TextureRef final_output = raster_context.textures.output.tex;

        /*
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
            */
        editor_ui->RenderGUI(cmd_list, final_output);
        //editor_ui->RegisterUIFunc("Save Screenshot start ", [&b_save_screenshot, &starttime, time] {
        //    if (ImGui::IsKeyPressed(ImGuiKey::ImGuiKey_S)) {
        //        b_save_screenshot = true;
        //        starttime         = time;
        //    }
        //});
        //if (time > starttime + 10) { b_save_screenshot = false; }
        //if (b_save_screenshot) {
        //    //LOG_INFO("Save Screenshot, start {}, time {}", starttime, time);
        //    cmd_list.CopyFrom(final_output->GetView(), copy_back_data[time % 5]);
        //    cmd_list.AddCallback(
        //        [&data0(copy_back_data[(time - 3 + 5) % 5]), resolution, time(time - 3)](
        //        ) { // retrieve a 'before' copybackdata because of AddCallback order. this happens before current frame readback callback
        //            LambdaTask::Create([data_in(data0), resolution, time]() {
        //                auto data = data_in; // copy

        //                /*             for (size_t i = 0; i < data.size(); i += 4) {
        //            data[i]     = (Moer::byte)dequantentize_byte_to_srgb(ubyte(data[i]));
        //            data[i + 1] = (Moer::byte)dequantentize_byte_to_srgb(ubyte(data[i + 1]));
        //            data[i + 2] = (Moer::byte)dequantentize_byte_to_srgb(ubyte(data[i + 2]));
        //            data[i + 3] = (Moer::byte)dequantentize_byte_to_srgb(ubyte(data[i + 3]));
        //        }*/

        //                stbi_write_png(
        //                    std::format("screenshot_{}.png", time).c_str(),
        //                    resolution.x,
        //                    resolution.y,
        //                    4,
        //                    (void*)data.data(),
        //                    4 * resolution.x
        //                );
        //                LOG_INFO("Screenshot saved, {}", time);
        //            }).Dispatch();
        //        }
        //    );
        //}

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