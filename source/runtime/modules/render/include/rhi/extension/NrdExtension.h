#ifndef MOER_NRD_EXTENSION_H
#define MOER_NRD_EXTENSION_H

#include "rhi/RHI.h"

// NRD and NRI-based integration
#include <NRI.h>
#include <Extensions/NRIHelper.h>
#include <Extensions/NRIWrapperVK.h>
#include <NRD.h>
#include <NRDIntegration.h>

namespace Moer::Render::Ext {

    class NRDInterface {
    public:
        NRDInterface() {
            SetDefaultCommonSettings();
            SetDefaultDenoiserSettings(nrd::Identifier(type));
        }

        virtual ~NRDInterface() {
            for (auto& desc : texture_barrier_descs) {
                nrd.nri.rhi.DestroyTexture(*desc.texture);
            }

            nrd.nri.rhi.DestroyCommandBuffer(*nrd.nri.cmd_list);

            nrd.integration.Destroy();

            nri::nriDestroyDevice(*nrd.nri.device);
        }

        virtual void Begin() = 0;

        virtual void Denoise(CommandList& _cmd_list) = 0;

    private:
        nrd::CommonSettings                                   nrd_common_settings   = {};
        std::variant<nrd::ReblurSettings, nrd::RelaxSettings> nrd_denoiser_settings = {};

    public:
        enum struct EInResource : uint8 {
            MOTION_VECTOR,
            NORMAL_ROUGHNESS,
            VIEW_Z,
            BASECOLOR_METALNESS,
            IN_DIFFUSE,
            IN_SPECULAR,

            INPUT_NUM
        };

        enum struct EOutResource : uint8 {
            OUT_DIFFUSE,
            OUT_SPECULAR,

            OUTPUT_NUM
        };

    public:
        void UseDenoiser(const nrd::Denoiser _denoiser) {
            type = _denoiser;
            SetDefaultDenoiserSettings(nrd::Identifier(_denoiser));
        }

        virtual void SetInput(EInResource _index, TextureRef _texture) = 0;

        virtual void SetOutput(EOutResource _index, TextureRef _texture) = 0;

        void SetFrameIndex(uint32 _frame_index) {
            nrd_common_settings.frameIndex = _frame_index;
        }

        void SetCameraMatrix(const Matrix4x4f& _view, const Matrix4x4f& _proj) {
            // Set common settings
            memcpy(nrd_common_settings.worldToViewMatrixPrev, nrd_common_settings.worldToViewMatrix, sizeof(nrd_common_settings.worldToViewMatrix));
            memcpy(nrd_common_settings.viewToClipMatrixPrev, nrd_common_settings.viewToClipMatrix, sizeof(nrd_common_settings.viewToClipMatrix));
            memcpy(nrd_common_settings.worldToViewMatrix, &_view, sizeof(nrd_common_settings.worldToViewMatrix));
            memcpy(nrd_common_settings.viewToClipMatrix, &_proj, sizeof(nrd_common_settings.viewToClipMatrix));
        }

        void SetCommonSettings(const nrd::CommonSettings& _settings) {
            nrd_common_settings = _settings;
        }

        void SetDenoiserSettings(const nrd::ReblurSettings& _settings) {
            nrd_denoiser_settings = _settings;
        }

        void SetDenoiserSettings(const nrd::RelaxSettings& _settings) {
            nrd_denoiser_settings = _settings;
        }

    private:
        void SetDefaultDenoiserSettings(const nrd::Identifier _denoiser_id) {
            // import from RTXDI
            if (_denoiser_id < nrd::Identifier(nrd::Denoiser::RELAX_DIFFUSE)) {
                auto reblur_settings                      = nrd::ReblurSettings();
                reblur_settings.enableAntiFirefly         = true;
                reblur_settings.diffusePrepassBlurRadius  = 30.0f;
                reblur_settings.specularPrepassBlurRadius = 30.0f;
                nrd_denoiser_settings                     = reblur_settings;
            } else if (_denoiser_id < nrd::Identifier(nrd::Denoiser::SIGMA_SHADOW)) {
                auto relax_settings                                      = nrd::RelaxSettings();
                relax_settings.diffuseMaxFastAccumulatedFrameNum         = 1;
                relax_settings.specularMaxFastAccumulatedFrameNum        = 1;
                relax_settings.diffusePhiLuminance                       = 1.0f;
                relax_settings.spatialVarianceEstimationHistoryThreshold = 1;
                relax_settings.enableAntiFirefly                         = true;
                relax_settings.diffusePrepassBlurRadius                  = 30.0f;
                relax_settings.specularPrepassBlurRadius                 = 30.0f;
                nrd_denoiser_settings                                    = relax_settings;
            } else {
                // not implemented
            }
        }

        void SetDefaultCommonSettings() {
            // import from RTXDI
            nrd_common_settings                                     = nrd::CommonSettings();
            nrd_common_settings.rectOrigin[0]                       = 0;
            nrd_common_settings.rectOrigin[1]                       = 0;
            nrd_common_settings.timeDeltaBetweenFrames              = 0.0f;
            nrd_common_settings.denoisingRange                      = 1000.0f;
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

    protected:
        nrd::Denoiser type = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;

        struct NRDEntry {
            struct NRIEntry {
                struct NRIInterface
                    : public nri::CoreInterface,
                      public nri::HelperInterface,
                      public nri::WrapperVKInterface {
                };
                NRIInterface        rhi      = {};
                nri::Device*        device   = nullptr;
                nri::CommandBuffer* cmd_list = nullptr;
            };

            uint16 frame_width         = 0;
            uint16 frame_height        = 0;
            uint8  max_frame_in_flight = 0;

            NRIEntry nri = {};

            nrd::Integration integration = {};

            nrd::UserPool user_pool = {};
        };

        NRDEntry nrd = {};

        StaticArray<nri::TextureBarrierDesc, uint8(EInResource::INPUT_NUM) + uint8(EOutResource::OUTPUT_NUM)> texture_barrier_descs = {};
    };

    class NRDExtension : public DeviceExtension {
    public:
        ~NRDExtension()                        = default;
        static constexpr std::string_view name = "NRDExt";

        virtual UniquePtr<NRDInterface> CreateInterface(
            uint8  _max_frame_in_flight = 0,
            uint16 _frame_width         = 0,
            uint16 _frame_height        = 0) = 0;
    };
}// namespace Moer::Render::Ext

#endif