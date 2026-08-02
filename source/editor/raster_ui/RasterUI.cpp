#include "RasterUI.h"

// 提供 Raster 管线的功能控制项和轻量级运行时诊断信息。

#include "config/ConfigManager.h"

#include <imgui.h>
#include <string_view>

namespace Moer {

RasterUI::RasterUI(RasterConfig& config) : m_cooperative_ops_ui(config), m_config(config) {
    if (ConfigManager::GetInstance().GetConfig().engine.render.raster.low_quality_mode) {
        m_config.ao_mode = EAoMode::SSAO;

        m_config.shadow_map_mode                     = EShadowMapMode::CSM;
        m_config.shadow_csm_num_of_cascades          = 1;
        m_config.shadow_csm_sm_size                  = 2048;
        m_config.shadow_csm_cover_ratio_of_camera[0] = 0.03f;

        m_config.aa_mode = EAaMode::FXAA_SIMPLIFIED;
    }

    if (!ConfigManager::GetInstance().GetConfig().engine.render.raster.enable_shadow) {
        m_config.shadow_map_mode = EShadowMapMode::NONE;
    }
}

void RasterUI::ShowConfig() {
    if (!ImGui::TreeNode("Raster Settings")) {
        return;
    }

    const auto draw_border = []() {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(min, max, IM_COL32(255, 255, 255, 255));
    };

    if (!m_frame_buffer_names.empty() &&
        ImGui::TreeNode(
            "Output Frame Buffer",
            "Output: [%s]",
            m_frame_buffer_names[m_config.selected_frame_buffer_index].c_str()
        )) {
        for (uint i = 0; i < m_frame_buffer_names.size(); i++) {
            if (ImGui::Selectable(
                    m_frame_buffer_names[i].c_str(),
                    m_config.selected_frame_buffer_index == i
                )) {
                m_config.selected_frame_buffer_index = i;
            }
            draw_border();
        }
        ImGui::TreePop();
    }

    // MARK: Geometry & Culling
    if (ImGui::TreeNode("Geometry & Culling")) {

        const auto toggle_button = [](const char* label, bool& value) {
            ImGui::PushStyleColor(
                ImGuiCol_Button, value ? IM_COL32(70, 130, 95, 255) : IM_COL32(65, 65, 65, 255)
            );
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered, value ? IM_COL32(85, 155, 115, 255) : IM_COL32(85, 85, 85, 255)
            );
            ImGui::PushStyleColor(
                ImGuiCol_ButtonActive, value ? IM_COL32(55, 105, 80, 255) : IM_COL32(45, 45, 45, 255)
            );
            if (ImGui::Button(label, ImVec2(120.0f, 0.0f))) {
                value = !value;
            }
            ImGui::PopStyleColor(3);
        };

        toggle_button("Frustum Cull", m_config.enable_frustum_culling);

        toggle_button("Occlusion Cull", m_config.enable_occlusion_culling);

        ImGui::SliderFloat("LOD Threshold (px)", &m_config.cluster_lod_error_threshold, 0.1f, 16.0f);

        ImGui::SliderInt("Force LOD Level", &m_config.force_lod_level, -1, 8, 
            m_config.force_lod_level < 0 ? "Auto" : "%d");

        static constexpr const char* debug_visualization_names[] = {
            "Off", "Cluster ID", "frac(UV)", "Vertex Normal"
        };
        ImGui::Combo(
            "Debug Visualization",
            &m_config.geometry_debug_visualization,
            debug_visualization_names,
            4
        );

        // Culling Statistics
        {
            ImGui::Separator();
            ImGui::Text("Culling Statistics:");
            ImGui::Indent();

            if (m_config.culling_stats.total_instances_before == 0 &&
                m_config.culling_stats.lod_culled_instances == 0) {
                ImGui::TextDisabled("  Waiting for data...");
            } else {
                const auto& stats = m_config.culling_stats;

                // LOD 信息（独立展示，不混入剔除条形图）
                const uint lod_active = stats.total_instances_before;
                const uint lod_total  = lod_active + stats.lod_culled_instances;
                ImGui::TextColored(ImVec4(0.51f, 0.39f, 0.82f, 1.0f),
                    "LOD active: %u / %u primitives", lod_active, lod_total);

                // 几何剔除统计（基于 LOD active 的 instance）
                ImGui::Text(
                    "Culling: %u / %u visible\n(frustum %u, occlusion %u)",
                    stats.total_instances_after,
                    stats.total_instances_before,
                    stats.frustum_culled_instances,
                    stats.occlusion_culled_instances
                );

                ImVec2 bar_size(ImGui::GetContentRegionAvail().x, 18.0f);
                bar_size.x = bar_size.x < 150.0f ? 150.0f : bar_size.x;

                ImVec2      bar_min = ImGui::GetCursorScreenPos();
                ImVec2      bar_max(bar_min.x + bar_size.x, bar_min.y + bar_size.y);
                ImDrawList* draw_list = ImGui::GetWindowDrawList();

                ImGui::InvisibleButton("##CullingRatioBar", bar_size);

                const float total           = float(stats.total_instances_before);
                const float frustum_width   = total > 0 ? bar_size.x * float(stats.frustum_culled_instances) / total : 0;
                const float occlusion_width = total > 0 ? bar_size.x * float(stats.occlusion_culled_instances) / total : 0;
                float       rendered_width  = bar_size.x - frustum_width - occlusion_width;
                if (rendered_width < 0.0f) {
                    rendered_width = 0.0f;
                }

                const ImU32 frustum_color   = IM_COL32(220, 95, 80, 255);
                const ImU32 occlusion_color = IM_COL32(225, 170, 75, 255);
                const ImU32 rendered_color  = IM_COL32(80, 155, 120, 255);

                float cursor_x = bar_min.x;
                draw_list->AddRectFilled(
                    ImVec2(cursor_x, bar_min.y),
                    ImVec2(cursor_x + frustum_width, bar_max.y),
                    frustum_color,
                    0.0f
                );
                cursor_x += frustum_width;
                draw_list->AddRectFilled(
                    ImVec2(cursor_x, bar_min.y),
                    ImVec2(cursor_x + occlusion_width, bar_max.y),
                    occlusion_color,
                    0.0f
                );
                cursor_x += occlusion_width;
                draw_list->AddRectFilled(
                    ImVec2(cursor_x, bar_min.y),
                    ImVec2(cursor_x + rendered_width, bar_max.y),
                    rendered_color,
                    0.0f
                );
                draw_list->AddRect(bar_min, bar_max, IM_COL32(255, 255, 255, 90), 3.0f);

                ImGui::PushStyleColor(ImGuiCol_Text, frustum_color);
                ImGui::TextUnformatted("Frustum");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, occlusion_color);
                ImGui::TextUnformatted("Occlusion");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, rendered_color);
                ImGui::TextUnformatted("Rendered");
                ImGui::PopStyleColor();
            }
            ImGui::Unindent();
            ImGui::Separator();
        }

        ImGui::Checkbox("Enable Alpha Test", &m_config.geometry_enable_alpha_test);
        ImGui::SliderFloat("Alpha Cutoff", &m_config.geometry_alpha_test_blend_pixel_cutoff, 0.0f, 1.0f);

        ImGui::TreePop();
    }

    // MARK: Hardware Tessellation Showcase
    if (ImGui::TreeNode(
            "Tessellated Surface",
            "Tessellated Surface: [%s]",
            m_config.tessellated_surface_enabled ? "Enable" : "Disable"
        )) {
        ImGui::Checkbox("Enable Surface", &m_config.tessellated_surface_enabled);

        ImGui::BeginDisabled(!m_config.tessellated_surface_enabled);

        static constexpr const char* preset_names[] = {
            "Sand Dunes",
            "Snow Field"
        };
        ImGui::Combo(
            "Surface Preset",
            &m_config.tessellated_surface_preset,
            preset_names,
            2
        );

        static constexpr const char* debug_mode_names[] = {
            "Material",
            "Tess Factor Heatmap",
            "World Normal",
            "Height"
        };
        ImGui::Combo(
            "Surface Debug",
            &m_config.tessellated_surface_debug_mode,
            debug_mode_names,
            4
        );

        ImGui::DragFloat3(
            "Surface Center",
            reinterpret_cast<float*>(&m_config.tessellated_surface_center),
            0.05f
        );
        ImGui::SliderFloat(
            "Half Extent (m)",
            &m_config.tessellated_surface_half_extent,
            0.5f,
            40.0f
        );
        ImGui::SliderInt(
            "Base Grid",
            &m_config.tessellated_surface_grid_resolution,
            2,
            64,
            "%d cells/axis"
        );
        ImGui::SliderFloat(
            "Target Edge (px)",
            &m_config.tessellated_surface_target_edge_pixels,
            2.0f,
            32.0f
        );
        ImGui::SliderFloat(
            "Min Tess Factor",
            &m_config.tessellated_surface_min_tess_factor,
            2.0f,
            m_config.tessellated_surface_max_tess_factor
        );
        ImGui::SliderFloat(
            "Max Tess Factor",
            &m_config.tessellated_surface_max_tess_factor,
            m_config.tessellated_surface_min_tess_factor,
            64.0f
        );
        ImGui::SliderFloat(
            "Height Scale",
            &m_config.tessellated_surface_height_scale,
            0.0f,
            3.0f
        );
        ImGui::SliderFloat(
            "Detail Scale",
            &m_config.tessellated_surface_detail_scale,
            0.0f,
            3.0f
        );
        ImGui::SliderFloat(
            "Wind Angle",
            &m_config.tessellated_surface_wind_angle_degrees,
            -180.0f,
            180.0f,
            "%.0f deg"
        );

        if (ImGui::Button("Reset Surface Settings")) {
            m_config.tessellated_surface_preset             = 0;
            m_config.tessellated_surface_debug_mode         = 0;
            m_config.tessellated_surface_grid_resolution    = 16;
            m_config.tessellated_surface_center             = float3(0.0f, 0.04f, 0.0f);
            m_config.tessellated_surface_half_extent        = 8.0f;
            m_config.tessellated_surface_target_edge_pixels = 8.0f;
            m_config.tessellated_surface_min_tess_factor    = 2.0f;
            m_config.tessellated_surface_max_tess_factor    = 32.0f;
            m_config.tessellated_surface_height_scale       = 1.0f;
            m_config.tessellated_surface_detail_scale       = 1.0f;
            m_config.tessellated_surface_wind_angle_degrees = 18.0f;
        }

        ImGui::TextDisabled(
            "Triangle patches: VS -> HS -> DS -> PS. The showcase receives scene shadows but is not yet a CSM caster."
        );

        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    // MARK: Shading
    if (ImGui::TreeNode(
            "Shading", "Shading: [%s]", s_shading_mode_name_map.at(m_config.shading_mode).c_str()
        )) {

        // 当前仅开放一种着色模型，因此本区域直接配置其 BRDF。

        if (m_config.shading_mode == EShadingMode::DEFAULT_PBR) {
            ImGui::Text("BRDF Settings:");

            // 补偿单次散射微表面近似造成的能量损失。
            ImGui::Checkbox("MultiScatter (Kulla-Conty)", &m_config.shading_brdf_enable_multi_scatter);

            // Spacing
            ImGui::Dummy(ImVec2(0.0f, 5.0f));

            // NDF
            ImGui::Text("NDF:");
            for (uint i = 0; i < s_brdf_ndf_mode_name_map.size(); i++) {
                const auto cur_enum = static_cast<EBrdfNdfMode>(i);
                if (ImGui::Selectable(
                        s_brdf_ndf_mode_name_map.at(cur_enum).c_str(),
                        m_config.shading_brdf_NDF_mode == cur_enum
                    )) {
                    m_config.shading_brdf_NDF_mode = cur_enum;
                }
                draw_border();
            }

            // Spacing
            ImGui::Dummy(ImVec2(0.0f, 5.0f));

            // Geometry Function
            ImGui::Text("Geometry Function:");
            for (uint i = 0; i < s_brdf_geometry_mode_name_map.size(); i++) {
                const auto cur_enum = static_cast<EBrdfGMode>(i);
                if (ImGui::Selectable(
                        s_brdf_geometry_mode_name_map.at(cur_enum).c_str(),
                        m_config.shading_brdf_G_mode == cur_enum
                    )) {
                    m_config.shading_brdf_G_mode = cur_enum;
                }
                draw_border();
            }
            if (m_config.shading_brdf_G_mode == EBrdfGMode::G_SCHLICK) {
                // Schlick 几何近似在 IBL 中使用不同的重映射。
                ImGui::Checkbox("Light Source is IBL", &m_config.shading_brdf_G_is_ibl);
            }
            ImGui::Dummy(ImVec2(0.0f, 5.0f));
        }

        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 5.0f));

        ImGui::Checkbox("Enable Extra Ambient", &m_config.shading_enable_extra_ambient);
        ImGui::SliderFloat("Ambient Intensity", &m_config.shading_extra_ambient_intensity, 0.0f, 1.0f);
        ImGui::SliderFloat3("Ambient Color", (float*)&m_config.shading_extra_ambient_color, 0.0f, 1.0f);

        ImGui::TreePop();
    }

    // MARK: Probe GI
    if (ImGui::TreeNode("Probe GI", "Probe GI: [%s]", m_config.probe_gi_enabled ? "Enable" : "Disable")) {
        const auto clamp_probe_counts = [](ProbeVolumeConfig& volume) {
            const auto clamp_int = [](int value, int low, int high) {
                return value < low ? low : (value > high ? high : value);
            };

            volume.count_x = clamp_int(volume.count_x, 1, 16);
            volume.count_y = clamp_int(volume.count_y, 1, 16);
            volume.count_z = clamp_int(volume.count_z, 1, 16);

            const auto probe_count = [&]() {
                return volume.count_x * volume.count_y * volume.count_z;
            };

            while (probe_count() > static_cast<int>(Render::RASTER_PROBE_MAX_COUNT_PER_VOLUME)) {
                if (volume.count_z >= volume.count_x && volume.count_z >= volume.count_y && volume.count_z > 1) {
                    --volume.count_z;
                } else if (volume.count_x >= volume.count_y && volume.count_x > 1) {
                    --volume.count_x;
                } else if (volume.count_y > 1) {
                    --volume.count_y;
                } else {
                    break;
                }
            }
        };

        ImGui::Checkbox("Enable Probe GI", &m_config.probe_gi_enabled);
        ImGui::SliderInt(
            "Volume Count",
            &m_config.probe_gi_volume_count,
            1,
            static_cast<int>(Render::RASTER_PROBE_VOLUME_MAX_COUNT)
        );
        m_config.probe_gi_volume_count =
            Max(1, Min(m_config.probe_gi_volume_count, static_cast<int>(Render::RASTER_PROBE_VOLUME_MAX_COUNT)));
        m_config.probe_gi_selected_volume =
            Max(0, Min(m_config.probe_gi_selected_volume, m_config.probe_gi_volume_count - 1));
        ImGui::SliderInt(
            "Selected Volume",
            &m_config.probe_gi_selected_volume,
            0,
            m_config.probe_gi_volume_count - 1
        );

        ProbeVolumeConfig& selected_volume = m_config.probe_gi_volumes[m_config.probe_gi_selected_volume];
        ImGui::Checkbox("Volume Enabled", &selected_volume.enabled);
        ImGui::SliderInt("Probe Count X", &selected_volume.count_x, 1, 16);
        ImGui::SliderInt("Probe Count Y", &selected_volume.count_y, 1, 16);
        ImGui::SliderInt("Probe Count Z", &selected_volume.count_z, 1, 16);
        clamp_probe_counts(selected_volume);
        ImGui::Checkbox("Camera Clipmap", &selected_volume.camera_clipmap);
        if (selected_volume.camera_clipmap) {
            ImGui::Checkbox("Clipmap Follow Y", &selected_volume.clipmap_follow_y);
            ImGui::SliderFloat(
                "Clipmap Anchor Hysteresis",
                &m_config.probe_gi_clipmap_anchor_hysteresis,
                0.0f,
                0.49f
            );
        }

        int active_volume_count = 0;
        int probe_total = 0;
        for (int volume_index = 0; volume_index < m_config.probe_gi_volume_count; ++volume_index) {
            ProbeVolumeConfig& volume = m_config.probe_gi_volumes[volume_index];
            clamp_probe_counts(volume);
            if (volume.enabled) {
                ++active_volume_count;
                probe_total += volume.count_x * volume.count_y * volume.count_z;
            }
        }
        ImGui::Text(
            "Active Volumes: %d / %d, Probes: %d / %u",
            active_volume_count,
            m_config.probe_gi_volume_count,
            probe_total,
            Render::RASTER_PROBE_MAX_COUNT
        );
        ImGui::Text(
            "Brick Size: %u x %u x %u",
            Render::RASTER_PROBE_BRICK_DIM,
            Render::RASTER_PROBE_BRICK_DIM,
            Render::RASTER_PROBE_BRICK_DIM
        );
        ImGui::Text(
            "Cell Size: %u x %u x %u Bricks",
            Render::RASTER_PROBE_CELL_BRICK_DIM,
            Render::RASTER_PROBE_CELL_BRICK_DIM,
            Render::RASTER_PROBE_CELL_BRICK_DIM
        );
        const int selected_brick_count_x =
            (selected_volume.count_x + int(Render::RASTER_PROBE_BRICK_DIM) - 1) /
            int(Render::RASTER_PROBE_BRICK_DIM);
        const int selected_brick_count_y =
            (selected_volume.count_y + int(Render::RASTER_PROBE_BRICK_DIM) - 1) /
            int(Render::RASTER_PROBE_BRICK_DIM);
        const int selected_brick_count_z =
            (selected_volume.count_z + int(Render::RASTER_PROBE_BRICK_DIM) - 1) /
            int(Render::RASTER_PROBE_BRICK_DIM);
        const int selected_cell_count_x =
            (selected_brick_count_x + int(Render::RASTER_PROBE_CELL_BRICK_DIM) - 1) /
            int(Render::RASTER_PROBE_CELL_BRICK_DIM);
        const int selected_cell_count_y =
            (selected_brick_count_y + int(Render::RASTER_PROBE_CELL_BRICK_DIM) - 1) /
            int(Render::RASTER_PROBE_CELL_BRICK_DIM);
        const int selected_cell_count_z =
            (selected_brick_count_z + int(Render::RASTER_PROBE_CELL_BRICK_DIM) - 1) /
            int(Render::RASTER_PROBE_CELL_BRICK_DIM);
        ImGui::Text(
            "Selected APV Cell Grid: %d x %d x %d (%d)",
            selected_cell_count_x,
            selected_cell_count_y,
            selected_cell_count_z,
            selected_cell_count_x * selected_cell_count_y * selected_cell_count_z
        );
        ImGui::Checkbox(
            "Adaptive Hierarchy Sampling",
            &m_config.probe_gi_adaptive_hierarchy_enabled
        );
        if (m_config.probe_gi_adaptive_hierarchy_enabled) {
            ImGui::SliderFloat(
                "Hierarchy Transition Width",
                &m_config.probe_gi_adaptive_transition_width,
                0.0f,
                4.0f
            );
        }
        ImGui::Checkbox("Adaptive Placement Analysis", &m_config.probe_gi_adaptive_placement_enabled);
        if (m_config.probe_gi_adaptive_placement_enabled) {
            ImGui::SliderFloat(
                "Geometry Padding",
                &m_config.probe_gi_adaptive_geometry_padding,
                0.0f,
                4.0f
            );
            ImGui::SliderFloat(
                "Fine Occupancy Threshold",
                &m_config.probe_gi_adaptive_fine_occupancy,
                1.0f / float(Render::RASTER_PROBE_OCCUPANCY_VOXEL_COUNT),
                1.0f
            );
            ImGui::SliderInt(
                "Fine Primitive Threshold",
                &m_config.probe_gi_adaptive_fine_primitives,
                1,
                512
            );
        }
        ImGui::SliderInt(
            "Physical Probe Capacity",
            &m_config.probe_gi_physical_probe_capacity,
            static_cast<int>(
                Render::RASTER_PROBE_BRICK_DIM * Render::RASTER_PROBE_BRICK_DIM * Render::RASTER_PROBE_BRICK_DIM
            ),
            static_cast<int>(Render::RASTER_PROBE_MAX_COUNT)
        );
        ImGui::Checkbox("Streaming Residency", &m_config.probe_gi_streaming_enabled);
        if (m_config.probe_gi_streaming_enabled) {
            ImGui::SliderInt(
                "Brick Load Budget",
                &m_config.probe_gi_streaming_load_budget,
                1,
                static_cast<int>(Render::RASTER_PROBE_MAX_BRICK_COUNT)
            );
            ImGui::SliderInt(
                "Brick Eviction Budget",
                &m_config.probe_gi_streaming_eviction_budget,
                1,
                static_cast<int>(Render::RASTER_PROBE_MAX_BRICK_COUNT)
            );
        }
        ImGui::Checkbox("Sparse Brick Residency", &m_config.probe_gi_sparse_bricks_enabled);
        if (m_config.probe_gi_sparse_bricks_enabled) {
            ImGui::SliderFloat(
                "Brick Resident Distance",
                &m_config.probe_gi_brick_resident_distance,
                0.5f,
                128.0f
            );
            ImGui::SliderFloat(
                "Brick Resident Hysteresis",
                &m_config.probe_gi_brick_resident_hysteresis,
                0.0f,
                16.0f
            );
            ImGui::Checkbox("Motion Prefetch", &m_config.probe_gi_motion_prefetch_enabled);
            if (m_config.probe_gi_motion_prefetch_enabled) {
                ImGui::SliderFloat(
                    "Prefetch Motion Threshold",
                    &m_config.probe_gi_motion_prefetch_threshold,
                    0.0f,
                    2.0f
                );
                ImGui::SliderInt(
                    "Prefetch Keep Frames",
                    &m_config.probe_gi_motion_prefetch_keep_frames,
                    1,
                    120
                );
            }
        }
        ImGui::Checkbox("Update Scheduler", &m_config.probe_gi_update_scheduler_enabled);
        if (m_config.probe_gi_update_scheduler_enabled) {
            ImGui::SliderInt(
                "Brick Update Budget",
                &m_config.probe_gi_update_brick_budget,
                1,
                static_cast<int>(Render::RASTER_PROBE_MAX_BRICK_COUNT)
            );
        }
        ImGui::Checkbox("Dirty Tracking", &m_config.probe_gi_dirty_tracking_enabled);
        if (m_config.probe_gi_dirty_tracking_enabled) {
            ImGui::SliderFloat(
                "Dirty Influence Scale",
                &m_config.probe_gi_dirty_influence_scale,
                0.0f,
                1.0f
            );
        }

        const auto sanitize_volume_size = [](float3 value) {
            return float3(
                value.x < 0.1f ? 0.1f : value.x,
                value.y < 0.1f ? 0.1f : value.y,
                value.z < 0.1f ? 0.1f : value.z
            );
        };

        float3 volume_size = sanitize_volume_size(selected_volume.extent);
        float3 volume_center = float3(
            selected_volume.origin.x + volume_size.x * 0.5f,
            selected_volume.origin.y + volume_size.y * 0.5f,
            selected_volume.origin.z + volume_size.z * 0.5f
        );

        const bool center_changed = ImGui::SliderFloat3("Volume Center", (float*)&volume_center, -64.0f, 64.0f);
        const bool size_changed = ImGui::SliderFloat3("Volume Size", (float*)&volume_size, 0.5f, 128.0f);
        if (center_changed || size_changed) {
            volume_size = sanitize_volume_size(volume_size);
            selected_volume.extent = volume_size;
            selected_volume.origin = float3(
                volume_center.x - volume_size.x * 0.5f,
                volume_center.y - volume_size.y * 0.5f,
                volume_center.z - volume_size.z * 0.5f
            );
        }
        ImGui::SliderFloat("Volume Intensity Scale", &selected_volume.intensity_scale, 0.0f, 4.0f);
        ImGui::SliderFloat("Volume Blend Distance", &selected_volume.blend_distance, 0.01f, 32.0f);

        const float3 volume_max = float3(
            selected_volume.origin.x + selected_volume.extent.x,
            selected_volume.origin.y + selected_volume.extent.y,
            selected_volume.origin.z + selected_volume.extent.z
        );
        ImGui::Text(
            "Volume Min: (%.2f, %.2f, %.2f)",
            selected_volume.origin.x,
            selected_volume.origin.y,
            selected_volume.origin.z
        );
        ImGui::Text("Volume Max: (%.2f, %.2f, %.2f)", volume_max.x, volume_max.y, volume_max.z);

        ImGui::SliderFloat("Intensity", &m_config.probe_gi_intensity, 0.0f, 32.0f);
        ImGui::SliderFloat("Normal Bias", &m_config.probe_gi_normal_bias, 0.0f, 1.0f);
        ImGui::SliderFloat("Trace Distance", &m_config.probe_gi_trace_distance, 0.5f, 64.0f);
        ImGui::SliderInt(
            "Trace Rays",
            &m_config.probe_gi_trace_ray_count,
            1,
            static_cast<int>(Render::RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT)
        );
        ImGui::SliderFloat("Visibility Bias", &m_config.probe_gi_visibility_bias, 0.0f, 2.0f);
        ImGui::SliderFloat("Visibility Power", &m_config.probe_gi_visibility_power, 0.25f, 8.0f);
        ImGui::SliderFloat("Visibility Min Weight", &m_config.probe_gi_visibility_min_weight, 0.0f, 1.0f);
        ImGui::SliderFloat("Visibility Strength", &m_config.probe_gi_visibility_strength, 0.0f, 1.0f);
        ImGui::SliderFloat("Irradiance Hysteresis", &m_config.probe_gi_irradiance_hysteresis, 0.0f, 0.99f);
        ImGui::SliderFloat("Visibility Hysteresis", &m_config.probe_gi_visibility_hysteresis, 0.0f, 0.99f);
        ImGui::SliderFloat("Sky Intensity", &m_config.probe_gi_sky_intensity, 0.0f, 8.0f);
        ImGui::ColorEdit3("Sky Color", (float*)&m_config.probe_gi_sky_color);
        ImGui::ColorEdit3("Ground Color", (float*)&m_config.probe_gi_ground_color);

        static const char* s_probe_gi_debug_modes[] = {
            "Off",
            "Volume Cells",
            "Irradiance",
            "Contribution",
            "Visibility",
            "Brick Residency",
            "Update Age",
            "Physical Allocation",
            "APV Cell Layout",
            "APV Desired Level",
            "Hierarchy Resolve",
            "Dirty Priority",
            "Streaming State",
            "Clipmap / Prefetch",
        };
        ImGui::Combo("Debug View", &m_config.probe_gi_debug_mode, s_probe_gi_debug_modes, 14);
        if (m_config.probe_gi_debug_mode != 0) {
            ImGui::SliderFloat("Debug Scale", &m_config.probe_gi_debug_scale, 0.0f, 8.0f);
        }

        ImGui::Separator();
        ImGui::Checkbox("Show Volume Bounds", &m_config.probe_gi_volume_bounds_enabled);
        if (m_config.probe_gi_volume_bounds_enabled) {
            ImGui::SliderFloat("Bounds Thickness", &m_config.probe_gi_volume_bounds_thickness, 0.002f, 0.12f);
            ImGui::ColorEdit3("Bounds Color", (float*)&m_config.probe_gi_volume_bounds_color);
        }
        ImGui::Checkbox("Show Probe Gizmos", &m_config.probe_gi_gizmo_enabled);
        if (m_config.probe_gi_gizmo_enabled) {
            static const char* s_probe_gizmo_color_modes[] = {
                "Fixed Color",
                "Irradiance",
                "Visibility",
                "State",
                "APV Selected Level",
            };
            ImGui::Combo("Gizmo Color", &m_config.probe_gi_gizmo_color_mode, s_probe_gizmo_color_modes, 5);
            ImGui::SliderFloat("Gizmo Size", &m_config.probe_gi_gizmo_size, 0.02f, 1.0f);
            ImGui::SliderFloat("Gizmo Thickness", &m_config.probe_gi_gizmo_thickness, 0.002f, 0.08f);
            ImGui::SliderFloat("Gizmo Intensity", &m_config.probe_gi_gizmo_intensity, 0.1f, 8.0f);
            if (m_config.probe_gi_gizmo_color_mode == 0) {
                ImGui::ColorEdit3("Gizmo Fixed Color", (float*)&m_config.probe_gi_gizmo_fixed_color);
            }
        }

        ImGui::TreePop();
    }

    // MARK: Tonemapping
    // Tonemapping究极重要，所以放到最开头，以提示用户调节选项
    if (ImGui::TreeNode("Tonemapping")) {

        ImGui::Checkbox("Enable Auto Exposure", &m_config.tonemapping_ae.enabled);

        // 自动曝光或手动曝光都可以调整Exposure EV
        ImGui::SliderFloat("Exposure EV", &m_config.tonemapping_exposure_ev, -15.0f, 10.0f);

        // 自动曝光
        if (m_config.tonemapping_ae.enabled) {

            ImGui::Checkbox("Visualize (Try this!)", &m_config.tonemapping_ae.debug_visualize);

            ImGui::Checkbox("Enable ACES ToneMapping", &m_config.tonemapping_ae.aces_tonemapping_enabled);

            ImGui::SliderFloat("Luminance(log2) Min", &m_config.tonemapping_ae.log2lum_min, -20.0f, 5.0f);
            ImGui::SliderFloat("Luminance(log2) Max", &m_config.tonemapping_ae.log2lum_max, -5.0f, 20.0f);
            m_config.tonemapping_ae.log2lum_min =
                Min(m_config.tonemapping_ae.log2lum_min, m_config.tonemapping_ae.log2lum_max);
            m_config.tonemapping_ae.log2lum_max =
                Max(m_config.tonemapping_ae.log2lum_min, m_config.tonemapping_ae.log2lum_max);

            ImGui::SliderFloat(
                "Histogram Low Percentile", &m_config.tonemapping_ae.histogram_low_percentile, 0.0f, 1.0f
            );
            ImGui::SliderFloat(
                "Histogram High Percentile", &m_config.tonemapping_ae.histogram_high_percentile, 0.0f, 1.0f
            );
            m_config.tonemapping_ae.histogram_low_percentile =
                Min(m_config.tonemapping_ae.histogram_low_percentile,
                    m_config.tonemapping_ae.histogram_high_percentile);
            m_config.tonemapping_ae.histogram_high_percentile =
                Max(m_config.tonemapping_ae.histogram_low_percentile,
                    m_config.tonemapping_ae.histogram_high_percentile);

            ImGui::SliderFloat(
                "Eye Adaptation Speed (Up)", &m_config.tonemapping_ae.eye_adaptation_speed_up, 0.1f, 10.0f
            );
            ImGui::SliderFloat(
                "Eye Adaptation Speed (Down)", &m_config.tonemapping_ae.eye_adaptation_speed_down, 0.1f, 10.0f
            );

        } else {

            // 手动曝光
            ImGui::Checkbox("Enable Reinhard Tone Mapping", &m_config.tonemapping_reinhard_enabled);
        }

        ImGui::TreePop();
    }

    // MARK: Bloom
    if (ImGui::TreeNode("Bloom", "Bloom: [%s]", (m_config.bloom_enabled ? "Enable" : "Disable"))) {
        if (ImGui::Selectable("Enable", m_config.bloom_enabled)) {
            m_config.bloom_enabled = true;
        }
        draw_border();
        if (ImGui::Selectable("Disable", !m_config.bloom_enabled)) {
            m_config.bloom_enabled = false;
        }
        draw_border();

        ImGui::TreePop();
    }

    // MARK: Shadow
    if (ImGui::TreeNode(
            "Shadow", "Shadow: [%s]", s_shadow_map_mode_name_map.at(m_config.shadow_map_mode).c_str()
        )) {

        ImGui::Text("Only project 1st Dir.Light");

        for (uint i = 0; i < s_shadow_map_mode_name_map.size(); i++) {
            const EShadowMapMode cur_enum = static_cast<EShadowMapMode>(i);
            if (ImGui::Selectable(
                    s_shadow_map_mode_name_map.at(cur_enum).c_str(), m_config.shadow_map_mode == cur_enum
                )) {
                m_config.shadow_map_mode = cur_enum;
            }
            draw_border();
        }

        ImGui::Checkbox("Enable PCSS", &m_config.shadow_pcss_enabled);
        if (m_config.shadow_pcss_enabled) {
            ImGui::SliderFloat("Light Size World", &m_config.shadow_pcss_light_size_world, 0.001f, 0.1f);
        }

        const auto draw_csm_common_settings = [&]() {
            ImGui::SliderInt("Num of Cascades", &m_config.shadow_csm_num_of_cascades, 1, CSM_MAX_CASCADES);
            ImGui::SliderInt("Shadow Map Size", &m_config.shadow_csm_sm_size, 512, 4096);
            ImGui::Checkbox("Visualize CSM Cascade", &m_config.shadow_csm_visualize_cascade);
            if (m_config.shadow_csm_visualize_cascade) {
                ImGui::TextDisabled("Cascade Colors:");

                static const ImVec4 s_cascade_visualize_colors[CSM_MAX_CASCADES] = {
                    ImVec4(0.96f, 0.24f, 0.24f, 1.0f),
                    ImVec4(0.96f, 0.58f, 0.18f, 1.0f),
                    ImVec4(0.25f, 0.78f, 0.32f, 1.0f),
                    ImVec4(0.20f, 0.45f, 0.96f, 1.0f)
                };
                static const char* s_cascade_visualize_color_names[CSM_MAX_CASCADES] = {
                    "Cascade 0: Red", "Cascade 1: Orange", "Cascade 2: Green", "Cascade 3: Blue"
                };

                for (int cascade_index = 0; cascade_index < m_config.shadow_csm_num_of_cascades;
                     ++cascade_index) {
                    ImGui::TextColored(
                        s_cascade_visualize_colors[cascade_index],
                        "%s",
                        s_cascade_visualize_color_names[cascade_index]
                    );
                }
            }
            ImGui::Checkbox("Enable CSM Blend", &m_config.shadow_csm_blend_option);
            if (m_config.shadow_csm_blend_option) {
                ImGui::SliderFloat("Blend Percentage", &m_config.shadow_csm_blend_percentage, 0, 1);
            }
        };

        const auto draw_csm_cache_settings = [&]() {
            ImGui::Separator();
            ImGui::Checkbox("Enable Shadow Cache", &m_config.shadow_cache_enabled);
            ImGui::TextWrapped(
                "First N cascades always refresh. Later cascades may reuse cached shadow maps when camera "
                "motion stays below threshold."
            );
            ImGui::SliderInt(
                "Disable Cache For First N Cascades",
                &m_config.shadow_cache_disable_first_n_cascades,
                0,
                m_config.shadow_csm_num_of_cascades
            );
            m_config.shadow_cache_disable_first_n_cascades =
                Max(0,
                    Min(m_config.shadow_cache_disable_first_n_cascades, m_config.shadow_csm_num_of_cascades));
            for (int cascade_index = 0; cascade_index < m_config.shadow_csm_num_of_cascades;
                 ++cascade_index) {
                ImGui::SliderFloat(
                    std::format("Cascade {} Cache Move Threshold (Texels)", cascade_index).c_str(),
                    &m_config.shadow_cache_camera_move_threshold_in_texels[cascade_index],
                    0.0f,
                    128.0f
                );
            }
        };

        if (m_config.shadow_map_mode == EShadowMapMode::POINT_CUBE) {
            ImGui::SliderInt("Shadow Map Size", &m_config.shadow_csm_sm_size, 512, 4096);
            ImGui::Checkbox(
                "Enable Point Shadow Multiview",
                &m_config.shadow_point_multiview_enabled
            );
        } else if (m_config.shadow_map_mode == EShadowMapMode::CSM) {
            draw_csm_common_settings();
            float minimum_cover_ratio = 0.0f;
            for (int cascade_index = 0; cascade_index < m_config.shadow_csm_num_of_cascades;
                 ++cascade_index) {
                ImGui::SliderFloat(
                    std::format("{}-th CSM Cover Ratio", cascade_index).c_str(),
                    &m_config.shadow_csm_cover_ratio_of_camera[cascade_index],
                    0.0f,
                    (cascade_index < m_config.shadow_csm_num_of_cascades - 1) ? 0.2f : 1.0f
                );
                m_config.shadow_csm_cover_ratio_of_camera[cascade_index] = Max(
                    m_config.shadow_csm_cover_ratio_of_camera[cascade_index], minimum_cover_ratio
                );
                minimum_cover_ratio = m_config.shadow_csm_cover_ratio_of_camera[cascade_index];
            }
            draw_csm_cache_settings();
        } else if (m_config.shadow_map_mode == EShadowMapMode::CSM_AUTO) {
            draw_csm_common_settings();
            ImGui::SliderFloat(
                "Max Cover Ratio",
                &m_config.shadow_csm_auto_max_cover_ratio_of_camera,
                0.0001f,
                1.0f
            );
            ImGui::SliderFloat("Lerp Factor", &m_config.shadow_csm_lerp_factor, 0, 1);
            draw_csm_cache_settings();
        }

        ImGui::TreePop();
    }

    // MARK: AO
    if (ImGui::TreeNode(
            "Ambient Occlusion", "Ambient Occlusion: [%s]", s_ao_mode_name_map.at(m_config.ao_mode).c_str()
        )) {

        assert(s_ao_mode_name_map.size() == static_cast<uint32>(EAoMode::NUM));
        for (uint i = 0; i < s_ao_mode_name_map.size(); i++) {
            const EAoMode cur_enum = static_cast<EAoMode>(i);
            if (ImGui::Selectable(s_ao_mode_name_map.at(cur_enum).c_str(), m_config.ao_mode == cur_enum)) {
                m_config.ao_mode = cur_enum;
            }
            draw_border();
        }

        ImGui::Separator();
        ImGui::Checkbox("Half Resolution AO", &m_config.ao_half_resolution);

        if (m_config.ao_mode == EAoMode::SSAO || m_config.ao_mode == EAoMode::SSAO_AO_ONLY) {
            ImGui::SliderFloat("Intensity", &m_config.ssao_intensity, 0.0f, 2.0f);
            ImGui::SliderFloat("Ray Trace Radius", &m_config.ssao_max_distance, 0.0f, 5.0f);
            ImGui::SliderInt("Samples Per Pixel", &m_config.ssao_spp, 1, 32);
            ImGui::SliderInt("Sample Radius", &m_config.ssao_sample_radius, 1, 32);

        } else if (m_config.ao_mode == EAoMode::RTAO || m_config.ao_mode == EAoMode::RTAO_AO_ONLY) {
            ImGui::SliderFloat("Intensity", &m_config.rtao_intensity, 0.0f, 1.0f);
            ImGui::SliderFloat("Ray Trace Radius", &m_config.rtao_ray_trace_distance, 0.0f, 20.0f);
            ImGui::SliderInt("Samples Per Pixel", &m_config.rtao_spp, 1, 32);

            ImGui::Text("RTAO Sample Mode:");
            assert(s_rtao_sample_mode_name_map.size() == static_cast<uint32>(ERtaoSampleMode::NUM));
            for (uint i = 0; i < s_rtao_sample_mode_name_map.size(); i++) {
                const ERtaoSampleMode cur_enum = static_cast<ERtaoSampleMode>(i);
                if (ImGui::Selectable(
                        s_rtao_sample_mode_name_map.at(cur_enum).c_str(),
                        m_config.rtao_sample_mode == cur_enum
                    )) {
                    m_config.rtao_sample_mode = cur_enum;
                }
                draw_border();
            }

            ImGui::Separator();

            ImGui::Checkbox("Enable RTAO TAA Denoiser", &m_config.rtao_denoiser_enable);

            // Denoiser 启用后才能启用 Reprojection
            if (m_config.rtao_denoiser_enable) {
                ImGui::SliderFloat(
                    "Denoiser History Ratio", &m_config.rtao_denoiser_history_ratio, 0.0f, 1.0f
                );

                ImGui::Checkbox("Enable RTAO Reprojection", &m_config.rtao_denoiser_reprojection_enable);
            }

            // 启用 Reprojection 后才能启用 Validation
            if (m_config.rtao_denoiser_reprojection_enable) {
                ImGui::Checkbox("Enable RTAO Validation", &m_config.rtao_denoiser_validation_enable);
                ImGui::Checkbox("Enable RTAO History Clamp", &m_config.rtao_denoiser_history_clamp_enable);
                ImGui::Checkbox(
                    "Enable RTAO Motion Weighting", &m_config.rtao_denoiser_motion_weighting_enable
                );
            }

            // 启用 Validation 后的额外选项
            if (m_config.rtao_denoiser_validation_enable) {
                ImGui::SliderFloat(
                    "Validation Depth Threshold", &m_config.rtao_denoiser_valid_depth_threshold, 0.0f, 0.1f
                );
                ImGui::SliderFloat(
                    "Validation Normal Threshold", &m_config.rtao_denoiser_valid_normal_threshold, 0.0f, 1.0f
                );
            }

        } else if (m_config.ao_mode == EAoMode::SSDO || m_config.ao_mode == EAoMode::SSDO_AO_ONLY) {
            ImGui::SliderFloat("Intensity", &m_config.ssao_intensity, 0.0f, 2.0f);
            ImGui::SliderFloat("Indirect Intensity", &m_config.ssdo_indirect_intensity, 0.0f, 2.0f);
            ImGui::SliderFloat("Ray Trace Radius", &m_config.ssdo_max_distance, 0.0f, 20.0f);
            ImGui::SliderInt("Samples Per Pixel", &m_config.ssao_spp, 1, 16);
            ImGui::SliderFloat("Sample Radius", &m_config.ssdo_sample_radius, 0.0f, 5.0f);
            ImGui::SliderFloat("Depth Bias", &m_config.ssdo_depth_bias, 0.0f, 0.1f);
        }

        ImGui::TreePop();
    }

    // MARK: SSR
    if (ImGui::TreeNode("SSR", "SSR: [%s]", (m_config.ssr_is_ssr_enabled == 1 ? "Enable" : "Disable"))) {
        if (ImGui::Selectable("Enable", m_config.ssr_is_ssr_enabled == 1)) {
            m_config.ssr_is_ssr_enabled = 1;
        }
        draw_border();
        if (ImGui::Selectable("Disable", m_config.ssr_is_ssr_enabled == 0)) {
            m_config.ssr_is_ssr_enabled = 0;
        }
        draw_border();

        if (m_config.ssr_is_ssr_enabled == 1) {
            ImGui::Checkbox("Enable Jitter", &m_config.ssr_is_enable_jitter);
            ImGui::Checkbox("Force Ground Enable SSR", &m_config.ssr_is_force_ground_enable_ssr);
            ImGui::SliderInt("Sample Count", &m_config.ssr_sample_count, 1, 64);
            ImGui::SliderFloat("Step Base", &m_config.ssr_step_base, 0.0f, 0.1f);
            ImGui::SliderFloat("Roughness Threshold", &m_config.ssr_roughness_threshold, 0.0f, 1.0f);
            ImGui::SliderFloat("Metallic Threshold", &m_config.ssr_metallic_threshold, 0.0f, 1.0f);
        }

        ImGui::TreePop();
    }

    // MARK: Cooperative Ops
    m_cooperative_ops_ui.ShowConfig();

    // MARK: Skybox
    if (ImGui::TreeNode("Skybox")) {

        ImGui::Checkbox("Enable Exposure Correction", &m_config.skybox_exposure_correct_enabled);
        ImGui::SliderFloat(
            "Exp.Cect. Factor (log10)", &m_config.skybox_exposure_correct_factor_log10, -3.0f, 1.0f
        );

        ImGui::TreePop();
    }

    // MARK: Denoiser
    if (ImGui::TreeNode(
            "Denoiser",
            "Denoiser: [%s]",
            s_denoiser_mode_name_map.at(static_cast<EDenoiserMode>(m_config.denoiser_mode)).c_str()
        )) {

        assert(s_denoiser_mode_name_map.size() == static_cast<uint32>(EDenoiserMode::NUM));
        for (uint i = 0; i < s_denoiser_mode_name_map.size(); i++) {
            const EDenoiserMode cur_enum = static_cast<EDenoiserMode>(i);
            if (ImGui::Selectable(
                    s_denoiser_mode_name_map.at(cur_enum).c_str(), m_config.denoiser_mode == cur_enum
                )) {
                m_config.denoiser_mode = cur_enum;
            }
            draw_border();
        }

        ImGui::Separator();

        ImGui::SliderInt("双边滤波 Radius", &m_config.denoiser_bfd_kernel_radius, 1, 10);
        ImGui::SliderFloat(
            "双边滤波 SpatialSigma^2", &m_config.denoiser_bfd_spatial_sigma_square, 1.0f, 150.0f
        );
        ImGui::SliderFloat("双边滤波 RangeSigma^2", &m_config.denoiser_bfd_range_sigma_square, 0.001f, 0.05f);

        ImGui::TreePop();
    }

