#pragma once

#include "Core.h"
#include "misc/Traits.h"
#include "renderer/UIRenderer.h"
#include "rhi/RHIResource.h"

#include "ui/raster_ui/RasterUI.h"
#include "ui/raytracing_ui/RaytracingUI.h"

namespace Moer {

enum class ERenderMethod {
    Raster,
    Raytracing,
};

constexpr std::string_view k_render_method_names[] = {"Raster", "Raytracing"};

class EditorUI {

public:
    struct Config {
        ERenderMethod selected_render_method = ERenderMethod::Raster;
        std::string   scene_path             = "";

        float camera_speed = 25.f;
        float camera_fovy  = 60.f;
    };

    EditorUI(UniquePtr<Render::UIRenderer> renderer, uint2 resolution);
    ~EditorUI() = default;
    void InitFromConfigManager(); // will be called by Constructor
    void TickUI();
    void RenderGUI(Render::CommandList& cmd_list, const Render::TextureView& final_output);
    void PresentWindows();

    float2 GetSceneColorResolution() const {
        return m_scene_color_resolution;
    }
    float2 GetSceneColorPos() const {
        return m_scene_color_pos;
    }
    uint2 GetResolution() const {
        return m_resolution;
    }
    const Config& GetConfig() const {
        return m_config;
    }
    bool IsNeedReload() const {
        return m_b_need_reload;
    }
    float GetSceneColorAspectRatio() const {
        return m_scene_color_resolution.x / m_scene_color_resolution.y;
    }

    void SetShowSubUI(bool show) {
        m_b_show_sub_ui = show;
    }

    bool                IsSeperateWindow() const;
    Render::TextureView GetWindowFrameBuffer();

    void RegisterUIFunc(std::string_view _name, std::function<void()>&& _func);
    void UnregisterUIFunc(std::string_view _name);

public: // Sub UI
    RasterUI     m_raster_ui;
    RaytracingUI m_raytracing_ui;

private:
    void ResetState(); // reset m_b_need_reload, etc..
    void ShowSceneColor();
    void ShowConfig();

private:
    bool   m_b_show_scene_color = true;
    bool   m_b_show_config      = true;
    float2 m_scene_color_resolution; // TODO: why float2? not uint2?
    float2 m_scene_color_pos;
    bool   m_b_show = true;

    bool m_b_need_reload = false;
    bool m_b_show_sub_ui = false;

    Config m_config;

    UniquePtr<Render::UIRenderer> m_ui_renderer;
    uint2                         m_resolution; // TODO: update resolution in EditorUI

    // Custom Func
    UnorderedMap<std::string_view, std::function<void()>> m_show_func_map;
};

} // namespace Moer