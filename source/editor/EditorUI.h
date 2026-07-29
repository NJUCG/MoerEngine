#pragma once

// 协调编辑器顶层 ImGui 窗口，并将其状态传递给运行时渲染器。

#include "misc/Traits.h"
#include "remote/RemoteModuleController.h"
#include "renderer/EditorConfig.h"
#include "renderer/common/UIRenderer.h"
#include "rhi/RHIResource.h"

#include "inspector_ui/InspectorUI.h"
#include "profile_capture_ui/ProfileCaptureUI.h"
#include "raster_ui/RasterUI.h"
#include "raytracing_ui/RaytracingUI.h"
#include "scene_editing_ui/SceneEditingUI.h"
#include "subui/RemoteExamplesUI.h"

#include <entt/entity/entity.hpp>
#include <string>

namespace Moer {

class Scene;
class Engine;

/**
 * EditorUI 是编辑器主界面的窗口协调器。
 *
 * 结构:
 * - Scene Color / Hierarchy / Inspector / Config / SceneEditing 五类顶层窗口
 * - RasterUI / RaytracingUI / InspectorUI / SceneEditingUI 四个子模块
 * - 少量 editor 状态: 选中节点、scene color 区域、renderer reload 状态
 *
 * 改这里:
 * - 改主窗口布局和 docking: EditorUI.cpp
 * - 改 Inspector / SceneEditing 细节: inspector_ui 目录 + scene_editing_ui 目录
 * - 改 renderer 配置面板: raster_ui 目录 + raytracing_ui 目录 + Editor.cpp 回调
 *
 * 用法:
 * - 每帧先 TickUI(scene) 组装 ImGui
 * - 再 CaptureDrawFrame() 复制主视口和独立平台窗口的绘制数据
 *
 * 边界约束:
 * - 这里只负责窗口编排、菜单和 editor 级状态，不直接改 runtime 内部实现层
 * - Scene 相关 UI 应优先通过 Scene 正式 API 或 SceneEditing 完成，不直接访问 registry / LogicalScene
 */
class EditorUI {

public:
    EditorUI(
        UniquePtr<Render::UIRenderer>         renderer,
        SharedPtr<EditorConfig>               editor_config,
        const remote::RemoteModuleController& remote_controller,
        Engine&                               engine
    );
    ~EditorUI() = default;
    void TickUI(Scene& scene);
    Render::UiDrawFramePacket CaptureDrawFrame();

    float2 GetSceneColorResolution() const {
        return m_scene_color_resolution;
    }
    float2 GetSceneColorPos() const {
        return m_scene_color_pos;
    }
    const SharedPtr<EditorConfig> GetConfig() const {
        return m_config;
    }
    bool NeedsReload() const {
        return m_b_need_reload;
    }
    float GetSceneColorAspectRatio() const {
        return m_scene_color_resolution.x / m_scene_color_resolution.y;
    }

    void SetShowRenderConfigSubUI(bool show) {
        m_b_show_render_config_sub_ui = show;
    }

    bool               IsSeparateWindow() const;
    Render::TextureRef GetWindowFrameBuffer();

    void RegisterUIFunc(std::string name, std::function<void()>&& callback);
    void UnregisterUIFunc(std::string name);

public: // Sub UI
    RasterUI       m_raster_ui;
    RaytracingUI   m_raytracing_ui;
    SceneEditingUI m_scene_editing_ui;
    RemoteExamplesUI m_remote_examples_ui;
    ProfileCaptureUI m_profile_capture_ui;

private:
    void ResetFrameState();
    void ShowFileMenu(Scene& scene, bool is_scene_loading);
    void ShowGameView();
    void ShowSceneView(Scene& scene);
    void ShowViewportWindow(const char* window_name, bool* p_open, EEditorViewportMode viewport_mode);
    void ShowConfig(Scene& scene);
    void ShowSceneEditing(Scene& scene);
    void ShowHierarchy(Scene& scene);
    void ShowInspector(Scene& scene);
    const char* GetActiveViewportWindowName() const;

    // 同步顶层子窗口开关到 ImGui ini 自定义段
    void SyncWindowVisibilitySettings();
#if WITH_PROFILE
    void ShowMemoryProfiler(bool* p_open);
    void DrawPassAndChildren(const char* parent_name, int depth);
#endif

private:
    bool   m_b_show_scene_color           = true;
    bool   m_b_show_scene_view            = true;
    bool   m_b_show_hierarchy             = true;
    bool   m_b_show_inspector             = true;
    bool   m_b_show_config                = true;
    bool   m_b_show_scene_editing         = true;
    bool   m_b_show_profile_capture       = false;
    bool   m_b_scene_color_mouse_captured = false;
    bool   m_b_active_viewport_window_seen = false;
    // ImGui 窗口几何信息使用浮点屏幕坐标表示。
    float2 m_scene_color_resolution;
    float2 m_scene_color_pos;
    bool   m_b_show = true;

    bool        m_b_need_reload               = false;
    bool        m_b_show_render_config_sub_ui = true; // 当前 renderer 的专属配置面板是否可见。
    std::string m_last_file_action_status;

    // Hierarchy selection state
    entt::entity m_selected_node = entt::null;

#if WITH_PROFILE
    bool   m_b_show_memory_profiler = false;
    float2 m_memory_profiler_pos{};
    float2 m_memory_profiler_resolution{300, 200};
#endif

    SharedPtr<EditorConfig>        m_config;
    remote::RemoteModuleController m_remote_controller;

    UniquePtr<Render::UIRenderer> m_ui_renderer;
    InspectorUI                   m_inspector_ui;

    // Custom Func
    UnorderedMap<std::string, std::function<void()>> m_show_func_map;
};

} // namespace Moer