#if WITH_CUDA
    // MARK: AI - Upsample
    if (ImGui::TreeNode(
            "Upsample",
            "Upsample: [%s]",
            s_upsample_mode_name_array[static_cast<uint32>(m_config.upsample_mode)].c_str()
        )) {

        ImGui::Text("目前需要在RasterTextures.h中编译期启用");

        for (uint i = 0; i < s_upsample_mode_name_array.size(); i++) {
            if (ImGui::Selectable(
                    s_upsample_mode_name_array[i].c_str(),
                    m_config.upsample_mode == static_cast<EUpsampleMode>(i)
                )) {
                m_config.upsample_mode = static_cast<EUpsampleMode>(i);
            }
            draw_border();
        }

        if (m_config.upsample_mode == EUpsampleMode::BILINEAR) {
            ImGui::SliderInt("Input Size", &m_config.inSize_x, 0, 1500);
            ImGui::SliderInt("Output Size X", &m_config.outSize_x, 0, 3000);
            ImGui::SliderInt("Output Size Y", &m_config.outSize_y, 0, 3000);
        }

        ImGui::TreePop();
    }

    // MARK: AI - CUDA
    if (ImGui::TreeNode("CUDA", "CUDA: [%s]", (m_config.ai_is_cuda_enabled == 1 ? "Enable" : "Disable"))) {
        if (ImGui::Selectable("Enable", m_config.ai_is_cuda_enabled == 1)) {
            m_config.ai_is_cuda_enabled = 1;
            // 自动选择out_final_output
            for (uint i = 0; i < s_ai_trt_visualize_buffer_array.size(); i++) {
                if (strcmp(s_ai_trt_visualize_buffer_array[i].c_str(), "Engine2 out_final_output") == 0) {
                    m_config.ai_trt_visualize_buffer_idx = i;
                }
            }
            // 自动启用RTAO
            if (m_config.ao_mode != EAoMode::RTAO) {
                m_config.ao_mode = EAoMode::RTAO;
                LOG_INFO("Ambient Occlusion Mode switched to RTAO automatically.");
            }
        }
        draw_border();
        if (ImGui::Selectable("Disable", m_config.ai_is_cuda_enabled == 0)) {
            m_config.ai_is_cuda_enabled = 0;
        }
        draw_border();

        // 这里这个选项，是为了修复ONNX网络无法处理HDR的问题
        // 此处使用了一个非常简单的Reinhard ToneMapping，没有自动曝光。所以明亮处会过曝
        ImGui::Checkbox("Force LDR Input & Output", &m_config.ai_trt_force_ldr);

        if (m_config.ai_is_cuda_enabled == 1) {
            for (uint i = 0; i < s_ai_trt_visualize_buffer_array.size(); i++) {
                if (ImGui::Selectable(
                        s_ai_trt_visualize_buffer_array[i].c_str(), m_config.ai_trt_visualize_buffer_idx == i
                    )) {
                    m_config.ai_trt_visualize_buffer_idx = i;
                    m_config.ai_trt_visualize_buffer     = s_ai_trt_visualize_buffer_array[i];
                }
                draw_border();
            }
        }

        ImGui::TreePop();
    }
