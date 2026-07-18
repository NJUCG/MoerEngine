#include "renderer/raster/RasterTool.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIImpl.h"

#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

using namespace Moer;
using namespace Moer::Render;
using namespace Moer::Render::Raster;

namespace {

void Expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

const ScopeCmd& ScopeAt(const CmdSubmit& submit, size_t index) {
    Expect(index < submit.cmds.size(), "scope command index is out of range");
    Expect(submit.cmds[index]->Type() == Command::EType::Scope, "expected a ScopeCmd");
    return *static_cast<const ScopeCmd*>(submit.cmds[index].get());
}

void MarkerScopesAreBalancedColoredAndImmobile() {
    static_assert(!std::is_copy_constructible_v<ScopedGpuMarker>);
    static_assert(!std::is_move_constructible_v<ScopedGpuMarker>);

    CommandList cmd_list;
    {
        ScopedGpuMarker renderer(cmd_list, "Renderer", GpuMarkerPalette::Renderer());
        {
            ScopedGpuMarker timed(
                cmd_list,
                "Timed Pass",
                GpuMarkerPalette::Pass(),
                EGpuMarkerMode::Timestamp
            );
        }
    }

    CmdSubmit submit = cmd_list.Submit().DebugLabel("Test Frame", GpuMarkerPalette::Frame());
    Expect(submit.debug_label == "Test Frame", "submission debug label was not retained");
    Expect(submit.cmds.size() == 4, "RAII markers emitted an unbalanced command pair");

    const ScopeCmd& renderer_push = ScopeAt(submit, 0);
    const ScopeCmd& timed_push    = ScopeAt(submit, 1);
    const ScopeCmd& timed_pop     = ScopeAt(submit, 2);
    const ScopeCmd& renderer_pop  = ScopeAt(submit, 3);

    Expect(renderer_push.IsPush(), "renderer scope did not begin with a push");
    Expect(renderer_pop.IsPop(), "renderer scope did not end with a pop");
    Expect(
        renderer_push.ScopeName() == renderer_pop.ScopeName(),
        "renderer push/pop names differ"
    );
    Expect(!renderer_push.QueryTimestamp(), "visual renderer marker unexpectedly requested timing");
    Expect(timed_push.QueryTimestamp() && timed_pop.QueryTimestamp(), "timed marker lost its query pair");
    Expect(timed_push.ScopeName() == timed_pop.ScopeName(), "timed push/pop names differ");
    Expect(
        renderer_push.Color().x == GpuMarkerPalette::Renderer().x,
        "marker category color was not retained"
    );

    // Timestamp keys are per submission, not per CommandList lifetime.
    {
        ScopedGpuMarker timed_again(
            cmd_list,
            "Timed Pass",
            GpuMarkerPalette::Pass(),
            EGpuMarkerMode::Timestamp
        );
    }
    CmdSubmit next_submit = cmd_list.Submit();
    Expect(next_submit.cmds.size() == 2, "timestamp key was not reset after Submit");
    Expect(ScopeAt(next_submit, 0).QueryTimestamp(), "second submission lost timestamp profiling");
}

void PointAndCsmMarkerNamesAreCompleteAndDisjoint() {
    std::set<std::string_view> point_faces;
    std::set<std::string_view> point_culling;
    std::set<std::string_view> point_draw;
    for (uint face = 0; face < CUBE_FACE_Num; ++face) {
        point_faces.emplace(RasterTool::GetPointShadowFaceMarkerName(face));
        point_culling.emplace(RasterTool::GetPointShadowCullingProfileScopeName(face));
        point_draw.emplace(RasterTool::GetPointShadowDrawProfileScopeName(face));
    }
    Expect(point_faces.size() == CUBE_FACE_Num, "point-shadow face labels are not unique");
    Expect(point_culling.size() == CUBE_FACE_Num, "point-shadow culling keys are not unique");
    Expect(point_draw.size() == CUBE_FACE_Num, "point-shadow draw keys are not unique");
    Expect(
        RasterTool::GetPointShadowFaceMarkerName(CUBE_FACE_PX).find("+X") != std::string_view::npos,
        "positive-X cube face label is incorrect"
    );
    Expect(
        RasterTool::GetPointShadowFaceMarkerName(CUBE_FACE_NZ).find("-Z") != std::string_view::npos,
        "negative-Z cube face label is incorrect"
    );

    std::set<std::string_view> csm_culling;
    std::set<std::string_view> csm_draw;
    for (uint cascade = 0; cascade < CSM_MAX_CASCADES; ++cascade) {
        csm_culling.emplace(RasterTool::GetCsmShadowCullingProfileScopeName(cascade));
        csm_draw.emplace(RasterTool::GetCsmShadowDrawProfileScopeName(cascade));
    }
    Expect(csm_culling.size() == CSM_MAX_CASCADES, "CSM culling keys are not unique");
    Expect(csm_draw.size() == CSM_MAX_CASCADES, "CSM draw keys are not unique");
    for (std::string_view name : csm_culling) {
        Expect(!point_culling.contains(name), "CSM and point-shadow culling keys overlap");
    }
    for (std::string_view name : csm_draw) {
        Expect(!point_draw.contains(name), "CSM and point-shadow draw keys overlap");
    }
}

} // namespace

int main() {
    try {
        MarkerScopesAreBalancedColoredAndImmobile();
        PointAndCsmMarkerNamesAreCompleteAndDisjoint();
        std::cout << "RHI command marker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RHI command marker test failed: " << error.what() << '\n';
        return 1;
    }
}
