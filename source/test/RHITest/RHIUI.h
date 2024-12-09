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