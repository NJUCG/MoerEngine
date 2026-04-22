#pragma once

#include "Core.h"
#include "misc/Traits.h"
#include "renderer/EditorConfig.h"
#include "renderer/common/UIRenderer.h"
#include "rhi/RHIResource.h"

namespace Moer {

class EditorUI {

public:
    struct SceneWindowTarget {
        bool                is_separate_window = false;
        Render::TextureView frame_buffer;
    };

    EditorUI(UniquePtr<Render::UIRenderer> renderer, SharedPtr<EditorConfig> editor_config);
    ~EditorUI() = default;
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

    SceneWindowTarget   GetSceneWindowTarget();
    void RegisterRendererConfigSection(
        std::string renderer_name,
        std::string section_name,
        std::function<void()>&& func
    );
    void UnregisterRendererConfigSection(std::string renderer_name, std::string section_name);
    void RegisterOverlayFunc(std::string _name, std::function<void()>&& _func);
    void UnregisterOverlayFunc(std::string _name);
    void BindConsoleWindowState(std::function<bool()> getter, std::function<void(bool)> setter);

private:
    void ResetState(); // reset m_b_need_reload, etc..
    void ShowSceneColor();
    void ShowConfig();
#if WITH_PROFILE
    void ShowMemoryProfiler(bool* p_open);
    void DrawPassAndChildren(const char* parent_name, int depth);
#endif
    void ShowOverlay();
    void ApplyInputSnapshot();

private:
    bool   m_b_show_scene_color = true;
    bool   m_b_show_config      = true;
    float2 m_scene_color_resolution; // TODO: why float2? not uint2?
    float2 m_scene_color_pos;
    bool   m_b_show = true;
    bool   m_scene_color_hovered = false;

    bool m_b_need_reload = false;

#if WITH_PROFILE
    bool   m_b_show_memory_profiler = false;
    float2 m_memory_profiler_pos{};
    float2 m_memory_profiler_resolution{300, 200};
#endif

    SharedPtr<EditorConfig> m_config;

    UniquePtr<Render::UIRenderer> m_ui_renderer;

    UnorderedMap<std::string, UnorderedMap<std::string, std::function<void()>>> m_renderer_config_sections;
    UnorderedMap<std::string, std::function<void()>> m_overlay_func_map;
    std::function<bool()>                            m_get_console_window_visible;
    std::function<void(bool)>                        m_set_console_window_visible;
};

} // namespace Moer
