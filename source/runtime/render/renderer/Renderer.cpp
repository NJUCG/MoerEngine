#include "renderer/Renderer.h"

// 实现渲染器公共的资源所有权管理、交换链尺寸调整与帧准备性能统计。

// Runtime
#include "config/ConfigManager.h"
#include "misc/Timer.h"
#include "renderer/EditorConfig.h"
#include "rendergraph/RenderGraphResourcePool.h"
#include "rhi/RHI.h"
#include "rhi/RHIExecutor.h"
#include "scene/Scene.h"
#include "shader/ShaderResourceManager.h"
#include "window/WindowContext.h"

#include "common/UiCombinePass.h"

#include <algorithm>

namespace Moer::Render {

Renderer::Renderer(
    uint2                         initial_resolution,
    const SharedPtr<EditorConfig> config,
    SwapchainSurfaceInfo          main_window_surface,
    RenderProfileCapture*         _render_profile_capture
) :
    device(RenderDevice::Get()),
    manager(ShaderManager::Get()),
    gfx_queue(device.GetCommandQueue(EQueueType::Graphics)),
    resolution(initial_resolution),
    render_profile_capture(_render_profile_capture),
    scene(),
    cmd_list() {

    {
        swapchain_create_info = SwapchainCreateInfo{
            .surface          = std::move(main_window_surface),
            .size             = {resolution.x, resolution.y},
            .back_buffer_sz   = 2,
            .preferred_format = PF_R8G8B8A8_SRGB
        };
        swapchain = device.CreateSwapchain(swapchain_create_info);
    }
    {
        bindless_array = device.CreateBindlessArray();
        scene.SetBindlessArray(bindless_array);
        render_scene = MakeUnique<RenderScene>(bindless_array);

        // FIXME: 异步版有bug，会在gfx_queue.Execute()卡死，并且会卡住整台机器一分钟
        // scene.LoadSceneFromFileAsync(_config->scene_path);
        scene.LoadSceneFromFile(config->scene_path);
    }
    // Other vars
    {
        const auto& engine_config = ConfigManager::GetInstance().GetConfig().engine;
        timeline                  = device.CreateFence();
        time                      = 0ull;
        first_load                = true;
        max_frame_in_flight       = engine_config.rhi.max_frame_in_flight;
        if (engine_config.threading.profile_logging) {
            frame_prepare_profile_state = MakeUnique<FramePrepareProfileState>();
        }
    }
    {
        ui_combine_pass = MakeUnique<UiCombinePass>(manager);
    }
}

Renderer::~Renderer() = default;

PresentReceiptRef Renderer::CreateMainPresentReceipt(bool scene_content_ready) {
    if (first_main_present_confirmed.load(std::memory_order_acquire)) {
        return {};
    }

    constexpr auto scene_ready_grace_period = std::chrono::seconds(3);
    const auto     now                      = std::chrono::steady_clock::now();
    if (!scene_content_ready) {
        if (!first_present_candidate_started_at.has_value()) {
            first_present_candidate_started_at = now;
            return {};
        }
        if (now - *first_present_candidate_started_at < scene_ready_grace_period) {
            return {};
        }
    }

    if (!first_present_receipt_logged) {
        first_present_receipt_logged = true;
        if (scene_content_ready) {
            LOG_INFO(
                "[Startup][Renderer] First-present receipt armed: scene_content_ready=true."
            );
        } else {
            LOG_WARNING(
                "[Startup][Renderer] Scene content was not ready within 3 seconds of the first "
                "drawable frame; arming a fallback Present receipt so the editor remains usable."
            );
        }
    }
    return MakeShared<PresentReceipt>();
}

void Renderer::ApplyMainPresentReceipt(const PresentReceiptRef& receipt, const EngineHooks& hooks) {
    assert(IsCurrentlyGameThread());
    if (!receipt || first_main_present_confirmed.load(std::memory_order_acquire)) {
        return;
    }

    const PresentReceiptResult result = receipt->WaitForSubmission();
    if (!result.resolved) {
        LOG_WARNING(
            "[Startup][Renderer] Timed out after 10 seconds while waiting for the main-window "
            "Present receipt; keeping the Splash responsive and retrying on a later frame."
        );
        return;
    }
    if (result.recreate_swapchain) {
        main_swapchain_recreate_requested.store(true, std::memory_order_release);
        LOG_INFO(
            "[Startup][Renderer] Main swapchain requested recreation before the startup handoff."
        );
    }
    if (!result.submitted) {
        return;
    }

    bool expected = false;
    if (first_main_present_confirmed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire
        ) &&
        hooks.on_first_main_present) {
        hooks.on_first_main_present();
    }
}

