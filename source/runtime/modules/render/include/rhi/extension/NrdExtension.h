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
        NRDInterface() = default;

        virtual ~NRDInterface();

        virtual void Begin() = 0;

        virtual void Denoise(CommandList& _cmd_list) = 0;

    private:
        nrd::CommonSettings nrd_common_settings = {};

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
        virtual void SetInput(EInResource _index, TextureRef _texture)   = 0;
        virtual void SetOutput(EOutResource _index, TextureRef _texture) = 0;

        RENDER_API void UseDenoiser(const nrd::Denoiser _denoiser);
        RENDER_API void UpdateCommonSettings(uint32 _frame_index, const Vector2ui& _size, const Vector2f& _jitter, const Matrix4x4f& _view, const Matrix4x4f& _proj);
        RENDER_API void SetCommonSettings(const nrd::CommonSettings& _settings);
        RENDER_API void SetDenoiserSettings(const nrd::ReblurSettings& _settings);
        RENDER_API void SetDenoiserSettings(const nrd::RelaxSettings& _settings);

    protected:
        void SetDefaultDenoiserSettings(const nrd::Identifier _denoiser_id);
        void SetDefaultCommonSettings(uint16 _frame_width, uint16 _frame_height);

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