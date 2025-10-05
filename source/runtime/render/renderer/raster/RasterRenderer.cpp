#pragma once

// Runtime
#include "renderer/raster/RasterRenderer.h"

// Editor
#include "AaPass.h"
#include "AoPass.h"
#include "CudaPass.h"
#include "GeometryPass.h"
#include "LightingPass.h"
#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"
#include "RtaoPass.h"
#include "ShadowDepthPass.h"
#include "SsrPass.h"

namespace Moer::Render::Raster {

RasterRenderer::RasterRenderer(
    SharedPtr<uint2>                                          _resolution,
    const SharedPtr<EditorConfig>                             _config,
    const EngineHooks&                                        _hooks,
    std::function<void(const std::filesystem::path&, Scene*)> _load_scene_async
) :
    // Super
    Renderer(_resolution, _config, _hooks, _load_scene_async) {

    raster_context_ptr =
        MakeUnique<RasterContext>(device, manager, bindless_array, cmd_list, scene, resolution);
    auto& raster_context = *raster_context_ptr;

    raster_context.CreateFrameBuffers();
    raster_context.AllocateFrameBuffers();

    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    shadow_depth_pass = MakeUnique<ShadowDepthPass>(raster_context);
    geometry_pass     = MakeUnique<GeometryPass>(raster_context);
    lighting_pass     = MakeUnique<LightingPass>(raster_context);
    ao_pass           = MakeUnique<AoPass>(raster_context);
    rtao_pass         = MakeUnique<RtaoPass>(raster_context);
    ssr_pass          = MakeUnique<SsrPass>(raster_context);
    cuda_pass         = MakeUnique<CudaPass>(raster_context);
    aa_pass           = MakeUnique<AaPass>(raster_context);

    cmd_list.UpdateBindlessArray(bindless_array);
    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    // Other vars
}

RasterRenderer::~RasterRenderer() {
    auto& raster_context = *raster_context_ptr;
    raster_context.FreeFrameBuffers();

    // 下面这段需要在Renderer子类中执行。否则各种Pass对象被Release的时候，对应的资源还没有被释放
    ReleaseResources();
}

void RasterRenderer::Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        if (!RunSingle(editor_config, hooks)) {
            break;
        }
    }
}

bool RasterRenderer::RunSingle(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    auto& raster_context = *raster_context_ptr;

    if (hooks.on_tick_ui) {
        hooks.on_tick_ui();
    }

    if (hooks.on_raster_register_frame_buffers) {
        hooks.on_raster_register_frame_buffers(raster_context.GetDisplayableFrameBuffersView());
    }

    auto window_state = TickWindowContext(hooks);

    if (window_state == EWindowState::Hiding) {
        std::this_thread::yield(); // FIXME: 这个东西有用吗？

        // hook: RenderGUI
        if (hooks.on_render_gui) {
            hooks.on_render_gui(cmd_list, raster_context.textures.output.tex);
        }

        return true; // continue to next main loop body

    } else if (window_state == EWindowState::SizeChanged) {
        raster_context.FreeFrameBuffers();
        raster_context.CreateFrameBuffers();
        raster_context.AllocateFrameBuffers();

        if (hooks.on_raster_register_frame_buffers) {
            hooks.on_raster_register_frame_buffers(raster_context.GetDisplayableFrameBuffersView());
        }

    } else if (window_state == EWindowState::Default) {
        // do nothing

    } else {
        assert(false);
    }

    TextureRef default_output_texture = raster_context.textures.output.tex;

    if (Scene::GetCurrentSceneLoadInfo().Get() && Scene::GetCurrentSceneLoadInfo()->IsReady()) {
        if (first_load) {
            first_load = false;

            raster_context.LoadSceneData();
            RasterTool::InitRaytracingScene(raster_context, rt_geometries);

            gfx_queue.Execute(cmd_list.Submit());
            gfx_queue.Sync();
        }

        const auto& raster_config = editor_config->raster_config;
        auto        camera        = CameraManager::Get().Get(scene.GetCameras()[0]);

        {
            // Jitter Camera for SMAA T2x
            static uint8_t smaa_current_frame_index = 0;
            if (raster_config.aa_mode == EAaMode::SMAA_T2X) {

                smaa_current_frame_index ^= 1;
                static StaticArray<float2, 2> smaa_jitter = {float2(0.25f, -0.25f), float2(-0.25f, 0.25f)};
                camera->SetJitterMatrix(smaa_jitter[smaa_current_frame_index]);
            }
        }

        camera->Tick(editor_config->aspect_ratio, editor_config->camera_speed, editor_config->camera_fovy);

        // others
        RasterTool::UpdateRaytracingScene(raster_context);

        // Shadow Depth Pass
        shadow_depth_pass->Process(raster_context, raster_config, camera);

        // Geometry Pass
        geometry_pass->Process(raster_context, raster_config, camera);

        // Lighting Pass
        uint lighting_output = lighting_pass->Process(raster_context, raster_config, camera);

        // Post Process Passes
        // - Ambient Occlusion
        uint ao_output_ambient_only = 0;
        uint ao_output              = [&]() -> uint {
            if (raster_config.ao_mode == EAoMode::RTAO || raster_config.ao_mode == EAoMode::RTAO_AO_ONLY) {
                auto output =
                    rtao_pass->Process(raster_context, raster_config, camera, time, lighting_output);
                ao_output_ambient_only = output.ambient_only_output; // ambient only texture的handle
                return output.ao_output;                             // 实际输出的handle
            } else {
                return ao_pass->Process(raster_context, raster_config, lighting_output);
            }
        }();
        // - Screen Space Reflection
        uint ssr_output = ssr_pass->Process(raster_context, raster_config, camera, ao_output);

        // - CUDA Pass
        uint cuda_output = cuda_pass->Process(raster_context, raster_config, ssr_output);

        // - Anti-aliasing
        uint _aa_output = aa_pass->Process(raster_context, raster_config, camera, cuda_output);

        if (hooks.on_ui_combine_pass) {
            default_output_texture = hooks.on_ui_combine_pass(
                ui_combine_pass.get(),
                cmd_list,
                raster_context.GetSelectedFrameBufferView(raster_config.selected_frame_buffer_index),
                raster_context.textures.ui_frame_buffer.tex,
                raster_context.textures.output.tex
            );
        }

        // without test
        raster_context.rt_scene->AdvanceFrame();
    }

    if (hooks.on_render_gui) {
        hooks.on_render_gui(cmd_list, default_output_texture);
    }

    time++;
    /***
        currently using a phony timeline (any timeline signaled by copy queue) to remove error message from validation layer caused by host synced copy operations
        we're not waiting for the copy queue to finish, because operations we wanted are synced on host side, we use this timeline just to notifiy the validation layer
        that we've done flushing copy queue resources
        */
    gfx_queue.Execute(cmd_list.Submit().Signal(timeline, time).DeleteResources());
    gfx_queue.Present(swapchain, default_output_texture);

    if (hooks.on_present_windows) {
        hooks.on_present_windows();
    }
    if (hooks.on_is_need_reload && hooks.on_is_need_reload()) {
        return false; // break
    }

    return true;
}

} // namespace Moer::Render::Raster