void Renderer::ReleaseResources() {
    if (resources_released) {
        return;
    }
    resources_released = true;

    timeline->Wait(time);
    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    swapchain->Sync();
    device.WaitIdle();

    render_scene.reset();
    cmd_list.UpdateBindlessArray(bindless_array);
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        cmd_list.Submit().DeleteResources(),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    scene.Reset();
    RenderGraphResourcePool::Global().Reset();
}

WindowFrameSnapshot Renderer::TickWindowContext() {
    WindowContext::Tick();
    return main_window_frame_tracker.Advance(
        swapchain_create_info.surface.GetIdentity(),
        WindowContext::CaptureWindowFrameMetrics(*WindowContext::GetMainWindow())
    );
}

bool Renderer::PrepareRenderFrame(const WindowFrameSnapshot& window_frame) {
    if (time >= max_frame_in_flight) {
        timeline->Wait(time - max_frame_in_flight);
    }
    RenderGraphResourcePool::Global().Tick();

    const bool recreate_requested =
        main_swapchain_recreate_requested.exchange(false, std::memory_order_acq_rel);
    const bool presentation_ready =
        swapchain && swapchain->IsPresentationReady();
    if (!window_frame.IsDrawable()) {
        if (recreate_requested || !presentation_ready) {
            // A zero-extent swapchain cannot be recreated. Preserve the WSI
            // recovery request until the window has a drawable extent again.
            main_swapchain_recreate_requested.store(true, std::memory_order_release);
        }
        return false;
    }
    if (!swapchain) {
        main_swapchain_recreate_requested.store(true, std::memory_order_release);
        return false;
    }

    const Extent2D drawable_extent = window_frame.drawable_extent;
    const bool request_matches =
        presentation_ready &&
        committed_main_surface_identity == window_frame.surface_identity &&
        committed_main_drawable_generation == window_frame.drawable_generation;
    if (request_matches && !recreate_requested) {
        resolution = uint2(swapchain->size.x, swapchain->size.y);
        return true;
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    swapchain_create_info.size = drawable_extent;
    swapchain->Sync();
    const bool recreated = swapchain->Recreate(swapchain_create_info);
    if (!recreated ||
        !swapchain->IsPresentationReady() ||
        swapchain->GetCommittedSurfaceIdentity() != window_frame.surface_identity) {
        main_swapchain_recreate_requested.store(true, std::memory_order_release);
        return false;
    }
    committed_main_surface_identity       = window_frame.surface_identity;
    committed_main_drawable_generation   = window_frame.drawable_generation;
    resolution = uint2(swapchain->size.x, swapchain->size.y);
    return true;
}

void Renderer::LogSceneLoadStatus(const EditorConfig& config) const {
    if (!scene.IsStartLoading()) {
        // 周期性提示场景缺失，既保证问题持续可见，也避免刷屏。
        static LoopedTimer timer(2.0);
        if (timer.Tick()) {
            LOG_WARNING(
                "Don't find scene or scene format isn't supported. Please load a valid scene. Latest "
                "attempted scene: {}",
                config.scene_path
            );
        }
    }
}

FramePrepareProfileClock::time_point Renderer::BeginFramePrepareProfile() const {
    return frame_prepare_profile_state ? FramePrepareProfileClock::now() :
                                         FramePrepareProfileClock::time_point{};
}

void Renderer::CaptureFramePrepareUiWorkload(
    FramePrepareWorkload&    workload,
    const UiDrawFramePacket& ui_draw_frame
) const {
    if (!frame_prepare_profile_state) {
        return;
    }

    const auto accumulate_viewport = [&](const UiViewportDrawPacket& viewport) {
        workload.ui_vertices += viewport.vertices.size();
        workload.ui_indices += viewport.indices.size();
        workload.ui_commands += viewport.commands.size();
    };
    accumulate_viewport(ui_draw_frame.main_viewport);
    for (const auto& viewport : ui_draw_frame.platform_viewports) {
        accumulate_viewport(viewport);
    }
    workload.ui_vertices_max = workload.ui_vertices;
}

void Renderer::RecordFramePrepareProfile(
    std::string_view                     renderer_name,
    FramePrepareProfileClock::time_point started_at,
    const FramePrepareProfile&           profile,
    const FramePrepareWorkload&          workload
) {
    if (!frame_prepare_profile_state) {
        return;
    }

    auto&        state    = *frame_prepare_profile_state;
    const auto   now      = FramePrepareProfileClock::now();
    const double total_ms = std::chrono::duration<double, std::milli>(now - started_at).count();
    if (state.window_start == FramePrepareProfileClock::time_point{}) {
        state.window_start = started_at;
    }
    ++state.samples;
    state.total_ms += total_ms;
    state.total_max_ms = std::max(state.total_max_ms, total_ms);
    state.other_ms += std::max(0.0, total_ms - profile.MeasuredTotalMilliseconds());
    state.scene_update_max_ms   = std::max(state.scene_update_max_ms, profile.scene_update_ms);
    state.scene_snapshot_max_ms = std::max(state.scene_snapshot_max_ms, profile.scene_snapshot_ms);
    state.ui_draw_packet_max_ms = std::max(state.ui_draw_packet_max_ms, profile.ui_draw_packet_ms);
    state.accumulated.Accumulate(profile);
    state.workload.Accumulate(workload);

    const double window_ms = std::chrono::duration<double, std::milli>(now - state.window_start).count();
    if (window_ms < 1000.0) {
        return;
    }

    const double inverse_samples = 1.0 / double(state.samples);
    LOG_INFO(
        "[ThreadingProfile][Prepare] renderer={} window_ms={:.3f} samples={} "
        "total_avg_ms={:.3f} total_max_ms={:.3f} window_avg_ms={:.3f} "
        "scripting_avg_ms={:.3f} test_avg_ms={:.3f} ui_tick_avg_ms={:.3f} "
        "camera_and_test_avg_ms={:.3f} config_snapshot_avg_ms={:.3f} "
        "scene_update_avg_ms={:.3f} scene_update_max_ms={:.3f} "
        "scene_snapshot_avg_ms={:.3f} scene_snapshot_max_ms={:.3f} "
        "ui_composition_avg_ms={:.3f} ui_draw_packet_avg_ms={:.3f} "
        "ui_draw_packet_max_ms={:.3f} other_avg_ms={:.3f} "
        "scene_ready_frames={} scene_dirty_frames={} initial_gpu_update_frames={} "
        "update_gpu_update_frames={} geometry_snapshot_frames={} "
        "scene_snapshot_build_frames={} ui_vertices_avg={:.1f} ui_indices_avg={:.1f} "
        "ui_commands_avg={:.1f} ui_vertices_max={}",
        renderer_name,
        window_ms,
        state.samples,
        state.total_ms * inverse_samples,
        state.total_max_ms,
        state.accumulated.window_ms * inverse_samples,
        state.accumulated.scripting_ms * inverse_samples,
        state.accumulated.test_ms * inverse_samples,
        state.accumulated.ui_tick_ms * inverse_samples,
        state.accumulated.camera_and_test_ms * inverse_samples,
        state.accumulated.config_snapshot_ms * inverse_samples,
        state.accumulated.scene_update_ms * inverse_samples,
        state.scene_update_max_ms,
        state.accumulated.scene_snapshot_ms * inverse_samples,
        state.scene_snapshot_max_ms,
        state.accumulated.ui_composition_ms * inverse_samples,
        state.accumulated.ui_draw_packet_ms * inverse_samples,
        state.ui_draw_packet_max_ms,
        state.other_ms * inverse_samples,
        state.workload.scene_ready_frames,
        state.workload.scene_dirty_frames,
        state.workload.initial_gpu_update_frames,
        state.workload.update_gpu_update_frames,
        state.workload.geometry_snapshot_frames,
        state.workload.scene_snapshot_build_frames,
        double(state.workload.ui_vertices) * inverse_samples,
        double(state.workload.ui_indices) * inverse_samples,
        double(state.workload.ui_commands) * inverse_samples,
        state.workload.ui_vertices_max
    );

    state              = {};
    state.window_start = now;
}

} // namespace Moer::Render
