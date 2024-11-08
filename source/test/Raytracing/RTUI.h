#ifndef MOER_TEST_RTUI_H
#define MOER_TEST_RTUI_H
#include "Core.h"
#include "renderer/UIRenderer.h"
#include "rhi/RHIResource.h"
namespace Moer::Render {
    class RTUI {
    public:
        RTUI(UIRenderer& _renderer) : ui_renderer(_renderer) {}
        ~RTUI() = default;
        void TickUI();

        float2 GetSceneColorResolution() const { return scene_color_resolution; }
        float2 GetSceneColorPos() const { return scene_color_pos; }

        bool        IsSeperateWindow() const;
        TextureView GetWindowFrameBuffer();

    private:
        void ShowSceneColor();

    private:
        bool   b_show_scene_color = true;
        float2 scene_color_resolution;
        float2 scene_color_pos;
        bool   b_show = true;

        UIRenderer& ui_renderer;
    };
}// namespace Moer::Render
#endif