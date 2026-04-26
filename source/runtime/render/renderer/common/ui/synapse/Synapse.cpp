#include "renderer/common/ui/synapse/Synapse.h"

#include "IconsFontAwesome6.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace Moer::Synapse {
namespace {

ImVec4 ToImVec4(Color color) {
    return ImVec4(color.r, color.g, color.b, color.a);
}

ImVec2 ToImVec2(Size size) {
    return ImVec2(size.x, size.y);
}

Size ToSize(ImVec2 size) {
    return {size.x, size.y};
}

ImGuiMouseButton ToImGuiMouseButton(EMouseButton button) {
    switch (button) {
        case EMouseButton::Left:
            return ImGuiMouseButton_Left;
        case EMouseButton::Middle:
            return ImGuiMouseButton_Middle;
        default:
            return ImGuiMouseButton_Left;
    }
}

ImGuiKey ToImGuiKey(EKey key) {
    switch (key) {
        case EKey::F:
            return ImGuiKey_F;
        case EKey::LeftArrow:
            return ImGuiKey_LeftArrow;
        case EKey::RightArrow:
            return ImGuiKey_RightArrow;
        default:
            return ImGuiKey_None;
    }
}

void TextVImpl(const char* fmt, va_list args) {
    ImGui::TextV(fmt, args);
}

void TextDisabledVImpl(const char* fmt, va_list args) {
    ImGui::TextDisabledV(fmt, args);
}

void TextWrappedVImpl(const char* fmt, va_list args) {
    ImGui::TextWrappedV(fmt, args);
}

bool IsAnyCameraMouseButtonDown() {
    return ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
           ImGui::IsMouseDown(ImGuiMouseButton_Middle);
}

bool IsAnyCameraMouseButtonClicked() {
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
           ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
}

bool IsWindowFocusClick(const ImGuiContext& context) {
    return context.IO.MouseClicked[ImGuiMouseButton_Left] || context.IO.MouseClicked[ImGuiMouseButton_Right];
}

bool CanFocusWindow(ImGuiWindow* window) {
    return window && !window->Hidden && !window->Collapsed && !(window->Flags & ImGuiWindowFlags_Tooltip) &&
           !(window->Flags & ImGuiWindowFlags_NoNavFocus);
}

void FocusHoveredWindowOnClick() {
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context || !IsWindowFocusClick(*context) || context->HoveredWindow == nullptr) {
        return;
    }

    ImGuiWindow* window = context->HoveredWindow;
    if (!CanFocusWindow(window)) {
        return;
    }
    ImGui::FocusWindow(window, ImGuiFocusRequestFlags_RestoreFocusedChild | ImGuiFocusRequestFlags_UnlessBelowModal);
}

void RenderFocusedWindowBorder() {
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context || !context->NavWindow) {
        return;
    }

    ImGuiWindow* nav_window = context->NavWindow;
    ImGuiWindow* window = nav_window->RootWindowForTitleBarHighlight;
    if (!window) {
        window = nav_window;
    }
    if ((window->Flags & ImGuiWindowFlags_NoNavFocus) && !(nav_window->Flags & ImGuiWindowFlags_NoNavFocus)) {
        window = nav_window;
    }
    if (!window || window->Hidden || window->Collapsed || !window->Viewport) {
        return;
    }
    if (window->Flags & ImGuiWindowFlags_Tooltip) {
        return;
    }

    const float pulse = 0.65f + 0.35f * (0.5f + 0.5f * ImSin(static_cast<float>(context->Time) * 4.0f));
    const ImU32 glow_color = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.49f, 0.06f, 0.22f * pulse));
    const ImU32 line_color = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.78f, 0.12f, 0.82f * pulse));

    ImRect rect = window->Rect();
    rect.Expand(-1.0f);

    ImDrawList* draw_list = ImGui::GetForegroundDrawList(window->Viewport);
    draw_list->AddRect(rect.Min, rect.Max, glow_color, window->WindowRounding, 0, 3.0f);
    rect.Expand(-1.0f);
    draw_list->AddRect(rect.Min, rect.Max, line_color, window->WindowRounding, 0, 1.0f);
}

} // namespace

Context::Context() = default;

