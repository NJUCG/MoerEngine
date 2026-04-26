#include "RuntimeProfiler.h"

#if WITH_PROFILE

#include "Profile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <imgui.h>

namespace Moer {

void RuntimeProfiler::DrawPassAndChildren(const char* parent_name, int depth) {
    for (int i = 0; i < g_pass_history_count; i++) {
        auto& history = g_pass_history[i];
        if (!history.active) {
            continue;
        }
        if (std::strcmp(history.parent_name, parent_name) != 0) {
            continue;
        }
        if (history.avg_ms < 0.001f && history.max_ms < 0.001f) {
            continue;
        }

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::Indent((1 + depth) * 20.0f);
        ImGui::Text("%s", history.name);
        ImGui::Unindent((1 + depth) * 20.0f);

        ImGui::TableSetColumnIndex(1);
        if (history.avg_ms > 5.0f) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%.4f", history.avg_ms);
        } else if (history.avg_ms > 2.0f) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%.4f", history.avg_ms);
        } else {
            ImGui::Text("%.4f", history.avg_ms);
        }

        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%.4f", history.max_ms);

        ImGui::TableSetColumnIndex(3);
        float ordered[60];
        for (int sample_index = 0; sample_index < 60; sample_index++) {
            ordered[sample_index] = history.samples[(history.write_idx + sample_index) % 60];
        }
        char plot_label[64];
        std::snprintf(plot_label, sizeof(plot_label), "##pass_%d", i);
        const float plot_max = history.max_ms > 0.0f ? history.max_ms * 1.2f : 1.0f;
        ImGui::PlotLines(plot_label, ordered, 60, 0, nullptr, 0.0f, plot_max, ImVec2(-1, 28));

        DrawPassAndChildren(history.name, depth + 1);
    }
}

void RuntimeProfiler::TickUI() {
    Profile_TickSample();

    if (!m_open) {
        return;
    }

    if (!ImGui::Begin("runtime_profiler", &m_open)) {
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Real-time Metrics", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("MetricsTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("Current (MB)");
            ImGui::TableSetupColumn("Peak (MB)");
            ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableHeadersRow();

            for (int source_index = 0; source_index < SOURCE_COUNT; ++source_index) {
                const auto& config = g_UIConfigs[source_index];
                const float current_mb = Profile_GetBytesBySource(config.source) / 1048576.0f;
                const float peak_mb = Profile_GetPeakBytesBySource(config.source) / 1048576.0f;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", config.label);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", current_mb);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.3f", peak_mb);

                ImGui::TableSetColumnIndex(3);
                ImGui::ColorButton(
                    config.label,
                    config.color,
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoInputs
                );
            }
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Live Memory Graph")) {
        static float view_max_y = 5.0f;

        std::vector<TimePoint> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_history_mtx);
            snapshot.assign(g_history_data.begin(), g_history_data.end());
        }

        if (snapshot.empty()) {
            ImGui::TextDisabled("No memory samples yet...");
        } else {
            const float plot_width = ImGui::GetContentRegionAvail().x;
            for (int source_index = 0; source_index < SOURCE_COUNT; ++source_index) {
                struct PlotContext {
                    int                     source_index;
                    std::vector<TimePoint>* samples;
                };
                PlotContext context = {source_index, &snapshot};

                auto getter = [](void* data, int index) -> float {
                    auto* context = static_cast<PlotContext*>(data);
                    return context->samples->operator[](index).values[context->source_index];
                };

                const float last_value = snapshot.back().values[source_index];
                view_max_y = std::max(view_max_y, last_value * 1.2f);

                ImGui::PushStyleColor(ImGuiCol_PlotLines, g_UIConfigs[source_index].color);
                ImGui::PlotLines(
                    "##RuntimeMemorySourcePlot",
                    getter,
                    &context,
                    static_cast<int>(snapshot.size()),
                    0,
                    nullptr,
                    0.0f,
                    view_max_y,
                    ImVec2(plot_width, 80)
                );
                ImGui::PopStyleColor();

                ImGui::SameLine();
                ImGui::Text("%s: %.2f MB", g_UIConfigs[source_index].label, last_value);
            }
        }
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("GPU Pass Timing", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::lock_guard<std::mutex> lock(g_pass_history_mtx);

        if (g_pass_history_count == 0) {
            ImGui::TextDisabled("No GPU data yet...");
        } else if (ImGui::BeginTable(
                       "PassTable",
                       4,
                       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                       ImVec2(0, 300)
                   )) {
            ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthFixed, 260);
            ImGui::TableSetupColumn("Avg ms", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Max ms", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Last 60 frames", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            DrawPassAndChildren("", 0);

            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Memory Hotspots Export:");

    if (ImGui::Button("Quick Dump (Hex Only)")) {
        WriteHotspots(false);
    }

    ImGui::SameLine();

    if (ImGui::Button("Full Dump (With Symbols)")) {
        WriteHotspots(true);
    }

    ImGui::End();
}

} // namespace Moer

#endif