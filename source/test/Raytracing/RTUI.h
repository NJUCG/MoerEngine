#ifndef MOER_TEST_RTUI_H
#define MOER_TEST_RTUI_H
#include "Configs.h"
#include "Core.h"
#include "misc/STL.h"
#include "renderer/UIRenderer.h"
#include "rhi/RHIResource.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"
namespace Moer::Render {

    class RTUI {
    public:
        struct GridConfig {
            int   grid_mode      = 1;
            int   light_per_ceil = 512;
            float cell_size      = 1.f;
        };

        struct ReSTIRDIConfig {
            uint initial_local_light_sample_mode = 0;
        };
        struct Config {
            float3 sun_direction        = float3(0.f, 0.5f, 0.16f);
            float  exposure             = 80.f;
            float  sun_angular_diameter = 0.533f;
            uint   max_bounce           = 4;

            GridConfig     grid_config{};
            ReSTIRDIConfig restir_di_cfg{};
            EFinalColor    final_color = EFinalColor::EFC_SceneColor;
        };
        RTUI(UIRenderer& _renderer);
        ~RTUI() = default;
        void TickUI();

        float2  GetSceneColorResolution() const { return scene_color_resolution; }
        float2  GetSceneColorPos() const { return scene_color_pos; }
        Config& GetConfig() { return config; }

        bool        IsSeperateWindow() const;
        TextureView GetWindowFrameBuffer();

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

        UnorderedMap<std::string, uint> final_color_map;

        UIRenderer& ui_renderer;
    };
}// namespace Moer::Render
#endif