void Context::ApplyTheme(const Theme& theme) {
    m_theme = theme;

    ImGuiStyle& style = ImGui::GetStyle();
    auto&       colors = style.Colors;

    colors[ImGuiCol_Text]                  = ToImVec4(theme.text);
    colors[ImGuiCol_TextDisabled]          = ToImVec4(theme.text_disabled);
    colors[ImGuiCol_WindowBg]              = ToImVec4(theme.panel_bg);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.11f, 0.11f, 0.14f, 0.92f);
    colors[ImGuiCol_Border]                = ToImVec4(theme.panel_border);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.05f, 0.05f, 0.05f, 0.39f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.15f, 0.15f, 0.15f, 0.39f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.36f, 0.36f, 0.36f, 0.39f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.01f, 0.00f, 0.03f, 0.83f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.02f, 0.01f, 0.06f, 0.83f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.02f, 0.01f, 0.05f, 0.83f);
    colors[ImGuiCol_MenuBarBg]             = ToImVec4(theme.toolbar_bg);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.05f, 0.04f, 0.08f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.43f, 0.26f, 0.73f, 0.60f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.55f, 0.39f, 0.81f, 0.60f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.69f, 0.60f, 0.82f, 0.60f);
    colors[ImGuiCol_CheckMark]             = ToImVec4(theme.accent);
    colors[ImGuiCol_SliderGrab]            = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.41f, 0.39f, 0.80f, 0.60f);
    colors[ImGuiCol_Button]                = ToImVec4(theme.button);
    colors[ImGuiCol_ButtonHovered]         = ToImVec4(theme.button_hovered);
    colors[ImGuiCol_ButtonActive]          = ToImVec4(theme.button_active);
    colors[ImGuiCol_Header]                = ToImVec4(theme.panel_header);
    colors[ImGuiCol_HeaderHovered]         = ToImVec4(theme.panel_header_hovered);
    colors[ImGuiCol_HeaderActive]          = ToImVec4(theme.panel_header_active);
    colors[ImGuiCol_Separator]             = ImVec4(0.13f, 0.11f, 0.17f, 0.68f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.17f, 0.16f, 0.21f, 0.68f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.22f, 0.21f, 0.25f, 0.68f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.09f, 0.04f, 0.14f, 0.45f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.13f, 0.08f, 0.18f, 0.45f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.20f, 0.16f, 0.25f, 0.45f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.14f, 0.07f, 0.42f, 0.45f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.20f, 0.13f, 0.45f, 0.45f);
    colors[ImGuiCol_DockingPreview]        = ToImVec4(theme.docking_preview);
    colors[ImGuiCol_DockingEmptyBg]        = ToImVec4(theme.docking_empty_bg);
    colors[ImGuiCol_PlotLines]             = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]      = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogram]         = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.15f, 0.07f, 0.42f, 0.45f);
    colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.09f, 0.04f, 0.30f, 0.45f);
    colors[ImGuiCol_TableBorderLight]      = ImVec4(0.18f, 0.14f, 0.34f, 0.45f);
    colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.07f);

    style.TabRounding       = 4.0f;
    style.ScrollbarRounding = 9.0f;
    style.WindowRounding    = theme.panel_rounding;
    style.GrabRounding      = theme.frame_rounding;
    style.FrameRounding     = theme.frame_rounding;
    style.PopupRounding     = theme.popup_rounding;
    style.ChildRounding     = theme.child_rounding;
}

void Context::ApplyDefaultTheme() {
    ApplyTheme(Theme{});
}

void Context::BeginFrame(const Render::ImGuiIOInputSnapshot& input_snapshot) {
    m_input_snapshot = &input_snapshot;
    FocusHoveredWindowOnClick();
}

void Context::EndFrame() {
    RenderFocusedWindowBorder();
    m_input_snapshot = nullptr;
}

const Theme& Context::GetTheme() const {
    return m_theme;
}

const Render::ImGuiIOInputSnapshot& Context::GetInputSnapshot() const {
    return *m_input_snapshot;
}

float2 Context::GetMainViewportWorkSize() const {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    return {viewport->WorkSize.x, viewport->WorkSize.y};
}

float Context::GetFramerate() const {
    return ImGui::GetIO().Framerate;
}

bool Context::BeginMainDockspace(const DockspaceDesc& desc) {
    ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;
    ImGuiWindowFlags   window_flags    = ImGuiWindowFlags_MenuBar;

    if (desc.fullscreen) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        window_flags |= ImGuiWindowFlags_NoBackground;
    } else {
        dockspace_flags &= ~ImGuiDockNodeFlags_PassthruCentralNode;
    }

    if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode) {
        window_flags |= ImGuiWindowFlags_NoBackground;
    }

    if (!desc.padding) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    }

    m_dockspace_open = ImGui::Begin(desc.host_name, desc.open, window_flags);

    if (!desc.padding) {
        ImGui::PopStyleVar();
    }
    if (desc.fullscreen) {
        ImGui::PopStyleVar(2);
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
        ImGuiID dockspace_id = ImGui::GetID(desc.dockspace_name);
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
        SetupDefaultDockLayout(desc, dockspace_id);
    }

    return m_dockspace_open;
}

