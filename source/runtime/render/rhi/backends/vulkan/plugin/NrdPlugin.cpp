#include "rhi/plugin/NrdPlugin.h"

#if WITH_NRD

#include <cstring>

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

bool NRDInterface::PreparedFrame::IsValid() const {
    const auto has = [&](EResourceSlot slot) {
        return static_cast<bool>(resources[uint8(slot)]);
    };
    if (denoiser == nrd::Denoiser::MAX_NUM ||
        !has(EResourceSlot::MOTION_VECTOR) ||
        !has(EResourceSlot::NORMAL_ROUGHNESS) ||
        !has(EResourceSlot::VIEW_Z)) {
        return false;
    }
    switch (denoiser) {
        case nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR:
        case nrd::Denoiser::RELAX_DIFFUSE_SPECULAR:
            return has(EResourceSlot::IN_DIFFUSE) &&
                   has(EResourceSlot::IN_SPECULAR) &&
                   has(EResourceSlot::OUT_DIFFUSE) &&
                   has(EResourceSlot::OUT_SPECULAR);
        case nrd::Denoiser::REBLUR_DIFFUSE:
        case nrd::Denoiser::RELAX_DIFFUSE:
            return has(EResourceSlot::IN_DIFFUSE) &&
                   has(EResourceSlot::OUT_DIFFUSE);
        case nrd::Denoiser::REBLUR_SPECULAR:
        case nrd::Denoiser::RELAX_SPECULAR:
            return has(EResourceSlot::IN_SPECULAR) &&
                   has(EResourceSlot::OUT_SPECULAR);
        default:
            return false;
    }
}

NRDInterface::PreparedFrameRef
NRDInterface::PrepareFrame(FrameDesc _desc) const {
    if (_desc.size.x == 0 || _desc.size.y == 0) {
        return {};
    }

    auto prepared = MakeShared<PreparedFrame>();
    prepared->base_revision  = accepted_revision;
    prepared->denoiser       = _desc.denoiser;
    prepared->resources      = std::move(_desc.resources);
    prepared->common_settings = nrd_common_settings;
    prepared->generation_token = generation_token;

    auto& settings = prepared->common_settings;
    settings.frameIndex = _desc.frame_index;
    settings.motionVectorScale[0] = 1.f / _desc.size.x;
    settings.motionVectorScale[1] = 1.f / _desc.size.y;

    settings.cameraJitterPrev[0] = settings.cameraJitter[0];
    settings.cameraJitterPrev[1] = settings.cameraJitter[1];
    settings.cameraJitter[0]     = _desc.jitter.x;
    settings.cameraJitter[1]     = _desc.jitter.y;

    settings.resourceSizePrev[0] = settings.resourceSize[0];
    settings.resourceSizePrev[1] = settings.resourceSize[1];
    settings.resourceSize[0]     = _desc.size.x;
    settings.resourceSize[1]     = _desc.size.y;

    settings.rectSizePrev[0] = settings.rectSize[0];
    settings.rectSizePrev[1] = settings.rectSize[1];
    settings.rectSize[0]     = _desc.size.x;
    settings.rectSize[1]     = _desc.size.y;
    settings.rectOrigin[0]   = 0;
    settings.rectOrigin[1]   = 0;

    std::memcpy(
        settings.worldToViewMatrixPrev,
        settings.worldToViewMatrix,
        sizeof(settings.worldToViewMatrix)
    );
    std::memcpy(
        settings.viewToClipMatrixPrev,
        settings.viewToClipMatrix,
        sizeof(settings.viewToClipMatrix)
    );
    std::memcpy(
        settings.worldToViewMatrix,
        &_desc.view,
        sizeof(settings.worldToViewMatrix)
    );
    std::memcpy(
        settings.viewToClipMatrix,
        &_desc.projection,
        sizeof(settings.viewToClipMatrix)
    );
    settings.accumulationMode =
        accepted_history_valid && accepted_denoiser == _desc.denoiser ?
            nrd::AccumulationMode::CONTINUE :
            nrd::AccumulationMode::CLEAR_AND_RESTART;

    return prepared->IsValid() ? prepared : PreparedFrameRef{};
}

bool NRDInterface::CommitFrame(const PreparedFrameRef& _frame) {
    if (!OwnsPreparedFrame(_frame) || !_frame->IsValid() ||
        _frame->base_revision != accepted_revision) {
        return false;
    }
    nrd_common_settings     = _frame->common_settings;
    accepted_denoiser      = _frame->denoiser;
    accepted_history_valid = true;
    ++accepted_revision;
    return true;
}

void NRDInterface::ResetAcceptedHistory() {
    SetDefaultCommonSettings(nrd.frame_width, nrd.frame_height);
    accepted_history_valid = false;
    accepted_denoiser      = nrd::Denoiser::MAX_NUM;
    ++accepted_revision;
}

void NRDInterface::SetDefaultDenoiserSettings(const nrd::Identifier _denoiser_id) {
    // import from RTXDI
    const auto denoiser = static_cast<nrd::Denoiser>(_denoiser_id);
    if (IsReblurDenoiser(denoiser)) {
        auto reblur_settings                      = nrd::ReblurSettings();
        reblur_settings.enableAntiFirefly         = true;
        reblur_settings.diffusePrepassBlurRadius  = 30.0f;
        reblur_settings.specularPrepassBlurRadius = 30.0f;
        nrd.integration.SetDenoiserSettings(_denoiser_id, &reblur_settings);
    } else if (IsRelaxDenoiser(denoiser)) {
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
