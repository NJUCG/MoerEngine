#include "MainWindow.h"
#include "RendererManager.h"
#include "math/Math.h"
#include "Core.h"

#include "imgui.h"
#include "misc/Timer.h"
#include "rhi/RHICommon.h"
void StyleColorsDark(ImGuiStyle* dst = nullptr) {
    auto& colors = ImGui::GetStyle().Colors;

    colors[ImGuiCol_Text]                  = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.01f, 0.01f, 0.01f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.11f, 0.11f, 0.14f, 0.92f);
    colors[ImGuiCol_Border]                = ImVec4(0.08f, 0.07f, 0.10f, 0.50f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.05f, 0.05f, 0.05f, 0.39f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.15f, 0.15f, 0.15f, 0.39f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.36f, 0.36f, 0.36f, 0.39f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.01f, 0.00f, 0.03f, 0.83f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.02f, 0.01f, 0.06f, 0.83f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.02f, 0.01f, 0.05f, 0.83f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.00f, 0.00f, 0.02f, 0.80f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.05f, 0.04f, 0.08f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.43f, 0.26f, 0.73f, 0.60f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.55f, 0.39f, 0.81f, 0.60f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.69f, 0.60f, 0.82f, 0.60f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.91f, 0.76f, 0.09f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.41f, 0.39f, 0.80f, 0.60f);
    colors[ImGuiCol_Button]                = ImVec4(0.18f, 0.04f, 0.39f, 0.62f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.27f, 0.13f, 0.49f, 0.62f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.45f, 0.25f, 0.75f, 0.62f);
    colors[ImGuiCol_Header]                = ImVec4(0.05f, 0.03f, 0.12f, 0.62f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.10f, 0.06f, 0.22f, 0.62f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.12f, 0.06f, 0.30f, 0.62f);
    colors[ImGuiCol_Separator]             = ImVec4(0.13f, 0.11f, 0.17f, 0.68f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.17f, 0.16f, 0.21f, 0.68f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.22f, 0.21f, 0.25f, 0.68f);
    colors[ImGuiCol_ResizeGrip]            = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.78f, 0.82f, 1.00f, 0.60f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.78f, 0.82f, 1.00f, 0.90f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.09f, 0.04f, 0.14f, 0.45f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.13f, 0.08f, 0.18f, 0.45f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.20f, 0.16f, 0.25f, 0.45f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.14f, 0.07f, 0.42f, 0.45f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.20f, 0.13f, 0.45f, 0.45f);
    colors[ImGuiCol_DockingPreview]        = ImVec4(0.52f, 0.40f, 0.90f, 0.31f);
    colors[ImGuiCol_DockingEmptyBg]        = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_PlotLines]             = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]      = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogram]         = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.15f, 0.07f, 0.42f, 0.45f);
    colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.09f, 0.04f, 0.30f, 0.45f);
    colors[ImGuiCol_TableBorderLight]      = ImVec4(0.18f, 0.14f, 0.34f, 0.45f);
    colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.07f);
    colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.00f, 0.00f, 1.00f, 0.35f);
    colors[ImGuiCol_DragDropTarget]        = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight]          = ImVec4(0.45f, 0.45f, 0.90f, 0.80f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);

    auto& style             = ImGui::GetStyle();
    style.TabRounding       = 4;
    style.ScrollbarRounding = 9;
    style.WindowRounding    = 7;
    style.GrabRounding      = 3;
    style.FrameRounding     = 3;
    style.PopupRounding     = 4;
    style.ChildRounding     = 4;
}
bool ShowStyleSelector(const char* label) {
    static int style_idx = 0;
    if (ImGui::Combo(label, &style_idx, "Default\0Light\0Classic\0")) {
        switch (style_idx) {
            case 0: StyleColorsDark(); break;
            case 1: ImGui::StyleColorsLight(); break;
            case 2: ImGui::StyleColorsClassic(); break;
        }
        return true;
    }
    return false;
}

bool ShowResolutionSelector(const char* label, Extent2D& values) {
    static int style_idx = 0;
    if (ImGui::Combo(label, &style_idx, "res_1080p\0res_2k\0res_4k\0")) {
        switch (style_idx) {
            case 0: values = {1920, 1080}; break;
            case 1: values = {2560, 1440}; break;
            case 2: values = {3840, 2160}; break;
        }
        return true;
    }
    return false;
}

Moer::Timer timer;

void MainWindow::Show() {

    ImGuiIO& io = ImGui::GetIO();

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;
    // ImGuiWindowFlags_NoBackground;

    const ImGuiViewport* main_viewport = ImGui::GetMainViewport();

    if (!b_show)
        return;

    if (!ImGui::Begin("Moer Engine", &b_show, window_flags)) {
        ImGui::End();
        return;
    }
    static uint32_t frame_timer                 = 0;
    static float    frame_time                  = 0.1f;
    static float    frame_rate                  = 0.1f;
    static uint32_t frame_timer_update_interval = 1;
    if (frame_timer++ % frame_timer_update_interval == 0) {
        frame_time = timer.ElapsedMilliseconds();
        frame_rate = 1000.0f / frame_time;
    }
    frame_timer_update_interval = std::max(1u, uint32_t(frame_rate / 2.f));
    if (timer.IsRunning()) {

        timer.Stop();
        ImGui::Text("Time: %.3f ms/frame (%.1f FPS)", frame_time, frame_rate);
    }

    timer.Start();
    Moer::Vector2f render_target_window_pos  = {0.0f, 0.0f};
    Moer::Vector2f render_target_window_size = {0.0f, 0.0f};

    static bool b_first_time = true;
    if (b_first_time) {
        StyleColorsDark();
        b_first_time = false;
    }
    ShowStyleSelector("Colors##Default");
    static Extent2D values = {1920, 1080};

    ShowResolutionSelector("Resolution", values);

    auto& render_manager = Moer::RendererManager::GetInstance();
    auto  renderer_id    = render_manager.GetRendererID(MOER_DEFAULT_RENDERER_NAME);
    auto  output         = render_manager.GetRendererOutput(renderer_id);
    render_manager.SetRendererPresentResolution(renderer_id, values.x, values.y);
    float display_width  = ImGui::GetWindowWidth();
    float display_height = display_width * 9.f / 16.f;
    ImGui::Image(output,
                 {display_width, display_height});
    ImGui::ShowStyleEditor();

    // ImGui::Image()
    ImGui::End();
}