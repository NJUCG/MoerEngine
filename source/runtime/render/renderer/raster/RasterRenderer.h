#pragma once

#include "RasterFramePacket.h"
#include "renderer/Renderer.h"
#include "rhi/RHIResource.h"

#include <functional>
#include <optional>

namespace Moer::Render::Raster {

class RasterContext;
class HiZBuildPass;
class ShadowDepthPass;
class GeometryPass;
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
 * Raster渲染方法TODO Lists
 * 
 * TODO: 着色，LightingPass，目前还比较初步
 * TODO: IBL [Done by wk]
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
class RENDER_API RasterRenderer : public Renderer {

public:
    RasterRenderer(uint2 _resolution, const SharedPtr<EditorConfig> _config, const EngineHooks& _hooks);

    virtual ~RasterRenderer() override;

    virtual void Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) override;

    bool SupportsSynchronizedRenderThread() const override {
        return true;
    }

    RasterFramePacket
    PrepareFrame(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks);
    void RenderFrame(RasterFramePacket frame_packet);
    void ApplyFrameFeedback(RasterConfig& target_config);

    bool RunSingle(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks);

    void
    UpdateGlobalLightingData(RasterContext& context, const RasterConfig& ui_config, const Camera& camera);

private:
    // Context
    UniquePtr<RasterContext> raster_context_ptr; // For forward declaration

    // Pass
    UniquePtr<HiZBuildPass>                hiz_build_pass;
    UniquePtr<ShadowDepthPass>             shadow_depth_pass;
    UniquePtr<DirectionalShadowMaskPass>   directional_shadow_mask_pass;
    UniquePtr<ProbeUpdatePass>             probe_update_pass;
    UniquePtr<CsmGizmoPass>                csm_gizmo_pass;
    UniquePtr<ProbeGizmoPass>              probe_gizmo_pass;
    UniquePtr<CameraGizmoPass>             camera_gizmo_pass;
    UniquePtr<GeometryPass>                geometry_pass;
    UniquePtr<LightingPass>                lighting_pass;
    UniquePtr<SkyboxPass>                  skybox_pass;
    UniquePtr<AoPass>                      ao_pass;
    UniquePtr<RtaoDenoiserPass>            rtao_denoiser_pass;
    UniquePtr<BilateralFilterDenoiserPass> bfd_pass;
    UniquePtr<SsrPass>                     ssr_pass;
    UniquePtr<CooperativeOpsPass>          cooperative_ops_pass;
    UniquePtr<AaPass>                      aa_pass;
    UniquePtr<BloomPass>                   bloom_pass;
    UniquePtr<TonemappingPass>             tonemapping_pass;

#if WITH_CUDA
    UniquePtr<CudaPass>     cuda_pass;
    UniquePtr<TensorRTPass> tensor_rt_pass;
#endif

    // Other vars
    // TODO: rt_geometries 已迁移到 GpuScene，未来应移除
    Camera m_scene_view_camera;
    bool   m_b_scene_view_camera_initialized = false;
    bool   m_capture_scene_geometry_snapshot = true;

    std::optional<RasterFrameFeedback> m_latest_frame_feedback;
    uint64_t                           m_next_frame_id = 0;
}; // namespace Moer::Render::Raster

} // namespace Moer::Render::Raster
