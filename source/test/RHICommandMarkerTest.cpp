#include "renderer/raster/RasterTool.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIImpl.h"

#include <algorithm>
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

class RetainedTestBuffer final : public Buffer {
public:
    RetainedTestBuffer() :
        Buffer(BufferInfo{4096, 1, EBufferUsageFlags::ACCELERATION_STRUCTURE}) {}

    void SetName(const std::string_view) override {}
};

class RetainedTestTlas final : public RaytracingTlas {
public:
    explicit RetainedTestTlas(BufferRef buffer) : buffer(std::move(buffer)) {}

    Buffer* GetUnderlyingBuffer() const override {
        return buffer.Get();
    }

private:
    BufferRef buffer;
};

class RetainedTestGeometry final : public RaytracingGeometry {
public:
    explicit RetainedTestGeometry(BufferRef buffer) :
        RaytracingGeometry(RaytracingGeometryInfo{}),
        buffer(std::move(buffer)) {}

    Buffer* GetUnderlyingBuffer() const override {
        return buffer.Get();
    }

private:
    BufferRef buffer;
};

class RetainedTestScene final : public RaytracingScene {
public:
    RetainedTestScene(RaytracingTlasRef current, RaytracingTlasRef previous) :
        current(std::move(current)),
        previous(std::move(previous)) {}

    RaytracingInstance& AddInstance() override {
        throw std::logic_error("RetainedTestScene::AddInstance is not used");
    }
    void FreeInstance(uint) override {}
    void MarkModified(uint) override {}
    UniquePtr<Command> UpdateScene() override {
        return {};
    }
    void AdvanceFrame() override {
        std::swap(current, previous);
    }
    void RegisterGeometry(RaytracingGeometryRef) override {}
    void UnregisterGeometry(RaytracingGeometryRef) override {}
    RaytracingTlasRef GetTlas() const override {
        return current;
    }
    RaytracingTlasRef GetPrevTlas() const override {
        return previous;
    }

private:
    RaytracingTlasRef current;
    RaytracingTlasRef previous;
};

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

void ProfilingPhasesPreserveOneLogicalFrame() {
    CommandList complete_list;
    CmdSubmit complete = complete_list.Submit().TickProfiling();
    Expect(complete.EmitsProfilingQueries(), "complete profiling phase must emit queries");
    Expect(
        complete.BeginsProfilingFrame() && complete.EndsProfilingFrame(),
        "TickProfiling must remain a complete single-submit frame"
    );

    CommandList begin_list;
    CmdSubmit begin = begin_list.Submit().SetProfilingPhase(ERHIProfilingPhase::Begin);
    Expect(
        begin.EmitsProfilingQueries() && begin.BeginsProfilingFrame() &&
            !begin.EndsProfilingFrame(),
        "split profiling begin phase has incorrect boundaries"
    );

    CommandList continue_list;
    CmdSubmit continuation =
        continue_list.Submit().SetProfilingPhase(ERHIProfilingPhase::Continue);
    Expect(
        continuation.EmitsProfilingQueries() && !continuation.BeginsProfilingFrame() &&
            !continuation.EndsProfilingFrame(),
        "split profiling continuation must only emit pass queries"
    );

    CommandList end_list;
    CmdSubmit end = end_list.Submit().SetProfilingPhase(ERHIProfilingPhase::End);
    Expect(
        end.EmitsProfilingQueries() && !end.BeginsProfilingFrame() &&
            end.EndsProfilingFrame(),
        "split profiling end phase has incorrect boundaries"
    );
}

void ScopeCommandsPreserveCommandListQueueAffinity() {
    for (EQueueType queue :
         {EQueueType::Graphics, EQueueType::Compute, EQueueType::Copy}) {
        CommandList cmd_list(queue);
        cmd_list.PushScope("QueueLabel");
        cmd_list.PopScope();
        cmd_list.PushScopeWithTimeScope("QueueTimed");
        cmd_list.PopScopeWithTimeScope();

        CmdSubmit submit = cmd_list.Submit();
        Expect(
            submit.cmds.size() == 4,
            "queue-affinity marker test emitted an unexpected command count"
        );
        for (std::size_t index = 0; index < submit.cmds.size(); ++index) {
            Expect(
                ScopeAt(submit, index).GetQueueType() == queue,
                "ScopeCmd lost its originating CommandList queue"
            );
        }
    }
}

