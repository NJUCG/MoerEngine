#pragma once

#include "renderer/Renderer.h"
#include "rhi/RHIResource.h"

namespace Moer::Render::Raster {

class RasterContext;
class ShadowDepthPass;
class GeometryPass;
class DirectionalShadowMaskPass;
class LightingPass;
class SkyboxPass;
class AoPass;
class RtaoDenoiserPass;
class BilateralFilterDenoiserPass;
class SsrPass;
class AaPass;
class BloomPass;
class TonemappingPass;

#if WITH_CUDA
class CudaPass;
class TensorRTPass;
class UpsamplePass;
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
    RasterRenderer(
        uint2&                                                    _resolution,
        const SharedPtr<EditorConfig>                             _config,
        const EngineHooks&                                        _hooks,
        std::function<void(const std::filesystem::path&, Scene*)> _load_scene_async
    );

    virtual ~RasterRenderer() override;

    virtual void Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) override;

    bool RunSingle(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks);

    void
    UpdateGlobalLightingData(RasterContext& context, const RasterConfig& ui_config, const CameraRef& camera);

private:
    // Context
    UniquePtr<RasterContext> raster_context_ptr; // For forward declaration

    // Pass
    UniquePtr<ShadowDepthPass>             shadow_depth_pass;
    UniquePtr<DirectionalShadowMaskPass>   directional_shadow_mask_pass;
    UniquePtr<GeometryPass>                geometry_pass;
    UniquePtr<LightingPass>                lighting_pass;
    UniquePtr<SkyboxPass>                  skybox_pass;
    UniquePtr<AoPass>                      ao_pass;
    UniquePtr<RtaoDenoiserPass>            rtao_denoiser_pass;
    UniquePtr<BilateralFilterDenoiserPass> bfd_pass;
    UniquePtr<SsrPass>                     ssr_pass;
    UniquePtr<AaPass>                      aa_pass;
    UniquePtr<BloomPass>                   bloom_pass;
    UniquePtr<TonemappingPass>             tonemapping_pass;

#if WITH_CUDA
    UniquePtr<CudaPass>     cuda_pass;
    UniquePtr<TensorRTPass> tensor_rt_pass;
    UniquePtr<UpsamplePass> upsample_pass;
#endif

    // Other vars
    Array<RaytracingGeometryRef> rt_geometries;
}; // namespace Moer::Render::Raster

} // namespace Moer::Render::Raster