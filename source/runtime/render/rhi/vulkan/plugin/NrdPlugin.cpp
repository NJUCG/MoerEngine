#include "rhi/plugin/NrdPlugin.h"

#if WITH_NRD

namespace Moer::Render::Ext {

NRDInterface::~NRDInterface() {
    for (auto& desc_map : texture_barrier_descs) {
        for (auto& [_, desc] : desc_map) {
            nrd.nri.rhi.DestroyTexture(*desc.texture);
        }
    }

    for (auto& [_, cmd_list] : cmd_lists_on_use) {
        nrd.nri.rhi.DestroyCommandBuffer(*cmd_list);
    }

    nrd.integration.Destroy();

    nri::nriDestroyDevice(*nrd.nri.device);
}

void NRDInterface::UpdateCommonSettings(
    uint32            _frame_index,
    const Vector2ui&  _size,
    const Vector2f&   _jitter,
    const Matrix4x4f& _view,
    const Matrix4x4f& _proj
) {
    nrd_common_settings.frameIndex = _frame_index;

    nrd_common_settings.motionVectorScale[0] = 1.f / _size.x;
    nrd_common_settings.motionVectorScale[1] = 1.f / _size.y;

    nrd_common_settings.cameraJitterPrev[0] = nrd_common_settings.cameraJitter[0];
    nrd_common_settings.cameraJitterPrev[1] = nrd_common_settings.cameraJitter[1];
    nrd_common_settings.cameraJitter[0]     = _jitter.x;
    nrd_common_settings.cameraJitter[1]     = _jitter.y;

    nrd_common_settings.resourceSizePrev[0] = nrd_common_settings.resourceSize[0];
    nrd_common_settings.resourceSizePrev[1] = nrd_common_settings.resourceSize[1];
    nrd_common_settings.resourceSize[0]     = _size.x;
    nrd_common_settings.resourceSize[1]     = _size.y;

    nrd_common_settings.rectSizePrev[0] = nrd_common_settings.rectSize[0];
    nrd_common_settings.rectSizePrev[1] = nrd_common_settings.rectSize[1];
    nrd_common_settings.rectSize[0]     = _size.x;
    nrd_common_settings.rectSize[1]     = _size.y;

    nrd_common_settings.rectOrigin[0] = 0;
    nrd_common_settings.rectOrigin[1] = 0;

    // Set common settings
    memcpy(
        nrd_common_settings.worldToViewMatrixPrev,
        nrd_common_settings.worldToViewMatrix,
        sizeof(nrd_common_settings.worldToViewMatrix)
    );
    memcpy(
        nrd_common_settings.viewToClipMatrixPrev,
        nrd_common_settings.viewToClipMatrix,
        sizeof(nrd_common_settings.viewToClipMatrix)
    );
    memcpy(nrd_common_settings.worldToViewMatrix, &_view, sizeof(nrd_common_settings.worldToViewMatrix));
    memcpy(nrd_common_settings.viewToClipMatrix, &_proj, sizeof(nrd_common_settings.viewToClipMatrix));
    nrd.integration.SetCommonSettings(nrd_common_settings);
}

void NRDInterface::SetCommonSettings(const nrd::CommonSettings& _settings) {
    nrd.integration.SetCommonSettings(_settings);
}

void NRDInterface::SetDenoiserSettings(const nrd::Denoiser _type, const nrd::ReblurSettings& _settings) {
    assert(_type < nrd::Denoiser::RELAX_DIFFUSE && "Denoiser type is not reblur");
    nrd.integration.SetDenoiserSettings(nrd::Identifier(_type), &_settings);
}

void NRDInterface::SetDenoiserSettings(const nrd::Denoiser _type, const nrd::RelaxSettings& _settings) {
    assert(
        _type >= nrd::Denoiser::RELAX_DIFFUSE && _type < nrd::Denoiser::SIGMA_SHADOW &&
        "Denoiser type is not relax"
    );
    nrd.integration.SetDenoiserSettings(nrd::Identifier(_type), &_settings);
}

void NRDInterface::SetDefaultDenoiserSettings(const nrd::Identifier _denoiser_id) {
    // import from RTXDI
    if (_denoiser_id < nrd::Identifier(nrd::Denoiser::RELAX_DIFFUSE)) {
        auto reblur_settings                      = nrd::ReblurSettings();
        reblur_settings.enableAntiFirefly         = true;
        reblur_settings.diffusePrepassBlurRadius  = 30.0f;
        reblur_settings.specularPrepassBlurRadius = 30.0f;
        nrd.integration.SetDenoiserSettings(_denoiser_id, &reblur_settings);
    } else if (_denoiser_id < nrd::Identifier(nrd::Denoiser::SIGMA_SHADOW)) {
        auto relax_settings                                      = nrd::RelaxSettings();
        relax_settings.diffuseMaxFastAccumulatedFrameNum         = 1;
        relax_settings.specularMaxFastAccumulatedFrameNum        = 1;
        relax_settings.diffusePhiLuminance                       = 1.0f;
        relax_settings.spatialVarianceEstimationHistoryThreshold = 1;
        relax_settings.enableAntiFirefly                         = true;
        relax_settings.diffusePrepassBlurRadius                  = 30.0f;
        relax_settings.specularPrepassBlurRadius                 = 30.0f;
        nrd.integration.SetDenoiserSettings(_denoiser_id, &relax_settings);
    } else {
        // not implemented
    }
}

void NRDInterface::SetDefaultCommonSettings(uint16 _frame_width, uint16 _frame_height) {
    nrd_common_settings = nrd::CommonSettings();

    nrd_common_settings.resourceSizePrev[0] = _frame_width;
    nrd_common_settings.resourceSizePrev[1] = _frame_height;
    nrd_common_settings.resourceSize[0]     = _frame_width;
    nrd_common_settings.resourceSize[1]     = _frame_height;

    nrd_common_settings.rectSizePrev[0] = _frame_width;
    nrd_common_settings.rectSizePrev[1] = _frame_height;
    nrd_common_settings.rectSize[0]     = _frame_width;
    nrd_common_settings.rectSize[1]     = _frame_height;

    nrd_common_settings.rectOrigin[0] = 0;
    nrd_common_settings.rectOrigin[1] = 0;
    // import from RTXDI
    nrd_common_settings.rectOrigin[0]                       = 0;
    nrd_common_settings.rectOrigin[1]                       = 0;
    nrd_common_settings.timeDeltaBetweenFrames              = 0.0f;
    nrd_common_settings.denoisingRange                      = 50000.0f;
    nrd_common_settings.disocclusionThreshold               = 0.01f;
    nrd_common_settings.disocclusionThresholdAlternate      = 0.05f;
    nrd_common_settings.splitScreen                         = 0.0f;
    nrd_common_settings.debug                               = 0.0f;
    nrd_common_settings.frameIndex                          = 0;
    nrd_common_settings.accumulationMode                    = nrd::AccumulationMode::CONTINUE;
    nrd_common_settings.isMotionVectorInWorldSpace          = false;
    nrd_common_settings.isHistoryConfidenceAvailable        = false;
    nrd_common_settings.isDisocclusionThresholdMixAvailable = false;
    nrd_common_settings.isBaseColorMetalnessAvailable       = false;
    nrd_common_settings.enableValidation                    = false;
}

}; // namespace Moer::Render::Ext

#endif // WITH_NRD