void Context::EndMainDockspace() {
    ImGui::End();
    m_dockspace_open = false;
}

bool Context::BeginPanel(const PanelDesc& desc) {
    if (desc.open && !*desc.open) {
        return false;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (!desc.background) {
        flags |= ImGuiWindowFlags_NoBackground;
    }
    if (desc.menu_bar) {
        flags |= ImGuiWindowFlags_MenuBar;
    }

    const bool visible = ImGui::Begin(desc.name, desc.open, flags);
    if (!visible) {
        ImGui::End();
        return false;
    }
    return true;
}

bool Context::BeginRootPanel(const PanelDesc& desc) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    if (!desc.background) {
        flags |= ImGuiWindowFlags_NoBackground;
    }
    if (desc.menu_bar) {
        flags |= ImGuiWindowFlags_MenuBar;
    }
    const bool visible = ImGui::Begin(desc.name, desc.open, flags);
    if (!visible) {
        ImGui::End();
        return false;
    }
    return true;
}

void Context::EndPanel() {
    ImGui::End();
}

bool Context::BeginChild(const char* id, Size size, bool border, bool horizontal_scrollbar) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (horizontal_scrollbar) {
        flags |= ImGuiWindowFlags_HorizontalScrollbar;
    }
    return ImGui::BeginChild(id, ToImVec2(size), border, flags);
}

void Context::EndChild() {
    ImGui::EndChild();
}

void Context::PanelHeader(const PanelHeaderDesc& desc) {
    if (!desc.title) {
        return;
    }
    ImGui::SeparatorText(desc.title);
}

SceneViewportState Context::DrawSceneViewportPanel(const char* name, bool* open, Render::UIRenderer& ui_renderer) {
    SceneViewportState state{};
    if (open && !*open) {
        return state;
    }

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoBackground;
    if (!ImGui::Begin(name, open, window_flags)) {
        ImGui::End();
        return state;
    }

    ImGuiWindow* current_window = ImGui::FindWindowByName(name);
    if (!current_window) {
        ImGui::End();
        return state;
    }

    ImVec2 image_size = ImGui::GetContentRegionAvail();
    image_size.x      = std::max(image_size.x, 1.0f);
    image_size.y      = std::max(image_size.y, 1.0f);

    const ImGuiID imgui_id = ImGui::GetID("##RenderOutput");
    const Render::UIRenderer::RenderOutputSlotHandle slot =
        ui_renderer.RegisterRenderOutputSlot(static_cast<uint32_t>(imgui_id));
    const uint64_t texture_id = ui_renderer.GetRenderOutputTextureId(slot);
    const ImVec2 image_pos = ImGui::GetCursorScreenPos();
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(texture_id)), image_size);

    const ImVec2 viewport_pos = current_window->Viewport ? current_window->Viewport->Pos : ImVec2(0.0f, 0.0f);

    state.visible            = true;
    state.separate_window    = current_window->Viewport != ImGui::GetMainViewport();
    state.content_pos        = {image_pos.x - viewport_pos.x, image_pos.y - viewport_pos.y};
    state.content_resolution = {image_size.x, image_size.y};
    state.hovered            = ImGui::IsItemHovered();
    state.focused            = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    state.input_started      = state.focused && state.hovered && IsAnyCameraMouseButtonClicked();
    state.mouse_down         = IsAnyCameraMouseButtonDown();
    state.render_output_slot = slot;

    ImGui::End();
    return state;
}

bool Context::BeginMenuBar() {
    return ImGui::BeginMenuBar();
}

void Context::EndMenuBar() {
    ImGui::EndMenuBar();
}

bool Context::BeginMenu(const char* label) {
    return ImGui::BeginMenu(label);
}

void Context::EndMenu() {
    ImGui::EndMenu();
}

bool Context::MenuItem(const char* label, bool* selected) {
    return ImGui::MenuItem(label, nullptr, selected);
}

bool Context::BeginSection(const char* label) {
    return TreeNode(label);
}

bool Context::BeginSection(const char* label, const char* summary) {
    return TreeNode(label, "%s", summary);
}

bool Context::TreeNode(const char* label) {
    return ImGui::TreeNode(label);
}

bool Context::TreeNode(const char* label, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const bool open = ImGui::TreeNodeV(label, fmt, args);
    va_end(args);
    return open;
}

void Context::TreePop() {
    ImGui::TreePop();
}

bool Context::BeginCombo(const char* label, const char* preview_value) {
    return ImGui::BeginCombo(label, preview_value);
}

