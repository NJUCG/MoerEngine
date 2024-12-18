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
        const Array<std::string> k_ao_mode_name_array = {
            "None",
            "SSAO",
            "SSAO AO Only",
            "SSDO (TODO)",
            "SSDO AO Only (TODO)",
            "Linearized Depth / 10.0",
        };

        struct Config {
            uint aa_mode = 3;// default ssma 1x

            uint  ao_mode           = 1;// default ssao
            float ssao_intensity    = 1.0f;
            int   ssao_sample_count = 8;
            int   ssao_radius       = 2;
            float ssao_max_distance = 0.1f;

            bool  ssr_is_enable_ssr              = true;
            int   ssr_sample_count               = 32;
            bool  ssr_is_enable_jitter           = true;
            bool  ssr_is_force_ground_enable_ssr = true;
            float ssr_roughness_threshold        = 0.5;
            float ssr_metallic_threshold         = 0.5;
            float ssr_step_base                  = 0.025;

            uint selected_frame_buffer_index = 0;
        };

        RHIUI(
            UIRenderer&                                       _renderer,
            const Array<std::pair<TextureView, std::string>>& frame_buffer_and_name_array,
            uint                                              default_selected_frame_buffer_index);
        ~RHIUI() = default;
        void TickUI();

        float2        GetSceneColorResolution() const { return m_scene_color_resolution; }
        float2        GetSceneColorPos() const { return m_scene_color_pos; }
        const Config& GetConfig() const { return m_config; }
        float         GetSceneColorAspectRatio() const { return m_scene_color_resolution.x / m_scene_color_resolution.y; }

        void        RegisterFrameBuffers(const Array<std::pair<TextureView, std::string>>& frame_buffer_and_name_array,
                                         uint                                              default_selected_frame_buffer_index);
        TextureView GetSelectedFrameBuffer() const { return m_frame_buffer_and_name_array[m_config.selected_frame_buffer_index].first; }

        bool        IsSeperateWindow() const;
        TextureView GetWindowFrameBuffer();

    private:
        void InitUIStyle();
        void ShowSceneColor();
        void ShowConfig();

    private:
        bool   m_b_show_scene_color = true;
        bool   m_b_show_config      = true;
        float2 m_scene_color_resolution;
        float2 m_scene_color_pos;
        bool   m_b_show = true;

        Array<std::pair<TextureView, std::string>> m_frame_buffer_and_name_array;

        Config m_config;

        UIRenderer& m_ui_renderer;
    };
}// namespace Moer::Render
#endif