#pragma once

#include "RenderAPI.h"
#include "misc/Traits.h"
#include "renderer/common/UIRenderer.h"
#include "renderer/common/ui/ImGuiIOInput.h"

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace Moer::Synapse {

struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct Size {
    float x = 0.0f;
    float y = 0.0f;
};

enum class EMouseButton {
    Left,
    Middle
};

enum class EKey {
    F,
    LeftArrow,
    RightArrow
};

enum class EIcon {
    Play,
    Stop,
    FolderOpen,
    Settings,
    Warning,
    Search,
    Save,
    Reload,
    Eye,
    Camera,
    ChevronRight
};

struct Theme {
    Color text                  = {0.90f, 0.90f, 0.90f, 1.00f};
    Color text_disabled         = {0.60f, 0.60f, 0.60f, 1.00f};
    Color panel_bg              = {0.01f, 0.01f, 0.01f, 1.00f};
    Color panel_border          = {0.08f, 0.07f, 0.10f, 0.50f};
    Color panel_header          = {0.05f, 0.03f, 0.12f, 0.62f};
    Color panel_header_hovered  = {0.10f, 0.06f, 0.22f, 0.62f};
    Color panel_header_active   = {0.12f, 0.06f, 0.30f, 0.62f};
    Color toolbar_bg            = {0.00f, 0.00f, 0.02f, 0.80f};
    Color accent                = {0.91f, 0.76f, 0.09f, 1.00f};
    Color button                = {0.18f, 0.04f, 0.39f, 0.62f};
    Color button_hovered        = {0.27f, 0.13f, 0.49f, 0.62f};
    Color button_active         = {0.45f, 0.25f, 0.75f, 0.62f};
    Color docking_preview       = {0.52f, 0.40f, 0.90f, 0.31f};
    Color docking_empty_bg      = {0.20f, 0.20f, 0.20f, 1.00f};

    float item_width            = 120.0f;
    float panel_rounding        = 7.0f;
    float child_rounding        = 4.0f;
    float frame_rounding        = 3.0f;
    float popup_rounding        = 4.0f;
    float toolbar_height        = 28.0f;
    float icon_button_size      = 24.0f;
};

struct DockspaceDesc {
    const char* host_name       = "Editor Menu";
    const char* dockspace_name  = "Docking Main";
    const char* scene_panel     = "Scene Color";
    const char* config_panel    = "Configs";
    const char* console_panel   = "Console";
    const char* profiler_panel  = "runtime_profiler";
    bool*       open            = nullptr;
    bool        fullscreen      = true;
    bool        padding         = false;
    bool        enable_profiler = false;
};

struct PanelDesc {
    const char* name       = nullptr;
    bool*       open       = nullptr;
    bool        background = true;
    bool        menu_bar   = false;
};

struct PanelHeaderDesc {
    const char* title = nullptr;
    EIcon       icon  = EIcon::ChevronRight;
};

struct SceneViewportState {
    bool   visible            = false;
    bool   separate_window    = false;
    bool   hovered            = false;
    bool   focused            = false;
    bool   input_started      = false;
    bool   mouse_down         = false;
    float2 content_pos        = {0.0f, 0.0f};
    float2 content_resolution = {0.0f, 0.0f};
    Render::UIRenderer::RenderOutputSlotHandle render_output_slot;
};

class RENDER_API Context {
public:
    Context();

    void ApplyTheme(const Theme& theme);
    void ApplyDefaultTheme();

    void BeginFrame(const Render::ImGuiIOInputSnapshot& input_snapshot);
    void EndFrame();

    const Theme& GetTheme() const;
    const Render::ImGuiIOInputSnapshot& GetInputSnapshot() const;
    float2 GetMainViewportWorkSize() const;
    float  GetFramerate() const;

    bool BeginMainDockspace(const DockspaceDesc& desc);
    void EndMainDockspace();

    bool BeginRootPanel(const PanelDesc& desc);
    bool BeginPanel(const PanelDesc& desc);
    void EndPanel();
    bool BeginChild(const char* id, Size size = {0.0f, 0.0f}, bool border = false, bool horizontal_scrollbar = false);
    void EndChild();
    void PanelHeader(const PanelHeaderDesc& desc);
    SceneViewportState DrawSceneViewportPanel(const char* name, bool* open, Render::UIRenderer& ui_renderer);

    bool BeginMenuBar();
    void EndMenuBar();
    bool BeginMenu(const char* label);
    void EndMenu();
    bool MenuItem(const char* label, bool* selected = nullptr);

    bool BeginSection(const char* label);
    bool BeginSection(const char* label, const char* summary);
    bool TreeNode(const char* label);
    bool TreeNode(const char* label, const char* fmt, ...);
    void TreePop();

    bool BeginCombo(const char* label, const char* preview_value);
    void EndCombo();
    void SetItemDefaultFocus();

    bool Selectable(const char* label, bool selected = false);
    bool Checkbox(const char* label, bool* value);
    bool Button(const char* label);
    bool InputTextWithHint(const char* label, const char* hint, char* buffer, size_t buffer_size);
    bool IconButton(EIcon icon, const char* tooltip, bool active = false);
    bool ToolbarButton(EIcon icon, const char* label, bool active = false);
    bool SliderFloat(const char* label, float* value, float min_value, float max_value);
    bool SliderFloat3(const char* label, float* value, float min_value, float max_value);
    bool SliderInt(const char* label, int* value, int min_value, int max_value);
    void ProgressBar(float fraction, Size size = {0.0f, 0.0f});

    void Text(const char* fmt, ...);
    void TextDisabled(const char* fmt, ...);
    void TextWrapped(const char* fmt, ...);
    void TextColored(Color color, const char* fmt, ...);

    void Separator();
    void SeparatorText(const char* label);
    void Spacing();
    void SameLine();
    void Dummy(Size size);
    void Indent(float width = 0.0f);
    void Unindent(float width = 0.0f);
    void PushItemWidth(float width);
    void PopItemWidth();
    void BeginDisabled(bool disabled = true);
    void EndDisabled();
    void DrawLastItemBorder(Color color = {1.0f, 1.0f, 1.0f, 1.0f});

    Size  GetCursorScreenPos() const;
    Size  GetContentRegionAvail() const;
    Size  GetMousePos() const;
    Size  GetMouseDelta() const;
    float GetMouseWheel() const;
    bool  IsShiftDown() const;
    bool  IsWindowHovered() const;
    bool  IsWindowFocusedChildWindows() const;
    bool  IsMouseDragging(EMouseButton button) const;
    bool  IsMouseHoveringRect(Size min, Size max) const;
    bool  IsMouseClicked(EMouseButton button) const;
    bool  IsKeyDown(EKey key) const;
    bool  IsKeyPressed(EKey key) const;

    float GetFontSize() const;
    Size  CalcTextSize(const char* text) const;
    void  OpenPopup(const char* id);
    bool  BeginPopup(const char* id);
    void  EndPopup();
    void  BeginTooltip();
    void  EndTooltip();

    void DrawLine(Size from, Size to, uint32_t color, float thickness = 1.0f);
    void DrawRect(Size min, Size max, uint32_t color, float rounding = 0.0f, float thickness = 1.0f);
    void DrawRectFilled(Size min, Size max, uint32_t color, float rounding = 0.0f);
    void DrawCircleFilled(Size center, float radius, uint32_t color, int segments = 12);
    void DrawCanvasText(Size pos, uint32_t color, const char* text);
    void DrawTextClipped(Size pos, uint32_t color, const char* text, float font_size, Size clip_min, Size clip_max);

    const char* GetIconGlyph(EIcon icon) const;

private:
    void SetupDefaultDockLayout(const DockspaceDesc& desc, unsigned int dockspace_id);
    void TextV(void (*text_func)(const char*, va_list), const char* fmt, va_list args);

private:
    Theme                                 m_theme;
    const Render::ImGuiIOInputSnapshot*   m_input_snapshot = nullptr;
    bool                                  m_dockspace_open = false;
    bool                                  m_default_dock_layout_applied = false;
};

} // namespace Moer::Synapse