void ConstSpanUploadsOwnImmutableSnapshots() {
    BufferRef target_buffer(MoerNew(RetainedTestBuffer)());
    const Array<byte> expected{
        byte{0x11},
        byte{0x22},
        byte{0x7f},
        byte{0xa5},
        byte{0x00},
        byte{0xff},
    };

    const auto validate_snapshot = [&](const CmdSubmit& submit) {
        Expect(
            submit.cmds.size() == 1,
            "const-span upload did not emit exactly one command"
        );
        Expect(
            submit.cmds.front()->Type() == Command::EType::UploadBuffer,
            "const-span upload changed command type"
        );
        const auto& upload =
            *static_cast<const UploadBufferCmd*>(submit.cmds.front().get());
        const auto data = upload.Data();
        Expect(
            data.size() == expected.size() &&
                std::equal(data.begin(), data.end(), expected.begin()),
            "const-span upload did not retain an immutable byte snapshot"
        );
        Expect(
            upload.Handle() == reinterpret_cast<uint64>(target_buffer.Get()),
            "const-span upload changed its destination buffer"
        );
        Expect(upload.Offset() == 128, "const-span upload changed its destination offset");
    };

    const auto record_snapshot = [&]() {
        CommandList cmd_list;
        Array<byte> source = expected;
        cmd_list.CopyFrom(
            std::span<const byte>(source.data(), source.size()),
            target_buffer->GetView(128, source.size()),
            "ConstSpanUploadSnapshot"
        );

        CmdSubmit submit = cmd_list.Submit();
        std::fill(source.begin(), source.end(), byte{0xee});
        validate_snapshot(submit);
        return submit;
    };

    CmdSubmit retained_submit = record_snapshot();
    validate_snapshot(retained_submit);
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

void PreparedRaytracingUpdatesAreNullSafeAndOneShot() {
    CommandList cmd_list;
    cmd_list.UpdateRaytracingScene(RaytracingSceneRef{});
    Expect(cmd_list.IsEmpty(), "a null ray tracing scene emitted a command");

    auto prepared = MakeUnique<UpdateRaytracingSceneCmd>(
        UnorderedMap<uint64, uint32>{},
        RaytracingSceneRef{},
        BufferRef{},
        BufferRef{},
        RaytracingTlasRef{},
        Array<RaytracingGeometryRef>{},
        Array<uint>{},
        Array<byte>{},
        0,
        false
    );
    cmd_list.UpdateRaytracingScene(std::move(prepared));
    Expect(!prepared, "prepared TLAS command ownership was not consumed");

    CmdSubmit submit = cmd_list.Submit();
    Expect(submit.cmds.size() == 1, "prepared TLAS command was not emitted exactly once");
    Expect(
        submit.cmds.front()->Type() == Command::EType::BuildTLAS,
        "prepared TLAS command changed command type"
    );
}

void PreparedRaytracingUpdatesRetainEveryNativeInput() {
    BufferRef instance_buffer(MoerNew(RetainedTestBuffer)());
    BufferRef scratch_buffer(MoerNew(RetainedTestBuffer)());
    BufferRef tlas_buffer(MoerNew(RetainedTestBuffer)());
    BufferRef previous_tlas_buffer(MoerNew(RetainedTestBuffer)());
    BufferRef geometry_buffer(MoerNew(RetainedTestBuffer)());
    RaytracingTlasRef tlas(MoerNew(RetainedTestTlas)(tlas_buffer));
    RaytracingTlasRef previous_tlas(
        MoerNew(RetainedTestTlas)(previous_tlas_buffer)
    );
    RaytracingGeometryRef geometry(
        MoerNew(RetainedTestGeometry)(geometry_buffer)
    );
    RaytracingSceneRef scene(
        MoerNew(RetainedTestScene)(tlas, previous_tlas)
    );

    const RaytracingScene*    scene_identity    = scene.Get();
    const Buffer*             instance_identity = instance_buffer.Get();
    const Buffer*             scratch_identity  = scratch_buffer.Get();
    const RaytracingTlas*     tlas_identity     = tlas.Get();
    const RaytracingGeometry* geometry_identity = geometry.Get();

    auto prepared = MakeUnique<UpdateRaytracingSceneCmd>(
        UnorderedMap<uint64, uint32>{{uint64(geometry.Get()), 1}},
        scene,
        instance_buffer,
        scratch_buffer,
        tlas,
        Array<RaytracingGeometryRef>{geometry},
        Array<uint>{0},
        Array<byte>{},
        1,
        false
    );
    CommandList cmd_list;
    cmd_list.UpdateRaytracingScene(std::move(prepared));

    scene           = {};
    instance_buffer = {};
    scratch_buffer  = {};
    tlas            = {};
    previous_tlas   = {};
    geometry        = {};
    tlas_buffer     = {};
    previous_tlas_buffer = {};
    geometry_buffer = {};

    CmdSubmit submit = cmd_list.Submit();
    Expect(submit.cmds.size() == 1, "retained TLAS command was not emitted");
    const auto* update =
        static_cast<const UpdateRaytracingSceneCmd*>(submit.cmds.front().get());
    Expect(update->Scene().Get() == scene_identity, "prepared update dropped its scene owner");
    Expect(
        update->InstanceBuffer().Get() == instance_identity,
        "prepared update dropped its instance buffer owner"
    );
    Expect(
        update->ScratchBuffer().Get() == scratch_identity,
        "prepared update dropped its scratch buffer owner"
    );
    Expect(update->Tlas().Get() == tlas_identity, "prepared update dropped its TLAS owner");
    Expect(
        update->GeometryRefs().size() == 1 &&
            update->GeometryRefs().front().Get() == geometry_identity,
        "prepared update dropped a related geometry owner"
    );
}

} // namespace

int main() {
    try {
        MarkerScopesAreBalancedColoredAndImmobile();
        ProfilingPhasesPreserveOneLogicalFrame();
        ScopeCommandsPreserveCommandListQueueAffinity();
        ConstSpanUploadsOwnImmutableSnapshots();
        PointAndCsmMarkerNamesAreCompleteAndDisjoint();
        PreparedRaytracingUpdatesAreNullSafeAndOneShot();
        PreparedRaytracingUpdatesRetainEveryNativeInput();
        std::cout << "RHI command marker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RHI command marker test failed: " << error.what() << '\n';
        return 1;
    }
}
