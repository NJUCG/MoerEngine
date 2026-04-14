#include "RasterRenderer.h"

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
#include "scene/LogicalComponents.h"
#include "trace/Trace.h"
#include "window/WindowContext.h"

#if WITH_CUDA
#include "CudaPass.h"
#include "TensorRTPass.h"
#endif

namespace Moer::Render::Raster {

RasterRenderer::RasterRenderer(
    uint2&                        _resolution,
    const SharedPtr<EditorConfig> _config,
    const EngineHooks&            _hooks
) :
    // Super
    Renderer(_resolution, _config, _hooks) {

    raster_context_ptr =
        MakeUnique<RasterContext>(device, manager, gfx_queue, bindless_array, cmd_list, scene, resolution);
    auto& raster_context = *raster_context_ptr;

    raster_context.CreateFrameBuffers();
    raster_context.UploadExternalFrameBuffers();
    raster_context.AllocateFrameBuffers();

    {
        Array<CommandList> init_cmd_lists{};
        init_cmd_lists.emplace_back(std::move(cmd_list));
        RHIExecutor::Get().Submit(std::move(init_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
        cmd_list = CommandList(EQueueType::Graphics);
    }

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
#endif

    cmd_list.UpdateBindlessArray(bindless_array);
    {
        Array<CommandList> init_cmd_lists{};
        init_cmd_lists.emplace_back(std::move(cmd_list));
        RHIExecutor::Get().Submit(std::move(init_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
        cmd_list = CommandList(EQueueType::Graphics);
    }

    // Other vars
}

RasterRenderer::~RasterRenderer() {
    auto& raster_context = *raster_context_ptr;
    raster_context.FreeFrameBuffers(true);

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
    const Camera&       camera
) {
    uint          csm_layers    = ui_config.shadow_csm_num_of_cascades;
    LightingData* lighting_data = &context.lighting_data;

    lighting_data->clip2world      = Transpose(camera.GetViewProjectionMatrixInv());
    lighting_data->light_count     = context.scene.cpu_scene().GetLightCount();
    lighting_data->camera_position = camera.GetPosition();

    // Shadow Parameters
    lighting_data->shadow_map_mode              = static_cast<int>(ui_config.shadow_map_mode);
    lighting_data->shadow_sampling_mode         = ui_config.shadow_sampling_mode;
    lighting_data->shadow_csm_num_of_cascades   = csm_layers;
    lighting_data->shadow_csm_sm_size           = ui_config.shadow_csm_sm_size;
    lighting_data->shadow_csm_visualize_cascade = ui_config.shadow_csm_visualize_cascade;

    // Shadow Map
    for (uint i = 0; i < csm_layers; i++) {
        lighting_data->cascade_shadow_map[i] = context.csm_data.shadow_map_textures[i].hdl;
    }
    lighting_data->point_shadow_map = context.point_shadow_data.shadow_cubes[0].handle;
    lighting_data->light_pos        = context.point_shadow_data.shadow_cubes[0].light_pos;
    lighting_data->light_radius     = context.point_shadow_data.shadow_cubes[0].far_plane;

    // Shadow Transform
    for (uint i = 0; i < csm_layers; i++) {
        lighting_data->world2shadow_clip[i] = Transpose(lighting_data->world2shadow_clip[i]);
    }
    lighting_data->world2view = Transpose(camera.GetViewMatrix());
    lighting_data->near_clip  = camera.GetNearClip();
    lighting_data->far_clip   = camera.GetFarClip();

    lighting_data->is_csm_blend_enabled = ui_config.shadow_csm_blend_option ? 1 : 0;
    // 注：此处不一定使用所有CSM，Shader中具体根据shadow_csm_num_of_cascades来决定

    // PCSS
    lighting_data->light_size_world = ui_config.shadow_pcss_light_size_world; //假定的光源大小，用于软阴影计算
    lighting_data->pcss_enabled     = ui_config.shadow_pcss_enabled ? 1 : 0;

    // BRDF
    {
        lighting_data->lut_ggx_emu_handle  = context.textures.lut_ggx_emu.hdl;
        lighting_data->lut_ggx_eavg_handle = context.textures.lut_ggx_eavg.hdl;

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
    TRACE_SCOPE_CAT("Raster.Frame", "Frame");
    auto& raster_context = *raster_context_ptr;

    LogSceneLoadStatus(*editor_config);

    // MARK: 1. Tick Window
    auto window_state = TickWindowContext(hooks);

    bool skip_present = false;
    if (window_state == EWindowState::Hiding) {
        std::this_thread::yield(); // FIXME: 这个东西有用吗？
        skip_present = true;

    } else if (window_state == EWindowState::SizeChanged) {
        LOG_INFO("Size Changed.");

        raster_context.FreeFrameBuffers(false);
        raster_context.CreateFrameBuffers();
        // raster_context.UploadExternalFrameBuffers(); // 窗口大小变化时，外部纹理不会变化，所以不需要上传
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

    if (scene.IsReady()) {

        // 处理场景加载过程中遗留的命令
        // The gfx CommandList contains a CopyScope for all CPU→GPU uploads.
        // The executor splits the stream at scope boundaries, routes the enclosed
        // commands to the copy queue, and auto-generates acquire/release barriers.
        auto&& scene_cmd_list = scene.PopPendingCommandList();
        if (!scene_cmd_list.gfx_queue_cmd_list.IsEmpty()) {
            Array<CommandList> scene_cmd_lists{};
            scene_cmd_lists.emplace_back(std::move(scene_cmd_list.gfx_queue_cmd_list));
            RHIExecutor::Get().Submit(std::move(scene_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
        }

        if (first_load) {
            first_load = false;

            // 随手加一句，避免出错（重构完毕后可以尝试去除）
            cmd_list.UpdateBindlessArray(bindless_array);

            Array<CommandList> first_load_cmd_lists{};
            first_load_cmd_lists.emplace_back(std::move(cmd_list));
            RHIExecutor::Get().Submit(std::move(first_load_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
            cmd_list = CommandList(EQueueType::Graphics);
        }

        auto& raster_config = editor_config->raster_config;
        auto& camera        = scene.GetMainCamera().camera;

        {
            // Jitter Camera for SMAA T2x
            static uint8_t smaa_current_frame_index = 0;
            if (raster_config.aa_mode == EAaMode::SMAA_T2X) {

                smaa_current_frame_index ^= 1;
                static StaticArray<float2, 2> smaa_jitter = {float2(0.25f, -0.25f), float2(-0.25f, 0.25f)};
                camera.SetJitterMatrix(smaa_jitter[smaa_current_frame_index]);
            }
        }

        camera.Tick(editor_config);

        raster_context.Update(camera.GetDeltaTime());

        // others
        // FIXME: 统一update scene
        // scene.GetGpuScene().UpdateRaytracingScene(cmd_list);

        // 当启用视锥剔除时，先恢复完整的 draw commands，
        // 确保 ShadowDepthPass 使用未被上一帧剔除的完整场景数据
        if (raster_config.enable_frustum_culling) {
            raster_context.scene.RestoreDrawCommands(raster_context.cmd_list);
        }

        // Shadow Depth Pass
        cmd_list.PushScopeWithTimeScope(RasterTool::GetShadowDepthPassProfileScopeName());
        shadow_depth_pass->Process(raster_context, raster_config, camera);
        cmd_list.PopScopeWithTimeScope();

        // Update Global Lighting Data
        UpdateGlobalLightingData(raster_context, raster_config, camera);

        // Geometry Pass
        cmd_list.PushScopeWithTimeScope(RasterTool::GetGeometryPassProfileScopeName());
        geometry_pass->Process(raster_context, raster_config, camera);
        cmd_list.PopScopeWithTimeScope();

        // Directional Shadow Mask Pass
        directional_shadow_mask_pass->Process(raster_context, raster_config, camera);

        // Lighting Pass
        lighting_pass->Process(raster_context, raster_config, camera);

        //Env&Atmo Pass
        skybox_pass->Process(raster_context, raster_config, camera);

        // Post Process Passes
        // - Ambient Occlusion
        auto ao_result = ao_pass->Process(raster_context, raster_config, camera, time);

        rtao_denoiser_pass->ProcessInPlace(raster_context, raster_config, ao_result.ao_only_idx);
        ao_pass->CompositeAo(raster_context, raster_config, ao_result.ao_only);

        TextureWithHandle processing_image = raster_context.textures.ao_output;

        // - CUDA Pass
#if WITH_CUDA
        if (raster_config.ai_is_cuda_enabled) {
            processing_image = tensor_rt_pass->Process(raster_context, raster_config, ao_result.ao_only_idx);
        }
#endif

        // - Denoiser Pass (Bilateral Filter)
        processing_image = bfd_pass->Process(raster_context, raster_config, processing_image);

        // - Screen Space Reflection
        processing_image = ssr_pass->Process(raster_context, raster_config, camera, processing_image);

        // - Anti-aliasing
        processing_image = aa_pass->Process(raster_context, raster_config, camera, processing_image);

        // - Bloom Pass
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
        // FIXME: 统一advance frame
        // scene.GetRaytracingScene()->AdvanceFrame();

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
    RHIPresentRequest present_request{swapchain, default_output_texture};
    cmd_list.Signal(timeline, time).DeleteResources().TickProfiling();
    Array<CommandList> frame_cmd_lists{};
    frame_cmd_lists.emplace_back(std::move(cmd_list));
    RHIExecutor::Get().Submit(
        std::move(frame_cmd_lists),
        ERHIExecSubmitFlags::FlushGPU | ERHIExecSubmitFlags::FrameEnd,
        skip_present ? nullptr : &present_request
    );
    cmd_list = CommandList(EQueueType::Graphics);

    if (!skip_present && hooks.on_present_windows) {
        hooks.on_present_windows();
    }

    if (hooks.on_is_need_reload && hooks.on_is_need_reload()) {
        return false; // break
    }

    return true;
}

} // namespace Moer::Render::Raster