void Context::EndCombo() {
    ImGui::EndCombo();
}

void Context::SetItemDefaultFocus() {
    ImGui::SetItemDefaultFocus();
}

bool Context::Selectable(const char* label, bool selected) {
    const bool changed = ImGui::Selectable(label, selected);
    DrawLastItemBorder({0.22f, 0.21f, 0.25f, 0.68f});
    return changed;
}

bool Context::Checkbox(const char* label, bool* value) {
    return ImGui::Checkbox(label, value);
}

bool Context::Button(const char* label) {
    return ImGui::Button(label);
}

bool Context::InputTextWithHint(const char* label, const char* hint, char* buffer, size_t buffer_size) {
    return ImGui::InputTextWithHint(label, hint, buffer, buffer_size);
}

bool Context::IconButton(EIcon icon, const char* tooltip, bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ToImVec4(m_theme.button_active));
    }
    const bool clicked = ImGui::Button(GetIconGlyph(icon), ImVec2(m_theme.icon_button_size, m_theme.icon_button_size));
    if (active) {
        ImGui::PopStyleColor();
    }
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

bool Context::ToolbarButton(EIcon icon, const char* label, bool active) {
    if (IconButton(icon, label, active)) {
        return true;
    }
    if (label) {
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
    }
    return false;
}

bool Context::SliderFloat(const char* label, float* value, float min_value, float max_value) {
    return ImGui::SliderFloat(label, value, min_value, max_value);
}

bool Context::SliderFloat3(const char* label, float* value, float min_value, float max_value) {
    return ImGui::SliderFloat3(label, value, min_value, max_value);
}

bool Context::SliderInt(const char* label, int* value, int min_value, int max_value) {
    return ImGui::SliderInt(label, value, min_value, max_value);
}

void Context::ProgressBar(float fraction, Size size) {
    ImGui::ProgressBar(fraction, ToImVec2(size));
}

void Context::Text(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    TextV(TextVImpl, fmt, args);
    va_end(args);
}

void Context::TextDisabled(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    TextV(TextDisabledVImpl, fmt, args);
    va_end(args);
}

void Context::TextWrapped(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    TextV(TextWrappedVImpl, fmt, args);
    va_end(args);
}

void Context::TextColored(Color color, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::TextColoredV(ToImVec4(color), fmt, args);
    va_end(args);
}

void Context::Separator() {
    ImGui::Separator();
}

void Context::SeparatorText(const char* label) {
    ImGui::SeparatorText(label);
}

void Context::Spacing() {
    ImGui::Spacing();
}

void Context::SameLine() {
    ImGui::SameLine();
}

void Context::Dummy(Size size) {
    ImGui::Dummy(ToImVec2(size));
}

void Context::Indent(float width) {
    ImGui::Indent(width);
}

void Context::Unindent(float width) {
    ImGui::Unindent(width);
}

void Context::PushItemWidth(float width) {
    ImGui::PushItemWidth(width);
}

void Context::PopItemWidth() {
    ImGui::PopItemWidth();
}

void Context::BeginDisabled(bool disabled) {
    ImGui::BeginDisabled(disabled);
}

void Context::EndDisabled() {
    ImGui::EndDisabled();
}

void Context::DrawLastItemBorder(Color color) {
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(min, max, ImGui::ColorConvertFloat4ToU32(ToImVec4(color)));
}

Size Context::GetCursorScreenPos() const {
    return ToSize(ImGui::GetCursorScreenPos());
}

Size Context::GetContentRegionAvail() const {
    return ToSize(ImGui::GetContentRegionAvail());
}

Size Context::GetMousePos() const {
    return ToSize(ImGui::GetMousePos());
}

Size Context::GetMouseDelta() const {
    return ToSize(ImGui::GetIO().MouseDelta);
}

float Context::GetMouseWheel() const {
    return ImGui::GetIO().MouseWheel;
}

bool Context::IsShiftDown() const {
    return ImGui::GetIO().KeyShift;
}

bool Context::IsWindowHovered() const {
    return ImGui::IsWindowHovered();
}

bool Context::IsWindowFocusedChildWindows() const {
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
}

bool Context::IsMouseDragging(EMouseButton button) const {
    return ImGui::IsMouseDragging(ToImGuiMouseButton(button));
}

bool Context::IsMouseHoveringRect(Size min, Size max) const {
    return ImGui::IsMouseHoveringRect(ToImVec2(min), ToImVec2(max));
}

bool Context::IsMouseClicked(EMouseButton button) const {
    return ImGui::IsMouseClicked(ToImGuiMouseButton(button));
}

