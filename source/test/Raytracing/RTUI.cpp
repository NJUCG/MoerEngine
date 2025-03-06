#include "RTUI.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "math/Function.h"
#include "shaderheaders/shared/ShaderParameters.h"
namespace Moer::Render {
    enum ELocalLightSelectionMode {
        ELLS_Uniform   = s_di_local_light_sample_mode_uniform,
        ELLS_Power_RIS = s_di_local_light_sample_mode_power_ris,
        ELLS_Grid      = s_di_local_light_sample_mode_grid,
        ELLS_Num
    };

    enum EGridLightPresampleMode {
        EGLPM_Uniform   = s_di_local_light_sample_mode_uniform,
        EGLPM_Power_RIS = s_di_local_light_sample_mode_power_ris,
        EGLPM_Num
    };

    enum EBiasCorrectionMode {
        EBCM_None      = s_di_bias_correction_none,
        EBCM_Basic     = s_di_bias_correction_basic,
        EBCM_Pair_Wise = s_di_bias_correction_pair_wise,
        EBCM_Traced    = s_di_bias_correction_traced,
        EBCM_Num
    };

    static constexpr std::string_view s_local_light_sample_mode_names[] = {
        "Uniform",
        "Power RIS",
        "Grid"};

    static constexpr std::string_view s_grid_light_presample_mode_names[] = {
        "Uniform",
        "Power RIS"};

    static constexpr std::string_view s_bias_correction_mode_names[] = {
        "None",
        "Basic",
        "Pair Wise",
        "Traced"};

    static constexpr std::string_view s_denoiser_mode_names[] = {
        "None",
        "ReBlur",
        "Relax"};

    static constexpr std::string_view s_aa_mode_names[] = {
        "TAA"};

    static constexpr std::string_view s_final_color_names[] = {
        "SceneColor"
        "DI",
        "Emissive",
        "Diffuse",
        "Specular",
        "Normal",
        "ViewDepth",
        "Depth",
        "Motion",
        "Grid",
        "Material",
        "Position"};

