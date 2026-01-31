#pragma once

#include "renderer/raster/RasterRenderer.h"

#include "AaPass.h"
#include "AoPass.h"
#include "BilateralFilterDenoiserPass.h"
#include "BloomPass.h"
#include "DirectionalShadowMaskPass.h"
#include "GeometryPass.h"
#include "LightingPass.h"
#include "RasterResource.h"
#include "RasterTextures.h"
#include "RasterTool.h"
#include "RtaoDenoiserPass.h"
#include "ShadowDepthPass.h"
#include "SkyboxPass.h"
#include "SsrPass.h"
#include "TonemappingPass.h"
#include "debug/RenderDocApi.h"
#include "misc/Timer.h"

#if WITH_CUDA
#include "CudaPass.h"
#include "TensorRTPass.h"
#include "UpsamplePass.h"
#endif

namespace Moer::Render::Raster {

RasterRenderer::RasterRenderer(
    uint2&                                                    _resolution,
    const SharedPtr<EditorConfig>                             _config,
    const EngineHooks&                                        _hooks,
    std::function<void(const std::filesystem::path&, Scene*)> _load_scene_async
) :
    // Super
    Renderer(_resolution, _config, _hooks, _load_scene_async) {

    raster_context_ptr =
        MakeUnique<RasterContext>(device, manager, gfx_queue, bindless_array, cmd_list, scene, resolution);
    auto& raster_context = *raster_context_ptr;

    raster_context.CreateFrameBuffers();
    raster_context.UploadExternalFrameBuffers();
    raster_context.AllocateFrameBuffers();

    gfx_queue.Execute(cmd_list.Submit());
    gfx_queue.Sync();

    shadow_depth_pass            = MakeUnique<ShadowDepthPass>(raster_context);
    directional_shadow_mask_pass = MakeUnique<DirectionalShadowMaskPass>(raster_context);
    geometry_pass                = MakeUnique<GeometryPass>(raster_context);
    lighting_pass                = MakeUnique<LightingPass>(raster_context);
    skybox_pass                  = MakeUnique<SkyboxPass>(raster_context);
    ao_pass                      = MakeUnique<AoPass>(raster_context);
    rtao_denoiser_pass           = MakeUnique<RtaoDenoiserPass>(raster_context);
    bfd_pass                     = MakeUnique<BilateralFilterDenoiserPass>(raster_context);
    ssr_pass                     = MakeUnique<SsrPass>(raster_context);
    aa_pass                      = MakeUnique<AaPass>(raster_context);
    bloom_pass                   = MakeUnique<BloomPass>(raster_context);
    tonemapping_pass             = MakeUnique<TonemappingPass>(raster_context);

#if WITH_CUDA
    // 固定CudaPass位于AoPass之后（需要保证AoPass必定往 ao_output 中写入数据
    cuda_pass      = MakeUnique<CudaPass>(raster_context, raster_context.textures.ao_output.tex);
    tensor_rt_pass = MakeUnique<TensorRTPass>(
        raster_context,
        raster_context.textures.ao_output_ambient_only.tex,
        raster_context.textures.depth_linear_sampler.tex->CastToTextureRef(),
        // 下面这个color，需要传入lighting_output，而非ao_output，因为模型的输入需要不带ao的color
        raster_context.textures.lighting_output.tex,
        raster_context.textures.camera_motion_vector.tex,
        raster_context.textures.ao_output_ambient_only_1.tex
    );
    upsample_pass = MakeUnique<UpsamplePass>(raster_context);
#endif

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

void RasterRenderer::UpdateGlobalLightingData(
    RasterContext&      context,
    const RasterConfig& ui_config,
    const CameraRef&    camera
) {
    uint          csm_layers    = ui_config.shadow_csm_num_of_cascades;
    LightingData* lighting_data = &context.lighting_data;

    lighting_data->inv_view_proj   = Transpose(camera->GetViewProjectionMatrixInv());
    lighting_data->light_count     = context.scene.GetLights().size();
    lighting_data->camera_position = camera->GetPosition();

    // Shadow Parameters
    lighting_data->shadow_map_mode              = static_cast<int>(ui_config.shadow_map_mode);
    lighting_data->shadow_sampling_mode         = ui_config.shadow_sampling_mode;
    lighting_data->shadow_csm_num_of_cascades   = csm_layers;
    lighting_data->shadow_csm_sm_size           = ui_config.shadow_csm_sm_size;
    lighting_data->shadow_csm_visualize_cascade = ui_config.shadow_csm_visualize_cascade;

    // Shadow Map
    for (uint i = 0; i < csm_layers; i++) {
        lighting_data->cascade_shadow_map[i] = context.csm_data.shadow_map_textures[i].handle;
    }
    lighting_data->point_shadow_map = context.point_shadow_data.shadow_cubes[0].handle;
    lighting_data->light_pos        = context.point_shadow_data.shadow_cubes[0].light_pos;
    lighting_data->light_radius     = context.point_shadow_data.shadow_cubes[0].far_plane;

    // Shadow Transform
    for (uint i = 0; i < csm_layers; i++) {
        lighting_data->world_to_shadow_clip[i] = Transpose(lighting_data->world_to_shadow_clip[i]);
    }
    lighting_data->view_matrix = Transpose(camera->GetViewMatrix());
    lighting_data->near_clip   = camera->GetNearClip();
    lighting_data->far_clip    = camera->GetFarClip();

    lighting_data->is_csm_blend_enabled = ui_config.shadow_csm_blend_option ? 1 : 0;
    // 注：此处不一定使用所有CSM，Shader中具体根据shadow_csm_num_of_cascades来决定

    // PCSS
    lighting_data->light_size_world = ui_config.shadow_pcss_light_size_world; //假定的光源大小，用于软阴影计算
    lighting_data->pcss_enabled     = ui_config.shadow_pcss_enabled ? 1 : 0;

    // BRDF
    {
        lighting_data->lut_ggx_emu_handle  = context.textures.lut_ggx_emu.handle;
        lighting_data->lut_ggx_eavg_handle = context.textures.lut_ggx_eavg.handle;

        lighting_data->brdf_enable_multi_scatter = ui_config.shading_brdf_enable_multi_scatter ? 1 : 0;
        lighting_data->brdf_NDF_mode             = static_cast<uint>(ui_config.shading_brdf_NDF_mode);
        lighting_data->brdf_G_mode               = static_cast<uint>(ui_config.shading_brdf_G_mode);
        lighting_data->brdf_G_is_ibl             = ui_config.shading_brdf_G_is_ibl ? 1 : 0;
    }

    context.cmd_list.CopyFrom(
        std::span<byte>((byte*)lighting_data, sizeof(LightingData)),
        context.lighting_data_buffer.buf->GetView()
    );
}

bool RasterRenderer::RunSingle(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    auto& raster_context = *raster_context_ptr;

    LogSceneLoadStatus(*editor_config);

    // MARK: 1. Tick Window
    auto window_state = TickWindowContext(hooks);

    bool skip_present = false;
    if (window_state == EWindowState::Hiding) {
        std::this_thread::yield(); // FIXME: 这个东西有用吗？
        skip_present = true;

    } else if (window_state == EWindowState::SizeChanged) { // FIXME: Runtime Error
        LOG_INFO("Size Changed.");

        raster_context.FreeFrameBuffers();
        raster_context.CreateFrameBuffers();
        raster_context.UploadExternalFrameBuffers();
        raster_context.AllocateFrameBuffers();

#if WITH_CUDA
        tensor_rt_pass->RecreateResource(
            raster_context,
            raster_context.textures.ao_output_ambient_only.tex,
            raster_context.textures.depth_linear_sampler.tex->CastToTextureRef(),
            // 下面这个color，需要传入lighting_output，而非ao_output，因为模型的输入需要不带ao的color
            raster_context.textures.lighting_output.tex,
            raster_context.textures.camera_motion_vector.tex,
            raster_context.textures.ao_output_ambient_only_1.tex
        );
#endif

        if (hooks.on_raster_register_frame_buffers) {
            hooks.on_raster_register_frame_buffers(raster_context.GetDisplayableFrameBuffersView());
        }

    } else if (window_state == EWindowState::Default) {
        // do nothing

    } else {
        assert(false);
    }

    // MARK: 2. Tick UI
    if (hooks.on_tick_ui) {
        hooks.on_tick_ui();
    }

    if (hooks.on_raster_register_frame_buffers) {
        hooks.on_raster_register_frame_buffers(raster_context.GetDisplayableFrameBuffersView());
    }

    TextureRef default_output_texture = raster_context.textures.output.tex;

    // MARK: 3. Run Render Passes

    if (Scene::IsSceneReady()) {
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

        camera->Tick(editor_config);

        raster_context.Update(camera->GetDeltaTime());

        // others
        RasterTool::UpdateRaytracingScene(raster_context);

        // Shadow Depth Pass
        shadow_depth_pass->Process(raster_context, raster_config, camera);

        // Update Global Lighting Data
        UpdateGlobalLightingData(raster_context, raster_config, camera);

        // Geometry Pass
        geometry_pass->Process(raster_context, raster_config, camera);

        // Directional Shadow Mask Pass
        directional_shadow_mask_pass->Process(raster_context, raster_config, camera);

        // Lighting Pass
        lighting_pass->Process(raster_context, raster_config, camera);

        //Env&Atmo Pass
        skybox_pass->Process(raster_context, raster_config, camera);

        // Post Process Passes
        // - Ambient Occlusion
        auto              ao_result        = ao_pass->Process(raster_context, raster_config, camera, time);
        TextureWithHandle processing_image = ao_result.ao_with_color;
        uint              ao_only_idx      = ao_result.ao_only_idx;

        rtao_denoiser_pass->ProcessInPlace(raster_context, raster_config, ao_only_idx);

        // - CUDA Pass
#if WITH_CUDA
        if (raster_config.ai_is_cuda_enabled) {
            processing_image = tensor_rt_pass->Process(
                raster_context, raster_config, ao_only_idx
            ); //如果开启了该Pass，Ao结果会被替换成TensorRT的结果（在纹理context.textures.lighting_output上执行），后续在该纹理上处理。否则，在纹理ao_with_color上处理
        }
#endif

        // - Denoiser Pass (Bilateral Filter)
        processing_image = bfd_pass->Process(raster_context, raster_config, processing_image);

        // - Screen Space Reflection
        processing_image = ssr_pass->Process(raster_context, raster_config, camera, processing_image);

        // - Anti-aliasing
        processing_image = aa_pass->Process(raster_context, raster_config, camera, processing_image);

#if WITH_CUDA && SUPER_RESOLUTION_ENABLED
        // - Upsample Pass
        processing_image = upsample_pass->Process(raster_context, raster_config, processing_image);
#endif

        //Bloom Pass
        processing_image = bloom_pass->Process(raster_context, raster_config, processing_image);

        // - Tonemapping Pass
        processing_image = tonemapping_pass->Process(raster_context, raster_config, processing_image);

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

        // debug
        if (raster_config.debug_fps_limit_enable) {
            // sleep 1.0 / fps seconds
            // 虽然不精确，但简单
            std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / raster_config.debug_fps_limit));
            LOG_DEBUG("FPS Limit Enabled: {}", raster_config.debug_fps_limit);
        }
    }

    // 因为目前Vulkan的输出信息会聚合后再print，所以我们需要轮询，打印出最后添加的信息
    device.FlushDebugMessages();

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
    if (!skip_present) {
        gfx_queue.Present(swapchain, default_output_texture);
        if (hooks.on_present_windows) {
            hooks.on_present_windows();
        }
    }

    if (hooks.on_is_need_reload && hooks.on_is_need_reload()) {
        return false; // break
    }

    return true;
}

} // namespace Moer::Render::Raster
