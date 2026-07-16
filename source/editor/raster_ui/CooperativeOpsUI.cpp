#include "CooperativeOpsUI.h"

// 将协作运算控制项和诊断信息与 Raster 主设置面板分离。

#include <imgui.h>

namespace Moer {

CooperativeOpsUI::CooperativeOpsUI(RasterConfig& config) : m_config(config) {}

void CooperativeOpsUI::ShowConfig() {
    if (!ImGui::TreeNode(
            "Cooperative Ops",
            "Cooperative Ops: [%s]",
            (m_config.cooperative_ops_enabled ? "Enable" : "Disable")
        )) {
        return;
    }

    const auto draw_border = []() {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(min, max, IM_COL32(255, 255, 255, 255));
    };

    const auto& status = m_config.cooperative_ops_status;

    if (ImGui::Selectable("Enable", m_config.cooperative_ops_enabled)) {
        m_config.cooperative_ops_enabled = true;
    }
    draw_border();

    if (ImGui::Selectable("Disable", !m_config.cooperative_ops_enabled)) {
        m_config.cooperative_ops_enabled = false;
    }
    draw_border();

    if (m_config.cooperative_ops_enabled) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", status.overview.c_str());
        ImGui::Text("Frames Evaluated: %u", status.frames_evaluated);
        ImGui::Text("Modes: matrix=%u vector=%u", status.matrix_mode_count, status.vector_mode_count);
        ImGui::Text(
            "Stage Mask: matrix=0x%X vector=0x%X",
            status.matrix_supported_stages,
            status.vector_supported_stages
        );
        if (status.max_vector_components > 0) {
            ImGui::Text("Max Vector Components: %u", status.max_vector_components);
        }

        ImGui::Separator();
        ImGui::TextWrapped("Matrix Summary: %s", status.matrix_summary.c_str());
        ImGui::TextWrapped("Matrix Runtime: %s", status.matrix_runtime_status.c_str());
        ImGui::TextWrapped("Vector Summary: %s", status.vector_summary.c_str());
        ImGui::TextWrapped("Vector Runtime: %s", status.vector_runtime_status.c_str());
    }

    ImGui::TreePop();
}

} // namespace Moer
