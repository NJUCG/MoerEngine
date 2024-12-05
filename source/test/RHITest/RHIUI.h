#ifndef MOER_TEST_RHIUI_H
#define MOER_TEST_RHIUI_H
#include "Core.h"
#include "renderer/UIRenderer.h"
#include "rhi/RHIResource.h"

namespace Moer::Render {

    /**
     * Reference: test/Raytracing/RTUI.h
     * 
     * TODO: merge common code into a base class
     */
    class RHIUI {

    public:
        const Array<std::string> k_aa_mode_name_array = {
            "None",
            "FXAA Simplified",
            "FXAA Quality",
            "SMAA 1x",
            "SMAA T2x",
        };

        struct Config {
            uint aa_mode = 3;
        };

        RHIUI(UIRenderer& _renderer) : ui_renderer(_renderer) {}
        ~RHIUI() = default;
        void TickUI();

        float2        GetSceneColorResolution() const { return scene_color_resolution; }
        float2        GetSceneColorPos() const { return scene_color_pos; }
        const Config& GetConfig() const { return config; }
        float         GetSceneColorAspectRatio() const { return scene_color_resolution.x / scene_color_resolution.y; }

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

        UIRenderer& ui_renderer;
    };
}// namespace Moer::Render
#endif