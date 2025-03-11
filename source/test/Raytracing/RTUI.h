#ifndef MOER_TEST_RTUI_H
#define MOER_TEST_RTUI_H
#include "AntiAliasPass.h"
#include "Configs.h"
#include "Core.h"
#include "misc/STL.h"
#include "renderer/UIRenderer.h"
#include "rhi/RHIResource.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"
namespace Moer::Render {
    enum EAnitiAliasMode {
        EAA_TAA = 0,
        EAA_Num
    };
    enum EOutputTexture {
        EOT_LDR = 0,
        EOT_HDR,
        EOT_Num
    };
    class RTUI {
    public:
        struct GridConfig {
            int   grid_mode      = 1;
            int   light_per_ceil = 512;
            float cell_size      = 1.f;
        };

        struct ReSTIRDIInitialSampleConfig {
            uint local_light_sample_mode = s_di_local_light_sample_mode_grid;
        };

        struct ReSTIRDITemporalResampleConfig {
            uint  bias_correction  = s_di_bias_correction_traced;
            float depth_threshold  = 10.f;
            float normal_threshold = 0.5f;
        };

        struct ReSTIRDISpatialResampleConfig {
            uint  bias_correction     = s_di_bias_correction_traced;
            float depth_threshold     = 0.2f;
            float normal_threshold    = 0.5f;
            int   num_spatial_samples = 2;
        };

        struct ReSTIRDIConfig {
            ReSTIRDIInitialSampleConfig    initial_sample_config;
            ReSTIRDITemporalResampleConfig temporal_resample_config;
            ReSTIRDISpatialResampleConfig  spatial_resample_config;
        };
        struct ReBlurHitDistParams {
            float A = 3.f;
            float B = 0.1f;
            float C = 20.f;
            float D = -25.f;
        };

        struct ReBlurAntilagParams {
            float luminance_sigma_scale = 2.f;
            float hit_dist_sigma_scale  = 2.f;

            float luminance_antilag_power = .5f;
            float hit_dist_antilag_power  = 1.f;
        };
        struct DenoiserConfig {
            ReBlurHitDistParams hit_dist_params{};
            ReBlurAntilagParams antilag_params{};
            uint                denoiser_type = s_denoiser_mode_reblur;
        };

        struct ToneMappingConfig {
            float histogram_low_percentile  = 0.8f;
            float histogram_high_percentile = 0.95f;
            float eye_adaptation_speed_up   = 1.f;
            float eye_adaptation_speed_down = 0.5f;
            float min_adapted_luminance     = 0.02f;
            float max_adapted_luminance     = 0.5f;
            float exposure_bias             = -0.5f;
            float white_point               = 1.5f;
            bool  enable_color_lut          = true;
        };

        struct AntiAliasConfig {
            EAnitiAliasMode        aa_mode                 = EAnitiAliasMode::EAA_TAA;
            AntialiasPass::EJitter jitter_mode             = AntialiasPass::EJitter::MSAA;
            float                  new_frame_weight        = 0.04f;
            float                  clamping_factor         = 1.3f;
            float                  max_radiance            = 200.f;
            bool                   enable_history_clamping = true;
        };

        struct ExportConfig {
            EOutputTexture output_texture = EOutputTexture::EOT_LDR;
            bool           b_export       = false;
        };
        struct Config {
            bool b_reset = false;

            float3 sun_direction        = float3(0.f, 0.5f, 0.16f);
            float  exposure             = 6.f;
            float  sun_angular_diameter = 0.533f;
            uint   max_bounce           = 4;

            GridConfig        grid_config{};
            ReSTIRDIConfig    restir_di_cfg{};
            DenoiserConfig    denoiser_cfg{};
            ToneMappingConfig tone_mapping_cfg{};
            AntiAliasConfig   aa_cfg{};
            ExportConfig      export_cfg{};
            EFinalColor       final_color = EFinalColor::EFC_SceneColor;
        };
        RTUI(UIRenderer& _renderer);
        ~RTUI() = default;
        void TickUI();

        float2  GetSceneColorResolution() const { return scene_color_resolution; }
        float2  GetSceneColorPos() const { return scene_color_pos; }
        Config& GetConfig() { return config; }

        bool        IsSeperateWindow() const;
        TextureView GetWindowFrameBuffer();

        void RegisterUIFunc(std::string_view _name, std::function<void()>&& _func);
        void UnregisterUIFunc(std::string_view _name);

    private:
        void ShowSceneColor();
        void ShowConfig();

    private:
        bool   b_show_scene_color = true;
        bool   b_show_config      = true;
        float2 scene_color_resolution;
        float2 scene_color_pos;
        bool   b_show = true;
        Config config;

        UnorderedMap<std::string, uint>                       final_color_map;
        UnorderedMap<std::string_view, std::function<void()>> show_func_map;

        UIRenderer& ui_renderer;
    };
}// namespace Moer::Render
#endif