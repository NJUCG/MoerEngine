#pragma once

#include "Core.h"
#include "misc/Traits.h"
#include "renderer/EditorConfig.h"
#include "renderer/common/UIRenderer.h"
#include "rhi/RHIResource.h"

#include "raster_ui/RasterUI.h"
#include "raytracing_ui/RaytracingUI.h"

namespace Moer {

class EditorUI {

public:
    struct SceneWindowTarget {
        bool                is_separate_window = false;
        Render::TextureView frame_buffer;
    };

    EditorUI(UniquePtr<Render::UIRenderer> renderer, SharedPtr<EditorConfig> editor_config);
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
    const SharedPtr<EditorConfig> GetConfig() const {
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

    SceneWindowTarget   GetSceneWindowTarget();
    bool                IsSeperateWindow() const;
    Render::TextureView GetWindowFrameBuffer();

    void RegisterUIFunc(std::string _name, std::function<void()>&& _func);
    void UnregisterUIFunc(std::string _name);
    void RegisterOverlayFunc(std::string _name, std::function<void()>&& _func);
    void UnregisterOverlayFunc(std::string _name);

public: // Sub UI
    RasterUI     m_raster_ui;
    RaytracingUI m_raytracing_ui;

private:
    void ResetState(); // reset m_b_need_reload, etc..
    void ShowSceneColor();
    void ShowConfig();
#if WITH_PROFILE
    void ShowMemoryProfiler(bool* p_open);
#endif
    void ShowOverlay();

private:
    bool   m_b_show_scene_color = true;
    bool   m_b_show_config      = true;
    float2 m_scene_color_resolution; // TODO: why float2? not uint2?
    float2 m_scene_color_pos;
    bool   m_b_show = true;

    bool m_b_need_reload = false;
    bool m_b_show_sub_ui = true; // TODO: 【10.3 Refactor】这玩意是干什么的？

#if WITH_PROFILE
    bool m_b_show_memory_profiler = false;
    float2 m_memory_profiler_pos{};
    float2 m_memory_profiler_resolution{300, 200};
#endif

    SharedPtr<EditorConfig> m_config;

    UniquePtr<Render::UIRenderer> m_ui_renderer;

    // Custom Func
    UnorderedMap<std::string, std::function<void()>> m_show_func_map;
    UnorderedMap<std::string, std::function<void()>> m_overlay_func_map;
};

} // namespace Moer
