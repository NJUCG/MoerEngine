#pragma once

namespace Moer {

// Editor 顶层窗口显示状态。ImGui 默认只保存 layout，不保存 p_open 状态
// 如果新增一个需要被 Window 菜单控制、并且关闭状态需要持久化的顶层 Sub UI，请沿这条路径同步修改
// 1. 在 EditorUI.h 中添加对应的 m_b_show_xxx 成员，作为 ImGui::Begin 的 p_open 数据源
// 2. 在 EditorUI.cpp 的 Window 菜单中添加 ImGui::MenuItem，并绑定同一个 m_b_show_xxx
// 3. 在 EditorUI.cpp 中添加或调用 ShowXxx，把 m_b_show_xxx 传给 ImGui::Begin("Xxx", &m_b_show_xxx)
// 4. 在 EditorUI 构造函数中，从 EditorUISettings::LoadWindowVisibilitySettings() 恢复 m_b_show_xxx
// 5. 在 EditorUI::SyncWindowVisibilitySettings() 中，把 m_b_show_xxx 写回 EditorWindowVisibilitySettings
// 6. 在本 struct 中添加对应 bool 字段，字段默认值决定没有 ini 记录时的默认显示状态
// 7. 在 EditorUISettings.cpp 中同步更新 ParseEditorWindowVisibilityLine / WriteAll / IsSameWindowVisibilitySettings
struct EditorWindowVisibilitySettings {
    bool loaded          = false;
    bool scene_color     = true;
    bool scene_view      = true;
    bool hierarchy       = true;
    bool inspector       = true;
    bool config          = true;
    bool scene_editing   = true;
    bool memory_profiler = false;
};

namespace EditorUISettings {

const EditorWindowVisibilitySettings& LoadWindowVisibilitySettings();
const EditorWindowVisibilitySettings& GetWindowVisibilitySettings();
void StoreWindowVisibilitySettings(const EditorWindowVisibilitySettings& settings);

} // namespace EditorUISettings

} // namespace Moer