    void StyleColorsDark(ImGuiStyle* _dst = nullptr) {
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
    bool ShowStyleSelector(const char* _label) {
        static int style_idx = 0;
        if (ImGui::Combo(_label, &style_idx, "Default\0Light\0Classic\0")) {
            switch (style_idx) {
                case 0: StyleColorsDark(); break;
                case 1: ImGui::StyleColorsLight(); break;
                case 2: ImGui::StyleColorsClassic(); break;
            }
            return true;
        }
        return false;
    }

    RTUI::RTUI(UIRenderer& _renderer)
        : ui_renderer(_renderer) {

        final_color_map["SceneColor"] = EFinalColor::EFC_SceneColor;
        final_color_map["DI"]         = EFinalColor::EFC_DI;
        final_color_map["Emissive"]   = EFinalColor::EFC_EMISSIVE;
        final_color_map["Diffuse"]    = EFinalColor::EFC_DIFFUSE;
        final_color_map["Specular"]   = EFinalColor::EFC_SPECULAR;
        final_color_map["Normal"]     = EFinalColor::EFC_NORMAL;
        final_color_map["ViewDepth"]  = EFinalColor::EFC_VIEW_DEPTH;
        final_color_map["Depth"]      = EFinalColor::EFC_DEPTH;
        final_color_map["Motion"]     = EFinalColor::EFC_MOTION;
        final_color_map["Grid"]       = EFinalColor::EFC_GRID;
        final_color_map["Material"]   = EFinalColor::EFC_MATERIAL;
        final_color_map["Position"]   = EFinalColor::EFC_POSITION;

        StyleColorsDark();
    }
    void RTUI::TickUI() {

        static bool               opt_fullscreen  = true;
        static bool               opt_padding     = false;
        static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;
        // ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());
        // We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
        // because it would be confusing to have two docking targets within each others.
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar;
        if (opt_fullscreen) {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
            window_flags |= ImGuiWindowFlags_NoBackground;
        } else {
            dockspace_flags &= ~ImGuiDockNodeFlags_PassthruCentralNode;
        }

        // When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
        // and handle the pass-thru hole, so we ask Begin() to not render a background.
        if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
            window_flags |= ImGuiWindowFlags_NoBackground;

        // Important: note that we proceed even if Begin() returns false (aka window is collapsed).
        // This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
        // all active windows docked into it will lose their parent and become undocked.
        // We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
        // any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
        if (!opt_padding)
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Editor Menu", &b_show, window_flags);
        if (!opt_padding)
            ImGui::PopStyleVar();

        if (opt_fullscreen)
            ImGui::PopStyleVar(2);

        // Submit the DockSpace
        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
            ImGuiID dockspace_id = ImGui::GetID("Docking Main");
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
        }
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Menu")) {
                // if (ImGui::MenuItem("Reload Current Level")) {
                // }
                // if (ImGui::MenuItem("Save Current Level")) {
                // }
                if (ImGui::MenuItem("Exit")) {
                    exit(0);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Window")) {

                ImGui::MenuItem("Scene Color", nullptr, &b_show_scene_color);
                ImGui::MenuItem("Configs", nullptr, &b_show_config);
                // ImGui::MenuItem("Inspector", nullptr, &m_b_show_inspector_window);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        ImGui::End();

        ShowSceneColor();
        ShowConfig();
    }

    void RTUI::ShowSceneColor() {
        ImGuiIO&         io           = ImGui::GetIO();
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;

        const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
        if (!b_show_scene_color) {
            return;
        }
        if (!ImGui::Begin("Scene Color", &b_show_scene_color, window_flags)) {

            ImGui::End();
            return;
        }
        float2 scene_size = {0, 0};

        static float2 xy_ratio = {16, 9};
        // auto          menu_rect = ImGui::GetCurrentWindow()->MenuBarRect();

        auto* current_window    = ImGui::FindWindowByName("Scene Color");
        bool  b_separate_window = current_window->ParentWindow == nullptr;
        auto  menu_rect         = current_window->MenuBarRect();

        scene_size.x = current_window->Size.x;
        scene_size.y = current_window->Size.y + current_window->Pos.y - menu_rect.Max.y;

        auto   window_rect = current_window->Rect();// this is main window rect
        ImRect parent_rect{};

        if (b_separate_window) {

            parent_rect = {current_window->Pos.x, current_window->Pos.y, current_window->Pos.x + current_window->Size.x, current_window->Pos.y + current_window->Size.y};

            float2 local_pos = {window_rect.Min.x - parent_rect.Min.x, menu_rect.Max.y - parent_rect.Min.y};

            scene_color_resolution = {scene_size.x, scene_size.y};
            scene_color_pos        = {local_pos.x, local_pos.y};
        } else {
            parent_rect = current_window->ParentWindow->Rect();

            float2 local_pos = {window_rect.Min.x - parent_rect.Min.x, menu_rect.Max.y - parent_rect.Min.y};

            scene_color_resolution = {scene_size.x, scene_size.y};
            scene_color_pos        = {local_pos.x, local_pos.y};
        }

        ImGui::End();
    }

    void RTUI::ShowConfig() {
        ImGuiIO&         io           = ImGui::GetIO();
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;

        if (!b_show_config) {
            return;
        }
        if (!ImGui::Begin("Configs", &b_show_config, window_flags)) {

            ImGui::End();
            return;
        }

        ShowStyleSelector("Colors##Default");

        if (ImGui::TreeNode("Final Color")) {
            for (auto& [name, index] : final_color_map) {
                if (ImGui::Selectable(name.c_str(), config.final_color == index)) {
                    config.final_color = static_cast<EFinalColor>(index);
                }
            }

            ImGui::TreePop();
        }

        //Grid Config
        if (ImGui::TreeNode("Grid Config")) {

            if (ImGui::TreeNode("Presample Mode")) {
                for (auto& name : s_grid_light_presample_mode_names) {
                    uint idx = &name - s_grid_light_presample_mode_names;
                    if (ImGui::Selectable(name.data(), config.grid_config.grid_mode == idx)) {
                        config.grid_config.grid_mode = idx;
                    }
                }
                ImGui::TreePop();
            }
            ImGui::SliderInt("Light Per Ceil", &config.grid_config.light_per_ceil, 1, 1024);
            ImGui::SliderFloat("Cell Size", &config.grid_config.cell_size, 1.f, 400.f);
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("ReSTIRDI")) {
            if (ImGui::TreeNode("InitialSampleSettings")) {
                if (ImGui::TreeNode("LocalLightSelection")) {
                    for (auto& name : s_local_light_sample_mode_names) {
                        uint idx = &name - s_local_light_sample_mode_names;
                        if (ImGui::Selectable(name.data(), config.restir_di_cfg.initial_sample_config.local_light_sample_mode == idx)) {
                            config.restir_di_cfg.initial_sample_config.local_light_sample_mode = idx;
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("TemporalResampleSettings")) {
                if (ImGui::TreeNode("BiasCorrection")) {
                    for (auto& name : s_bias_correction_mode_names) {
                        uint idx = &name - s_bias_correction_mode_names;
                        if (ImGui::Selectable(name.data(), config.restir_di_cfg.temporal_resample_config.bias_correction == idx)) {
                            config.restir_di_cfg.temporal_resample_config.bias_correction = idx;
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("SpatialResampleSettings")) {
                if (ImGui::TreeNode("BiasCorrection")) {
                    for (auto& name : s_bias_correction_mode_names) {
                        uint idx = &name - s_bias_correction_mode_names;
                        if (ImGui::Selectable(name.data(), config.restir_di_cfg.spatial_resample_config.bias_correction == idx)) {
                            config.restir_di_cfg.spatial_resample_config.bias_correction = idx;
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }

            ImGui::TreePop();
        }
        if (ImGui::TreeNode("DenoiserConfig")) {
            for (auto& name : s_denoiser_mode_names) {
                uint idx = &name - s_denoiser_mode_names;
                if (ImGui::Selectable(name.data(), config.denoiser_cfg.denoiser_type == idx)) {
                    config.denoiser_cfg.denoiser_type = idx;
                }
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("ToneMapping")) {
            ImGui::SliderFloat("Histogram Low Percentile", &config.tone_mapping_cfg.histogram_low_percentile, 0.0f, 1.0f);
            ImGui::SliderFloat("Histogram High Percentile", &config.tone_mapping_cfg.histogram_high_percentile, 0.0f, 1.0f);
            ImGui::SliderFloat("Eye Adaptation Speed Up", &config.tone_mapping_cfg.eye_adaptation_speed_up, 0.0f, 10.0f);
            ImGui::SliderFloat("Eye Adaptation Speed Down", &config.tone_mapping_cfg.eye_adaptation_speed_down, 0.0f, 10.0f);
            ImGui::SliderFloat("Min Adapted Luminance", &config.tone_mapping_cfg.min_adapted_luminance, 0.0f, 10.0f);
            ImGui::SliderFloat("Max Adapted Luminance", &config.tone_mapping_cfg.max_adapted_luminance, 0.0f, 10.0f);
            ImGui::SliderFloat("Exposure Bias", &config.tone_mapping_cfg.exposure_bias, -10.0f, 10.0f);
            ImGui::SliderFloat("WhitePoint", &config.tone_mapping_cfg.white_point, 0.0f, 10.0f);
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("AntiAlias")) {
            for (uint i = 0; i < EAnitiAliasMode::EAA_Num; i++) {
                if (ImGui::Selectable(s_aa_mode_names[i].data(), config.aa_cfg.aa_mode == i)) {
                    config.aa_cfg.aa_mode = (EAnitiAliasMode)i;
                }
            }
            ImGui::SliderFloat("New Frame Weight", &config.aa_cfg.new_frame_weight, 0.0f, 1.0f);
            ImGui::SliderFloat("Clamping Factor", &config.aa_cfg.clamping_factor, 0.0f, 10.0f);
            ImGui::SliderFloat("Max Radiance", &config.aa_cfg.max_radiance, 0.0f, 10000.0f);
            ImGui::Checkbox("Enable History Clamping", &config.aa_cfg.enable_history_clamping);
            ImGui::TreePop();
        }
        config.sun_direction = Normalizef(config.sun_direction);
        ImGui::SliderFloat3("Sun Direction", &config.sun_direction.x, -1.0f, 1.0f);
        ImGui::SliderFloat("Exposure", &config.exposure, 0.0f, 10.0f);
        ImGui::SliderFloat("Sun Angular Diameter", &config.sun_angular_diameter, 0.0f, 1.0f);

        int max_bounce = config.max_bounce;
        ImGui::SliderInt("Max Bounce", &max_bounce, 1, 5);
        config.max_bounce = max_bounce;
        //show fps
        ImGui::Text("FPS: %.1f", io.Framerate);
        ImGui::Text("Frame Time: %.1f ms", 1000.0f / io.Framerate);

        ImGui::End();
    }

    bool RTUI::IsSeperateWindow() const {
        auto* current_window = ImGui::FindWindowByName("Scene Color");

        return current_window->ParentWindow == nullptr;
    }

    TextureView RTUI::GetWindowFrameBuffer() {
        auto* current_window = ImGui::FindWindowByName("Scene Color");
        if (current_window->ParentWindow == nullptr) {
            return ui_renderer.GetWindowFrameBuffer(current_window->Viewport);
        }
        return TextureView();
    }
}// namespace Moer::Render