// 负责光栅帧准备、Pass 执行，以及游戏线程与渲染线程之间的反馈。
#pragma once

#include "RasterFramePacket.h"
#include "renderer/Renderer.h"
#include "rhi/RHIResource.h"

#include <string>
#include <unordered_set>

namespace Moer::Render::Raster {

class RasterContext;
class HiZBuildPass;
class ShadowDepthPass;
class GeometryPass;
class TessellatedSurfacePass;
class CsmGizmoPass;
class DirectionalShadowMaskPass;
class ProbeUpdatePass;
class ProbeGizmoPass;
class CameraGizmoPass;
class LightingPass;
class SkyboxPass;
class AoPass;
class RtaoDenoiserPass;
class BilateralFilterDenoiserPass;
class SsrPass;
class CooperativeOpsPass;
class AaPass;
class BloomPass;
class TonemappingPass;

#if WITH_CUDA
class CudaPass;
class TensorRTPass;
#endif

/**
 * 持有 Raster Pass，并协调游戏线程与渲染线程的帧边界。
 * 帧准备阶段捕获可变的 Editor/Scene 状态，RenderFrame 使用该快照。
 */
class RENDER_API RasterRenderer : public Renderer {

public:
    RasterRenderer(
        uint2                         initial_resolution,
        SharedPtr<EditorConfig>       config,
        RenderProfileCapture*         render_profile_capture
    );

    ~RasterRenderer() override;

    void Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) override;

    bool SupportsSynchronizedRenderThread() const override {
        return true;
    }

    RasterFramePacket
    PrepareFrame(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks);
    RasterFrameFeedback RenderFrame(RasterFramePacket frame_packet);
    void ApplyFrameFeedback(
        RasterFrameFeedback feedback,
        RasterConfig&       target_config,
        const EngineHooks&  hooks
    );

    bool RunSingle(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks);

    void
    UpdateGlobalLightingData(RasterContext& context, const RasterConfig& ui_config, const Camera& camera);

private:
    // 使用堆所有权，使公共头文件中只需前置声明 RasterContext。
    UniquePtr<RasterContext> raster_context_ptr;

    // 按执行顺序排列的 Raster Pass。
    UniquePtr<HiZBuildPass>                hiz_build_pass;
    UniquePtr<ShadowDepthPass>             shadow_depth_pass;
    UniquePtr<DirectionalShadowMaskPass>   directional_shadow_mask_pass;
    UniquePtr<ProbeUpdatePass>             probe_update_pass;
    UniquePtr<CsmGizmoPass>                csm_gizmo_pass;
    UniquePtr<ProbeGizmoPass>              probe_gizmo_pass;
    UniquePtr<CameraGizmoPass>             camera_gizmo_pass;
    UniquePtr<GeometryPass>                geometry_pass;
    UniquePtr<TessellatedSurfacePass>      tessellated_surface_pass;
    UniquePtr<LightingPass>                lighting_pass;
    UniquePtr<SkyboxPass>                  skybox_pass;
    UniquePtr<AoPass>                      ao_pass;
    UniquePtr<RtaoDenoiserPass>            rtao_denoiser_pass;
    UniquePtr<BilateralFilterDenoiserPass> bilateral_filter_denoiser_pass;
    UniquePtr<SsrPass>                     ssr_pass;
    UniquePtr<CooperativeOpsPass>          cooperative_ops_pass;
    UniquePtr<AaPass>                      aa_pass;
    UniquePtr<BloomPass>                   bloom_pass;
    UniquePtr<TonemappingPass>             tonemapping_pass;

#if WITH_CUDA
    UniquePtr<CudaPass>     cuda_pass;
    UniquePtr<TensorRTPass> tensor_rt_pass;
#endif

    Camera scene_view_camera;
    bool   scene_view_camera_initialized = false;
    bool   capture_scene_geometry_snapshot = true;

    uint64_t next_frame_id = 0;
    uint64_t render_extent_generation = 0;

    SceneRenderExtentTracker scene_render_extent_tracker;

    bool                            render_graph_enabled = false;
    bool                            render_graph_debug_dump = false;
    bool                            parallel_recording_enabled = false;
    bool                            render_graph_fallback_latched = false;
    std::unordered_set<std::string> logged_render_graph_dumps;
};

} // namespace Moer::Render::Raster