#endif

    // MARK: Anti-Aliasing
    if (ImGui::TreeNode(
            "Anti-Aliasing", "Anti-Aliasing: [%s]", s_aa_mode_name_map.at(m_config.aa_mode).c_str()
        )) {

        assert(s_aa_mode_name_map.size() == static_cast<uint32>(EAaMode::NUM));
        for (uint i = 0; i < s_aa_mode_name_map.size(); i++) {
            const EAaMode cur_enum = static_cast<EAaMode>(i);
            if (ImGui::Selectable(s_aa_mode_name_map.at(cur_enum).c_str(), m_config.aa_mode == cur_enum)) {
                m_config.aa_mode = cur_enum;
            }
            draw_border();
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Debug")) {
        ImGui::SliderFloat("Debug Param", &m_config.debug_param, 0.0f, 1.0f);

        ImGui::Separator(); // 分割线

        ImGui::Checkbox("Enable FPS Limit", &m_config.debug_fps_limit_enable);
        if (m_config.debug_fps_limit_enable) {
            ImGui::SliderFloat("FPS Limit", &m_config.debug_fps_limit, 0.5f, 240.0f);
        }
        ImGui::TreePop();
    }

    ImGui::TreePop();
}

void RasterUI::RegisterFrameBufferNames(const Array<std::string>& frame_buffer_names) {
    m_frame_buffer_names = frame_buffer_names;

    if (!m_frame_buffer_names_initialized) {
        m_frame_buffer_names_initialized = true;
        m_config.selected_frame_buffer_index = GetDefaultSelectedFrameBufferIndex();
    }

    assert(
        m_config.selected_frame_buffer_index < m_frame_buffer_names.size() &&
        "Invalid default selected frame buffer index"
    );
}

uint RasterUI::GetDefaultSelectedFrameBufferIndex() const {
    constexpr std::string_view k_default_frame_buffer_name = "tonemapping_output";

    for (uint i = 0; i < m_frame_buffer_names.size(); ++i) {
        if (m_frame_buffer_names[i] == k_default_frame_buffer_name) {
            return i;
        }
    }

    assert(false && "Invalid default selected frame buffer index");
    return uint(0);
}

} // namespace Moer