bool Context::IsKeyDown(EKey key) const {
    return ImGui::IsKeyDown(ToImGuiKey(key));
}

bool Context::IsKeyPressed(EKey key) const {
    return ImGui::IsKeyPressed(ToImGuiKey(key));
}

float Context::GetFontSize() const {
    return ImGui::GetFontSize();
}

Size Context::CalcTextSize(const char* text) const {
    return ToSize(ImGui::CalcTextSize(text));
}

void Context::OpenPopup(const char* id) {
    ImGui::OpenPopup(id);
}

bool Context::BeginPopup(const char* id) {
    return ImGui::BeginPopup(id);
}

void Context::EndPopup() {
    ImGui::EndPopup();
}

void Context::BeginTooltip() {
    ImGui::BeginTooltip();
}

void Context::EndTooltip() {
    ImGui::EndTooltip();
}

void Context::DrawLine(Size from, Size to, uint32_t color, float thickness) {
    ImGui::GetWindowDrawList()->AddLine(ToImVec2(from), ToImVec2(to), color, thickness);
}

void Context::DrawRect(Size min, Size max, uint32_t color, float rounding, float thickness) {
    ImGui::GetWindowDrawList()->AddRect(ToImVec2(min), ToImVec2(max), color, rounding, 0, thickness);
}

void Context::DrawRectFilled(Size min, Size max, uint32_t color, float rounding) {
    ImGui::GetWindowDrawList()->AddRectFilled(ToImVec2(min), ToImVec2(max), color, rounding);
}

void Context::DrawCircleFilled(Size center, float radius, uint32_t color, int segments) {
    ImGui::GetWindowDrawList()->AddCircleFilled(ToImVec2(center), radius, color, segments);
}

void Context::DrawCanvasText(Size pos, uint32_t color, const char* text) {
    ImGui::GetWindowDrawList()->AddText(ToImVec2(pos), color, text);
}

void Context::DrawTextClipped(Size pos, uint32_t color, const char* text, float font_size, Size clip_min, Size clip_max) {
    const ImVec4 clip = {clip_min.x, clip_min.y, clip_max.x, clip_max.y};
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(),
        font_size,
        ToImVec2(pos),
        color,
        text,
        nullptr,
        0.0f,
        &clip
    );
}

const char* Context::GetIconGlyph(EIcon icon) const {
    switch (icon) {
        case EIcon::Play:
            return ICON_FA_PLAY;
        case EIcon::Stop:
            return ICON_FA_STOP;
        case EIcon::FolderOpen:
            return ICON_FA_FOLDER_OPEN;
        case EIcon::Settings:
            return ICON_FA_GEAR;
        case EIcon::Warning:
            return ICON_FA_TRIANGLE_EXCLAMATION;
        case EIcon::Search:
            return ICON_FA_MAGNIFYING_GLASS;
        case EIcon::Save:
            return ICON_FA_FLOPPY_DISK;
        case EIcon::Reload:
            return ICON_FA_ROTATE_RIGHT;
        case EIcon::Eye:
            return ICON_FA_EYE;
        case EIcon::Camera:
            return ICON_FA_CAMERA;
        case EIcon::ChevronRight:
            return ICON_FA_CHEVRON_RIGHT;
        default:
            return "?";
    }
}

void Context::SetupDefaultDockLayout(const DockspaceDesc& desc, unsigned int dockspace_id) {
    ImGuiDockNode* dockspace_node = ImGui::DockBuilderGetNode(dockspace_id);
    if (!dockspace_node || m_default_dock_layout_applied) {
        return;
    }
    m_default_dock_layout_applied = true;

    ImGuiID main_id = dockspace_id;
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID right_id = ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Right, 0.28f, nullptr, &main_id);
    ImGuiID config_id = right_id;
    ImGuiID console_id = ImGui::DockBuilderSplitNode(right_id, ImGuiDir_Down, 0.34f, nullptr, &config_id);
    ImGuiID profiler_id = 0;
    if (desc.enable_profiler) {
        profiler_id = ImGui::DockBuilderSplitNode(config_id, ImGuiDir_Down, 0.45f, nullptr, &config_id);
    }

    ImGui::DockBuilderDockWindow(desc.scene_panel, main_id);
    ImGui::DockBuilderDockWindow(desc.config_panel, config_id);
    if (profiler_id != 0) {
        ImGui::DockBuilderDockWindow(desc.profiler_panel, profiler_id);
    }
    ImGui::DockBuilderDockWindow(desc.console_panel, console_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

void Context::TextV(void (*text_func)(const char*, va_list), const char* fmt, va_list args) {
    text_func(fmt, args);
}

} // namespace Moer::Synapse