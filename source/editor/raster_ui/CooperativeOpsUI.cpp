#include "CooperativeOpsUI.h"


namespace Moer {

CooperativeOpsUI::CooperativeOpsUI(RasterConfig& config) : m_config(config) {}

void CooperativeOpsUI::ShowConfig(Synapse::Context& ui) {
    if (!ui.TreeNode(
            "Cooperative Ops",
            "Cooperative Ops: [%s]",
            (m_config.cooperative_ops_enabled ? "Enable" : "Disable")
        )) {
        return;
    }

    auto draw_border = [&]() {
        ui.DrawLastItemBorder();
    };

    auto& status = m_config.cooperative_ops_status;

    if (ui.Selectable("Enable", m_config.cooperative_ops_enabled)) {
        m_config.cooperative_ops_enabled = true;
    }
    draw_border();

    if (ui.Selectable("Disable", !m_config.cooperative_ops_enabled)) {
        m_config.cooperative_ops_enabled = false;
    }
    draw_border();

    if (m_config.cooperative_ops_enabled) {
        ui.Separator();
        ui.TextWrapped("%s", status.overview.c_str());
        ui.Text("Frames Evaluated: %u", status.frames_evaluated);
        ui.Text("Modes: matrix=%u vector=%u", status.matrix_mode_count, status.vector_mode_count);
        ui.Text(
            "Stage Mask: matrix=0x%X vector=0x%X",
            status.matrix_supported_stages,
            status.vector_supported_stages
        );
        if (status.max_vector_components > 0) {
            ui.Text("Max Vector Components: %u", status.max_vector_components);
        }

        ui.Separator();
        ui.TextWrapped("Matrix Summary: %s", status.matrix_summary.c_str());
        ui.TextWrapped("Matrix Runtime: %s", status.matrix_runtime_status.c_str());
        ui.TextWrapped("Vector Summary: %s", status.vector_summary.c_str());
        ui.TextWrapped("Vector Runtime: %s", status.vector_runtime_status.c_str());
    }

    ui.TreePop();
}

} // namespace Moer