#include "rhi/RHICommand.h"
#include "rhi/RHIGpuScope.h"
#include "rhi/RHIImpl.h"
#include "rhi/RHIQuery.h"

#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using namespace Moer::Render;

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

void ExpectNear(
    double           _actual,
    double           _expected,
    std::string_view _message
) {
    if (std::abs(_actual - _expected) > 1.0e-9) {
        throw std::runtime_error(std::string(_message));
    }
}

RHIQueueBinding QueueBinding(
    EQueueType   _queue,
    std::uint32_t _native_queue_id,
    std::uint32_t _family_id
) {
    return RHIQueueBinding{
        .queue = _queue,
        .native_queue_id = _native_queue_id,
        .family_id = _family_id,
        .available = true,
    };
}

TimestampQueryResult Timestamp(
    std::uint64_t _begin_tick,
    std::uint64_t _end_tick,
    double        _tick_period_ns = 1.0
) {
    Expect(
        _end_tick >= _begin_tick,
        "test timestamp end precedes begin"
    );
    return TimestampQueryResult{
        .begin_tick = _begin_tick,
        .end_tick = _end_tick,
        .valid_bits = 64,
        .tick_period_ns = _tick_period_ns,
        .duration_ns =
            static_cast<double>(_end_tick - _begin_tick) *
            _tick_period_ns,
    };
}

void ResolveTimestampAt(
    CmdSubmit&                  _submit,
    std::size_t                 _index,
    const TimestampQueryResult& _timestamp
) {
    Expect(
        _index < _submit.query_tokens.size(),
        "test query token index is out of range"
    );
    Expect(
        QueryBackendAccess::ResolveTimestamp(
            _submit.query_tokens[_index], _timestamp
        ),
        "timestamp query did not win its terminal transition"
    );
}

void ReleaseTerminalSubmit(CmdSubmit& _submit) {
    _submit.callbacks.clear();
    _submit.query_tokens.clear();
}

void RecordTimedScope(CommandList& _list, std::string_view _name) {
    _list.PushScopeWithTimeScope(_name);
    _list.PopScopeWithTimeScope();
}

GpuScopeStreamConfig SmallConfig() {
    return GpuScopeStreamConfig{
        .max_resident_frames = 8,
        .max_pending_frames = 8,
        .max_resident_scopes = 64,
        .max_scopes_per_frame = 16,
        .max_sources_per_frame = 8,
        .max_scope_name_bytes = 64,
        .max_error_reason_bytes = 128,
    };
}

void ReverseTerminalOrderBuildsNestedHierarchyAndExclusiveTime() {
    GpuScopeStream stream(SmallConfig());
    GpuScopeFrameHandle frame = stream.BeginFrame(1001);
    Expect(frame.Valid(), "nested frame admission failed");

    CommandList list(EQueueType::Graphics);
    list.SetGpuScopeRecorder(
        frame.CreateRecorder(
            QueueBinding(EQueueType::Graphics, 3, 7), 19
        )
    );
    Expect(
        list.HasGpuScopeRecorder(),
        "nested CommandList did not retain its recorder"
    );

    list.PushScopeWithTimeScope("Outer");
    list.PushScopeWithTimeScope("Child");
    list.PushScopeWithTimeScope("Grandchild");
    list.PopScopeWithTimeScope();
    list.PopScopeWithTimeScope();
    list.PopScopeWithTimeScope();
    CmdSubmit submit = list.Submit();

    Expect(
        submit.query_tokens.size() == 3,
        "nested scopes did not emit one query pair apiece"
    );
    Expect(
        stream.SealFrame(frame),
        "nested frame could not be sealed"
    );

    ResolvedGpuScopeFrame resolved{};
    ResolveTimestampAt(submit, 1, Timestamp(120, 180));
    ResolveTimestampAt(submit, 2, Timestamp(130, 150));
    Expect(
        !stream.TryPopFrame(resolved),
        "partially terminal nested frame became visible"
    );
    ResolveTimestampAt(submit, 0, Timestamp(100, 200));
    Expect(
        stream.TryPopFrame(resolved),
        "reverse-terminal nested frame never became visible"
    );

    Expect(
        resolved.valid && resolved.frame_id == 1001 &&
            resolved.admitted_scope_count == 3 &&
            resolved.dropped_scope_count == 0 &&
            resolved.error_scope_count == 0,
        "nested frame lost its validity or accounting"
    );
    Expect(
        resolved.queue_roots[0].size() == 1 &&
            resolved.queue_roots[1].empty() &&
            resolved.queue_roots[2].empty(),
        "nested frame was materialized in the wrong queue"
    );

    const GpuScopeNode& outer = resolved.queue_roots[0].front();
    Expect(
        outer.name == "Outer" && outer.depth == 0 &&
            outer.parent_scope_id == 0 &&
            outer.children.size() == 1,
        "outer scope topology is incorrect"
    );
    const GpuScopeNode& child = outer.children.front();
    Expect(
        child.name == "Child" && child.depth == 1 &&
            child.parent_scope_id == outer.scope_id &&
            child.children.size() == 1,
        "child scope topology is incorrect"
    );
    const GpuScopeNode& grandchild = child.children.front();
    Expect(
        grandchild.name == "Grandchild" &&
            grandchild.depth == 2 &&
            grandchild.parent_scope_id == child.scope_id &&
            grandchild.children.empty(),
        "grandchild scope topology is incorrect"
    );
    Expect(
        outer.scope_id != child.scope_id &&
            child.scope_id != grandchild.scope_id,
        "nested scopes reused a scope identity"
    );
    ExpectNear(outer.total_duration_ns, 100.0, "outer total duration changed");
    ExpectNear(outer.exclusive_duration_ns, 40.0, "outer exclusive duration is wrong");
    ExpectNear(child.total_duration_ns, 60.0, "child total duration changed");
    ExpectNear(child.exclusive_duration_ns, 40.0, "child exclusive duration is wrong");
    ExpectNear(grandchild.total_duration_ns, 20.0, "grandchild total duration changed");
    ExpectNear(
        grandchild.exclusive_duration_ns,
        20.0,
        "leaf exclusive duration is wrong"
    );
    Expect(
        outer.local_order < child.local_order &&
            child.local_order < grandchild.local_order &&
            outer.source_order == 19 &&
            child.source_order == 19,
        "source/local topology order was not stable"
    );

    const GpuScopeStreamStats stats = stream.GetStats();
    Expect(
        stats.frames_opened == 1 && stats.frames_sealed == 1 &&
            stats.frames_ready == 1 && stats.frames_popped == 1 &&
            stats.scopes_attempted == 3 &&
            stats.scopes_admitted == 3 &&
            stats.scopes_ready == 3 &&
            stats.resident_frames == 0 &&
            stats.resident_scopes == 0,
        "nested scope stream counters are inconsistent"
    );
    ReleaseTerminalSubmit(submit);
}

void FramesPopInBeginOrderAndSealIsOneShot() {
    GpuScopeStream stream(SmallConfig());
    GpuScopeFrameHandle first = stream.BeginFrame(2001);
    GpuScopeFrameHandle second = stream.BeginFrame(2002);
    Expect(first.Valid() && second.Valid(), "ordered frames were not admitted");
    Expect(
        !stream.BeginFrame(2001).Valid(),
        "duplicate resident frame id was admitted"
    );

    CommandList first_list(EQueueType::Graphics);
    first_list.SetGpuScopeRecorder(
        first.CreateRecorder(
            QueueBinding(EQueueType::Graphics, 0, 0), 0
        )
    );
    RecordTimedScope(first_list, "FirstFrame");
    CmdSubmit first_submit = first_list.Submit();

    CommandList second_list(EQueueType::Graphics);
    second_list.SetGpuScopeRecorder(
        second.CreateRecorder(
            QueueBinding(EQueueType::Graphics, 0, 0), 0
        )
    );
    RecordTimedScope(second_list, "SecondFrame");
    CmdSubmit second_submit = second_list.Submit();

    Expect(
        stream.SealFrame(first) && stream.SealFrame(second),
        "ordered frames could not be sealed"
    );
    Expect(
        !stream.SealFrame(first),
        "a frame was sealed more than once"
    );

    ResolveTimestampAt(second_submit, 0, Timestamp(20, 30));
    ResolvedGpuScopeFrame resolved{};
    Expect(
        !stream.TryPopFrame(resolved),
        "later frame bypassed an unresolved earlier frame"
    );
    ResolveTimestampAt(first_submit, 0, Timestamp(10, 15));
    Expect(
        stream.TryPopFrame(resolved) &&
            resolved.frame_id == 2001,
        "first admitted frame was not popped first"
    );
    Expect(
        stream.TryPopFrame(resolved) &&
            resolved.frame_id == 2002,
        "second admitted frame did not follow the first"
    );
    Expect(
        !stream.TryPopFrame(resolved),
        "empty stream produced a frame"
    );

    GpuScopeStream foreign_stream(SmallConfig());
    GpuScopeFrameHandle foreign = foreign_stream.BeginFrame(2003);
    Expect(
        !stream.SealFrame(foreign),
        "stream sealed a frame owned by another stream"
    );
    Expect(
        foreign_stream.SealFrame(foreign),
        "foreign empty frame could not be sealed by its owner"
    );
    Expect(
        foreign_stream.TryPopFrame(resolved) &&
            resolved.frame_id == 2003 && resolved.valid,
        "sealed empty frame did not preserve ordered visibility"
    );

    const GpuScopeStreamStats stats = stream.GetStats();
    Expect(
        stats.frames_dropped_duplicate_id == 1 &&
            stats.frames_opened == 2 &&
            stats.frames_ready == 2 &&
            stats.frames_popped == 2,
        "frame ordering counters are inconsistent"
    );
    ReleaseTerminalSubmit(first_submit);
    ReleaseTerminalSubmit(second_submit);
}

void ManagedQueuesRemainIndependentTimestampDomains() {
    GpuScopeStream stream(SmallConfig());
    GpuScopeFrameHandle frame = stream.BeginFrame(3001);

    const RHIQueueBinding graphics_binding =
        QueueBinding(EQueueType::Graphics, 11, 2);
    const RHIQueueBinding compute_binding =
        QueueBinding(EQueueType::Compute, 22, 5);
    const RHIQueueBinding copy_binding =
        QueueBinding(EQueueType::Copy, 33, 7);

    CommandList graphics(EQueueType::Graphics);
    graphics.SetGpuScopeRecorder(
        frame.CreateRecorder(graphics_binding, 7)
    );
    RecordTimedScope(graphics, "GraphicsRoot");
    CmdSubmit graphics_submit = graphics.Submit();

    CommandList compute(EQueueType::Compute);
    compute.SetGpuScopeRecorder(
        frame.CreateRecorder(compute_binding, 7)
    );
    RecordTimedScope(compute, "ComputeRoot");
    CmdSubmit compute_submit = compute.Submit();

    CommandList copy(EQueueType::Copy);
    copy.SetGpuScopeRecorder(
        frame.CreateRecorder(copy_binding, 7)
    );
    RecordTimedScope(copy, "CopyRoot");
    CmdSubmit copy_submit = copy.Submit();

    Expect(
        stream.SealFrame(frame),
        "multi-domain frame could not be sealed"
    );
    ResolveTimestampAt(copy_submit, 0, Timestamp(100, 108, 4.0));
    ResolveTimestampAt(compute_submit, 0, Timestamp(100, 110, 3.0));
    ResolveTimestampAt(graphics_submit, 0, Timestamp(100, 120, 2.0));

    ResolvedGpuScopeFrame resolved{};
    Expect(
        stream.TryPopFrame(resolved) && resolved.valid,
        "multi-domain frame did not resolve"
    );
    Expect(
        resolved.queue_roots[0].size() == 1 &&
            resolved.queue_roots[1].size() == 1 &&
            resolved.queue_roots[2].size() == 1,
        "managed-queue roots were not independently partitioned"
    );
    const GpuScopeNode& graphics_node =
        resolved.queue_roots[0].front();
    const GpuScopeNode& compute_node =
        resolved.queue_roots[1].front();
    const GpuScopeNode& copy_node =
        resolved.queue_roots[2].front();
    Expect(
        graphics_node.queue_binding == graphics_binding &&
            compute_node.queue_binding == compute_binding &&
            copy_node.queue_binding == copy_binding &&
            graphics_node.source_order == 7 &&
            compute_node.source_order == 7 &&
            copy_node.source_order == 7,
        "queue-domain identity or per-domain source order was lost"
    );
    ExpectNear(
        graphics_node.total_duration_ns,
        40.0,
        "Graphics duration was converted through another domain"
    );
    ExpectNear(
        compute_node.total_duration_ns,
        30.0,
        "Compute duration was converted through another domain"
    );
    ExpectNear(
        copy_node.total_duration_ns,
        32.0,
        "Copy duration was converted through another domain"
    );
    ExpectNear(
        graphics_node.exclusive_duration_ns,
        40.0,
        "Graphics root exclusive duration is wrong"
    );
    ExpectNear(
        compute_node.exclusive_duration_ns,
        30.0,
        "Compute root exclusive duration is wrong"
    );
    ExpectNear(
        copy_node.exclusive_duration_ns,
        32.0,
        "Copy root exclusive duration is wrong"
    );

    ReleaseTerminalSubmit(graphics_submit);
    ReleaseTerminalSubmit(compute_submit);
    ReleaseTerminalSubmit(copy_submit);
}

void ErrorCancelAndRejectAreExactlyOnce() {
    {
        GpuScopeStream stream(SmallConfig());
        GpuScopeFrameHandle frame = stream.BeginFrame(4001);
        CommandList list(EQueueType::Graphics);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        RecordTimedScope(list, "DirectError");
        CmdSubmit submit = list.Submit();
        QueryToken token = submit.query_tokens.front();
        Expect(stream.SealFrame(frame), "error frame seal failed");

        Expect(
            QueryBackendAccess::ResolveErrorIfPending(
                token, "injected direct query error"
            ),
            "direct query error did not win"
        );
        Expect(
            !QueryBackendAccess::ResolveTimestamp(
                token, Timestamp(1, 2)
            ) &&
                !QueryBackendAccess::ResolveErrorIfPending(
                    token, "duplicate error"
                ),
            "direct error was overwritten by a later terminal"
        );
        for (auto& callback : submit.callbacks) {
            callback();
            callback();
        }
        submit.callbacks.clear();

        ResolvedGpuScopeFrame resolved{};
        Expect(
            stream.TryPopFrame(resolved) &&
                !resolved.valid &&
                resolved.error_scope_count == 1 &&
                resolved.queue_roots[0].size() == 1 &&
                resolved.queue_roots[0].front().status ==
                    GpuScopeTerminalStatus::Error &&
                resolved.queue_roots[0].front().error_reason ==
                    "injected direct query error",
            "direct Error was not materialized exactly once"
        );
        const GpuScopeStreamStats stats = stream.GetStats();
        Expect(
            stats.scopes_error == 1 && stats.scopes_ready == 0,
            "direct Error changed stream counters more than once"
        );
        submit.query_tokens.clear();
    }

    {
        GpuScopeStream stream(SmallConfig());
        GpuScopeFrameHandle frame = stream.BeginFrame(4002);
        CommandList list(EQueueType::Graphics);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        QueryCancellationView cancellation =
            list.GetQueryCancellationView();
        RecordTimedScope(list, "Cancelled");
        Expect(
            cancellation.Cancel("injected generation cancellation"),
            "query generation cancellation did not win"
        );
        Expect(
            !cancellation.Cancel("duplicate cancellation"),
            "query cancellation was not one-shot"
        );
        CmdSubmit submit = list.Submit();
        QueryToken token = submit.query_tokens.front();
        Expect(stream.SealFrame(frame), "cancelled frame seal failed");

        ResolvedGpuScopeFrame resolved{};
        Expect(
            !stream.TryPopFrame(resolved),
            "cancelled callback escaped before ownership handoff"
        );
        submit.RejectPendingQueries("late submit rejection");
        Expect(
            !QueryBackendAccess::ResolveErrorIfPending(
                token, "late duplicate cancellation"
            ),
            "cancelled query accepted another terminal transition"
        );
        Expect(
            stream.TryPopFrame(resolved) &&
                resolved.error_scope_count == 1 &&
                resolved.queue_roots[0].front().error_reason ==
                    "injected generation cancellation",
            "deferred cancellation did not resolve exactly once"
        );
        Expect(
            stream.GetStats().scopes_error == 1,
            "cancelled scope incremented Error more than once"
        );
        submit.callbacks.clear();
    }

    {
        GpuScopeStream stream(SmallConfig());
        GpuScopeFrameHandle frame = stream.BeginFrame(4003);
        CommandList list(EQueueType::Graphics);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        RecordTimedScope(list, "Rejected");
        CmdSubmit submit = list.Submit();
        QueryToken token = submit.query_tokens.front();
        Expect(stream.SealFrame(frame), "rejected frame seal failed");

        submit.RejectPendingQueries("injected submit rejection");
        submit.RejectPendingQueries("duplicate submit rejection");
        for (auto& callback : submit.callbacks) {
            callback();
        }
        submit.callbacks.clear();
        Expect(
            !QueryBackendAccess::ResolveTimestamp(
                token, Timestamp(10, 20)
            ),
            "rejected query accepted a late Ready result"
        );

        ResolvedGpuScopeFrame resolved{};
        Expect(
            stream.TryPopFrame(resolved) &&
                resolved.error_scope_count == 1 &&
                resolved.queue_roots[0].front().error_reason ==
                    "injected submit rejection" &&
                stream.GetStats().scopes_error == 1,
            "submit rejection did not resolve exactly once"
        );
    }
}

void MalformedTimestampPayloadAndTopologyFailClosed() {
    {
        GpuScopeStream stream(SmallConfig());
        GpuScopeFrameHandle frame = stream.BeginFrame(4501);
        CommandList list(EQueueType::Graphics);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        RecordTimedScope(list, "MalformedDuration");
        CmdSubmit submit = list.Submit();
        Expect(stream.SealFrame(frame), "malformed-duration frame seal failed");

        TimestampQueryResult malformed = Timestamp(0, 10);
        malformed.duration_ns = 999.0;
        ResolveTimestampAt(submit, 0, malformed);

        ResolvedGpuScopeFrame resolved{};
        Expect(
            stream.TryPopFrame(resolved) &&
                !resolved.valid &&
                resolved.error_scope_count == 1 &&
                resolved.queue_roots[0].front().status ==
                    GpuScopeTerminalStatus::Error &&
                resolved.queue_roots[0].front().error_reason ==
                    "GPU scope timestamp payload is invalid",
            "duration/raw-tick mismatch was accepted as a valid GPU scope"
        );
        ReleaseTerminalSubmit(submit);
    }

    {
        GpuScopeStream stream(SmallConfig());
        GpuScopeFrameHandle frame = stream.BeginFrame(4502);
        CommandList list(EQueueType::Graphics);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        list.PushScopeWithTimeScope("Parent");
        RecordTimedScope(list, "OversizedChild");
        list.PopScopeWithTimeScope();
        CmdSubmit submit = list.Submit();
        Expect(stream.SealFrame(frame), "invalid-topology frame seal failed");

        ResolveTimestampAt(submit, 0, Timestamp(0, 10));
        ResolveTimestampAt(submit, 1, Timestamp(0, 20));

        ResolvedGpuScopeFrame resolved{};
        Expect(
            stream.TryPopFrame(resolved) &&
                !resolved.valid &&
                resolved.error_scope_count == 0 &&
                resolved.queue_roots[0].size() == 1 &&
                resolved.queue_roots[0].front().
                    exclusive_duration_ns == 0.0,
            "child interval outside its parent was not failed closed"
        );
        ReleaseTerminalSubmit(submit);
    }

    {
        GpuScopeStream stream(SmallConfig());
        GpuScopeFrameHandle frame = stream.BeginFrame(4503);
        CommandList list(EQueueType::Graphics);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        list.PushScopeWithTimeScope("Parent");
        RecordTimedScope(list, "FirstChild");
        RecordTimedScope(list, "OverlappingChild");
        list.PopScopeWithTimeScope();
        CmdSubmit submit = list.Submit();
        Expect(stream.SealFrame(frame), "overlap frame seal failed");

        ResolveTimestampAt(submit, 0, Timestamp(0, 100));
        ResolveTimestampAt(submit, 1, Timestamp(10, 60));
        ResolveTimestampAt(submit, 2, Timestamp(50, 80));

        ResolvedGpuScopeFrame resolved{};
        Expect(
            stream.TryPopFrame(resolved) &&
                !resolved.valid &&
                resolved.error_scope_count == 0 &&
                resolved.queue_roots[0].front().
                    exclusive_duration_ns == 0.0,
            "overlapping sibling intervals were not failed closed"
        );
        ReleaseTerminalSubmit(submit);
    }

    {
        GpuScopeStream stream(SmallConfig());
        GpuScopeFrameHandle frame = stream.BeginFrame(4504);
        CommandList list(EQueueType::Graphics);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        RecordTimedScope(list, "RootA");
        RecordTimedScope(list, "OverlappingRootB");
        CmdSubmit submit = list.Submit();
        Expect(stream.SealFrame(frame), "root-overlap frame seal failed");

        ResolveTimestampAt(submit, 0, Timestamp(0, 100));
        ResolveTimestampAt(submit, 1, Timestamp(50, 80));

        ResolvedGpuScopeFrame resolved{};
        Expect(
            stream.TryPopFrame(resolved) &&
                !resolved.valid &&
                resolved.queue_roots[0].size() == 2 &&
                resolved.queue_roots[0][0].
                    exclusive_duration_ns == 0.0 &&
                resolved.queue_roots[0][1].
                    exclusive_duration_ns == 0.0,
            "overlapping roots from one source were not failed closed"
        );
        ReleaseTerminalSubmit(submit);
    }

    {
        GpuScopeStream stream(SmallConfig());
        GpuScopeFrameHandle frame = stream.BeginFrame(4505);
        CommandList list(EQueueType::Graphics);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        RecordTimedScope(list, "WrappedRootA");
        RecordTimedScope(list, "WrappedRootB");
        CmdSubmit submit = list.Submit();
        Expect(stream.SealFrame(frame), "wrapped-root frame seal failed");

        ResolveTimestampAt(
            submit,
            0,
            TimestampQueryResult{
                .begin_tick = 250,
                .end_tick = 5,
                .valid_bits = 8,
                .tick_period_ns = 1.0,
                .duration_ns = 11.0,
            }
        );
        ResolveTimestampAt(
            submit,
            1,
            TimestampQueryResult{
                .begin_tick = 10,
                .end_tick = 20,
                .valid_bits = 8,
                .tick_period_ns = 1.0,
                .duration_ns = 10.0,
            }
        );

        ResolvedGpuScopeFrame resolved{};
        Expect(
            stream.TryPopFrame(resolved) &&
                resolved.valid &&
                resolved.queue_roots[0].size() == 2 &&
                resolved.queue_roots[0][0].
                    exclusive_duration_ns == 11.0 &&
                resolved.queue_roots[0][1].
                    exclusive_duration_ns == 10.0,
            "valid wrapped roots from one source were rejected"
        );
        ReleaseTerminalSubmit(submit);
    }

    {
        GpuScopeStream stream(SmallConfig());
        GpuScopeFrameHandle frame = stream.BeginFrame(4506);
        CommandList list(EQueueType::Graphics);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        list.PushScopeWithTimeScope("WrappedParent");
        RecordTimedScope(list, "WrappedChild");
        list.PopScopeWithTimeScope();
        CmdSubmit submit = list.Submit();
        Expect(stream.SealFrame(frame), "wrapped frame seal failed");

        ResolveTimestampAt(
            submit,
            0,
            TimestampQueryResult{
                .begin_tick = 250,
                .end_tick = 20,
                .valid_bits = 8,
                .tick_period_ns = 1.0,
                .duration_ns = 26.0,
            }
        );
        ResolveTimestampAt(
            submit,
            1,
            TimestampQueryResult{
                .begin_tick = 255,
                .end_tick = 5,
                .valid_bits = 8,
                .tick_period_ns = 1.0,
                .duration_ns = 6.0,
            }
        );

        ResolvedGpuScopeFrame resolved{};
        Expect(
            stream.TryPopFrame(resolved) &&
                resolved.valid &&
                resolved.queue_roots[0].front().
                    exclusive_duration_ns == 20.0,
            "valid wrapped timestamp topology was rejected"
        );
        ReleaseTerminalSubmit(submit);
    }
}

void ClosedModernStreamNeverFallsBackToLegacyProfiling() {
    GpuScopeStream stream(SmallConfig());
    GpuScopeFrameHandle frame = stream.BeginFrame(4601);
    CommandList list(EQueueType::Graphics);
    list.SetGpuScopeRecorder(
        frame.CreateRecorder(
            QueueBinding(EQueueType::Graphics, 0, 0), 0
        )
    );
    RecordTimedScope(list, "BeforeClose");
    stream.Close();
    Expect(
        list.HasGpuScopeRecorder(),
        "closed stream erased the explicit modern recorder binding"
    );
    RecordTimedScope(list, "AfterClose");

    CmdSubmit submit = list.Submit();
    Expect(
        submit.query_tokens.size() == 1,
        "closed modern stream recorded another native query"
    );
    std::size_t legacy_timestamp_markers = 0;
    for (const auto& command : submit.cmds) {
        if (command->Type() != Command::EType::Scope) {
            continue;
        }
        const auto& scope = *static_cast<const ScopeCmd*>(command.get());
        if (scope.QueryTimestamp()) {
            ++legacy_timestamp_markers;
        }
    }
    Expect(
        legacy_timestamp_markers == 0,
        "closed modern stream silently fell back to legacy profiling"
    );
    submit.RejectPendingQueries("closed stream test cleanup");
    submit.callbacks.clear();
}

void RecorderBindingIsImmutableUntilGenerationReset() {
    {
        GpuScopeStream rejected_stream(SmallConfig());
        GpuScopeFrameHandle sealed_frame =
            rejected_stream.BeginFrame(4700);
        Expect(
            rejected_stream.SealFrame(sealed_frame),
            "empty-recorder frame could not be sealed"
        );
        CommandList rejected_list(EQueueType::Graphics);
        bool invalid_recorder_rejected = false;
        try {
            rejected_list.SetGpuScopeRecorder(
                sealed_frame.CreateRecorder(
                    QueueBinding(EQueueType::Graphics, 0, 0), 0
                )
            );
        } catch (const std::invalid_argument&) {
            invalid_recorder_rejected = true;
        }
        ResolvedGpuScopeFrame rejected_frame{};
        Expect(
            invalid_recorder_rejected &&
                !rejected_list.HasGpuScopeRecorder() &&
                rejected_stream.TryPopFrame(rejected_frame) &&
                rejected_frame.valid &&
                rejected_frame.admitted_scope_count == 0,
            "failed recorder creation silently selected legacy profiling"
        );
    }

    GpuScopeStream stream(SmallConfig());
    GpuScopeFrameHandle first_frame = stream.BeginFrame(4701);
    CommandList list(EQueueType::Graphics);
    const RHIQueueBinding binding =
        QueueBinding(EQueueType::Graphics, 0, 0);
    list.SetGpuScopeRecorder(first_frame.CreateRecorder(binding, 0));
    RecordTimedScope(list, "FirstSource");

    bool clear_rejected = false;
    try {
        list.SetGpuScopeRecorder({});
    } catch (const std::logic_error&) {
        clear_rejected = true;
    }
    bool rebind_rejected = false;
    try {
        list.SetGpuScopeRecorder(
            first_frame.CreateRecorder(binding, 1)
        );
    } catch (const std::logic_error&) {
        rebind_rejected = true;
    }
    Expect(
        clear_rejected && rebind_rejected &&
            list.HasGpuScopeRecorder(),
        "one CommandList generation changed its stable GPU scope source"
    );

    CmdSubmit first_submit = list.Submit();
    Expect(
        stream.SealFrame(first_frame),
        "immutable-binding first frame seal failed"
    );
    ResolveTimestampAt(first_submit, 0, Timestamp(0, 10));
    ResolvedGpuScopeFrame resolved{};
    Expect(
        stream.TryPopFrame(resolved) && resolved.valid,
        "immutable-binding first frame did not drain"
    );
    ReleaseTerminalSubmit(first_submit);

    GpuScopeFrameHandle second_frame = stream.BeginFrame(4702);
    list.SetGpuScopeRecorder(
        second_frame.CreateRecorder(binding, 0)
    );
    RecordTimedScope(list, "NextGeneration");
    CmdSubmit second_submit = list.Submit();
    Expect(
        stream.SealFrame(second_frame),
        "immutable-binding next frame seal failed"
    );
    ResolveTimestampAt(second_submit, 0, Timestamp(10, 20));
    Expect(
        stream.TryPopFrame(resolved) && resolved.valid &&
            resolved.frame_id == 4702,
        "Submit did not reset the recorder-binding generation"
    );
    ReleaseTerminalSubmit(second_submit);

    GpuScopeFrameHandle legacy_mixing_frame =
        stream.BeginFrame(4703);
    CommandList legacy_first(EQueueType::Graphics);
    RecordTimedScope(legacy_first, "LegacyBeforeModernBinding");
    bool legacy_to_modern_rejected = false;
    try {
        legacy_first.SetGpuScopeRecorder(
            legacy_mixing_frame.CreateRecorder(binding, 0)
        );
    } catch (const std::logic_error&) {
        legacy_to_modern_rejected = true;
    }
    Expect(
        legacy_to_modern_rejected &&
            !legacy_first.HasGpuScopeRecorder(),
        "a CommandList generation mixed legacy and modern timed scopes"
    );
    CmdSubmit legacy_submit = legacy_first.Submit();
    std::size_t legacy_timestamp_markers = 0;
    for (const auto& command : legacy_submit.cmds) {
        if (command->Type() == Command::EType::Scope &&
            static_cast<const ScopeCmd*>(command.get())->
                QueryTimestamp()) {
            ++legacy_timestamp_markers;
        }
    }
    Expect(
        legacy_timestamp_markers == 2 &&
            stream.SealFrame(legacy_mixing_frame) &&
            stream.TryPopFrame(resolved) && resolved.valid &&
            resolved.admitted_scope_count == 0,
        "legacy-to-modern rejection corrupted either profiling path"
    );
    ReleaseTerminalSubmit(legacy_submit);
}

void SealWaitsForAdmissionsThatAlreadyStarted() {
    constexpr std::size_t producer_count = 4;

    GpuScopeStreamConfig config = SmallConfig();
    config.max_resident_scopes = producer_count;
    config.max_scopes_per_frame = producer_count;
    config.max_sources_per_frame = producer_count;
    GpuScopeStream stream(config);
    GpuScopeFrameHandle frame = stream.BeginFrame(4801);
    Expect(frame.Valid(), "concurrent-admission frame was rejected");

    std::array<std::unique_ptr<CommandList>, producer_count> lists{};
    for (std::size_t index = 0; index < producer_count; ++index) {
        lists[index] = std::make_unique<CommandList>(
            EQueueType::Graphics
        );
        lists[index]->SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0),
                static_cast<std::uint64_t>(index)
            )
        );
    }

    std::array<std::exception_ptr, producer_count> errors{};
    std::array<std::thread, producer_count> producers{};
    std::atomic_bool start{false};
    std::atomic_uint32_t admission_pause_count{0};
    std::atomic_bool     release_admissions{false};
    GpuScopeTesting::InstallAdmissionPause(
        admission_pause_count, release_admissions
    );
    for (std::size_t index = 0; index < producer_count; ++index) {
        producers[index] = std::thread([&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                RecordTimedScope(
                    *lists[index], "ConcurrentPreSealScope"
                );
            } catch (...) {
                errors[index] = std::current_exception();
            }
        });
    }

    ResolvedGpuScopeFrame resolved{};
    std::atomic_bool      popped{false};
    std::atomic_bool      stop_consumer{false};
    std::thread consumer([&] {
        while (!stop_consumer.load(std::memory_order_acquire)) {
            if (stream.TryPopFrame(resolved)) {
                popped.store(true, std::memory_order_release);
                return;
            }
            std::this_thread::yield();
        }
    });

    start.store(true, std::memory_order_release);
    bool all_paused = false;
    for (std::size_t spin = 0; spin < 2'000'000; ++spin) {
        if (admission_pause_count.load(std::memory_order_acquire) ==
            producer_count) {
            all_paused = true;
            break;
        }
        std::this_thread::yield();
    }
    const bool sealed = stream.SealFrame(frame);
    release_admissions.store(true, std::memory_order_release);

    for (std::thread& producer : producers) {
        producer.join();
    }
    GpuScopeTesting::ClearAdmissionPause();
    std::exception_ptr producer_error{};
    for (const std::exception_ptr& error : errors) {
        if (error && !producer_error) {
            producer_error = error;
        }
    }
    if (!all_paused || !sealed || producer_error) {
        stop_consumer.store(true, std::memory_order_release);
        consumer.join();
        stream.Close();
        if (producer_error) {
            std::rethrow_exception(producer_error);
        }
    }
    Expect(
        all_paused,
        "scope producers did not reach the deterministic admission pause"
    );
    Expect(
        sealed,
        "concurrent-admission frame could not be sealed"
    );
    std::array<std::optional<CmdSubmit>, producer_count> submits{};
    bool all_queries_recorded = true;
    for (std::size_t index = 0; index < producer_count; ++index) {
        submits[index].emplace(lists[index]->Submit());
        if (submits[index]->query_tokens.size() != 1) {
            all_queries_recorded = false;
            continue;
        }
        all_queries_recorded &=
            QueryBackendAccess::ResolveTimestamp(
                submits[index]->query_tokens.front(),
                Timestamp(
                    static_cast<std::uint64_t>(index * 10),
                    static_cast<std::uint64_t>(index * 10 + 5)
                )
            );
    }

    for (std::size_t spin = 0; spin < 2'000'000; ++spin) {
        if (popped.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::yield();
    }
    stop_consumer.store(true, std::memory_order_release);
    consumer.join();
    Expect(
        all_queries_recorded &&
            popped.load(std::memory_order_acquire) && resolved.valid &&
            resolved.admitted_scope_count == producer_count &&
            resolved.dropped_scope_count == 0,
        "Seal rejected or exposed a frame before its pre-Seal admissions completed"
    );
    for (auto& submit : submits) {
        ReleaseTerminalSubmit(*submit);
    }
}

void FrameAdmissionCapacityIsBoundedAndRecovers() {
    GpuScopeStreamConfig config = SmallConfig();
    config.max_resident_frames = 2;
    config.max_pending_frames = 1;
    GpuScopeStream stream(config);

    GpuScopeFrameHandle first = stream.BeginFrame(5001);
    Expect(first.Valid(), "first bounded frame was rejected");
    Expect(
        !stream.BeginFrame(5002).Valid(),
        "pending-frame capacity was exceeded"
    );
    Expect(
        stream.SealFrame(first),
        "empty bounded frame could not be sealed"
    );

    GpuScopeFrameHandle second = stream.BeginFrame(5002);
    Expect(
        second.Valid(),
        "pending-frame capacity did not recover after seal"
    );
    Expect(
        !stream.BeginFrame(5003).Valid(),
        "resident-frame capacity was exceeded"
    );

    ResolvedGpuScopeFrame resolved{};
    Expect(
        stream.TryPopFrame(resolved) && resolved.frame_id == 5001,
        "first bounded frame did not release its resident slot"
    );
    Expect(
        stream.SealFrame(second),
        "second bounded frame could not be sealed"
    );
    GpuScopeFrameHandle third = stream.BeginFrame(5003);
    Expect(
        third.Valid(),
        "resident/pending frame capacity did not recover after pop/seal"
    );

    Expect(
        stream.TryPopFrame(resolved) && resolved.frame_id == 5002,
        "second bounded frame did not pop"
    );
    Expect(
        stream.SealFrame(third),
        "third bounded frame could not be sealed"
    );
    Expect(
        stream.TryPopFrame(resolved) && resolved.frame_id == 5003,
        "third bounded frame did not pop"
    );

    const GpuScopeStreamStats stats = stream.GetStats();
    Expect(
        stats.frames_dropped_pending_full == 1 &&
            stats.frames_dropped_resident_full == 1 &&
            stats.frames_opened == 3 &&
            stats.frames_popped == 3 &&
            stats.high_water_frames == 2 &&
            stats.high_water_pending_frames == 1,
        "frame admission capacity counters are inconsistent"
    );
}

void ConcurrentDuplicateFrameAdmissionReservesExactlyOneSlot() {
    GpuScopeStreamConfig config = SmallConfig();
    config.max_resident_frames = 32;
    config.max_pending_frames = 32;
    config.max_resident_scopes = 32;
    config.max_scopes_per_frame = 1;
    config.max_sources_per_frame = 1;

    GpuScopeStream stream(config);
    constexpr std::size_t contender_count = 32;
    Moer::Array<GpuScopeFrameHandle> handles(contender_count);
    Moer::Array<std::thread> threads{};
    threads.reserve(contender_count);
    std::atomic<std::size_t> arrived{0};
    std::atomic<bool> start{false};

    for (std::size_t index = 0; index < contender_count; ++index) {
        threads.emplace_back([&, index] {
            arrived.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            handles[index] = stream.BeginFrame(5050);
        });
    }
    while (arrived.load(std::memory_order_acquire) != contender_count) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::size_t admitted = 0;
    GpuScopeFrameHandle admitted_frame{};
    for (const GpuScopeFrameHandle& handle : handles) {
        if (handle.Valid()) {
            ++admitted;
            admitted_frame = handle;
        }
    }
    Expect(
        admitted == 1 && admitted_frame.Valid(),
        "concurrent duplicate BeginFrame admitted more than one reservation"
    );

    const GpuScopeStreamStats admitted_stats = stream.GetStats();
    Expect(
        admitted_stats.frames_opened == 1 &&
            admitted_stats.frames_dropped_duplicate_id ==
                contender_count - 1 &&
            admitted_stats.resident_frames == 1 &&
            admitted_stats.resident_pending_frames == 1,
        "concurrent duplicate frame-reservation counters are inconsistent"
    );

    Expect(
        stream.SealFrame(admitted_frame),
        "concurrent duplicate admitted frame seal failed"
    );
    ResolvedGpuScopeFrame resolved{};
    Expect(
        stream.TryPopFrame(resolved) &&
            resolved.frame_id == 5050 &&
            resolved.valid,
        "concurrent duplicate admitted frame did not drain"
    );
}

void PerFrameAndGlobalScopeCapacityAreBoundedAndRecover() {
    {
        GpuScopeStreamConfig config = SmallConfig();
        config.max_resident_scopes = 4;
        config.max_scopes_per_frame = 2;
        GpuScopeStream stream(config);
        GpuScopeFrameHandle frame = stream.BeginFrame(5101);
        CommandList list(EQueueType::Graphics);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        RecordTimedScope(list, "FrameA");
        RecordTimedScope(list, "FrameB");
        RecordTimedScope(list, "FrameOverflow");
        CmdSubmit submit = list.Submit();
        Expect(
            submit.query_tokens.size() == 2,
            "per-frame overflow emitted an unbounded query"
        );
        Expect(stream.SealFrame(frame), "per-frame capacity frame seal failed");
        ResolveTimestampAt(submit, 0, Timestamp(0, 4));
        ResolveTimestampAt(submit, 1, Timestamp(5, 9));

        ResolvedGpuScopeFrame resolved{};
        Expect(
            stream.TryPopFrame(resolved) &&
                !resolved.valid &&
                resolved.admitted_scope_count == 2 &&
                resolved.dropped_scope_count == 1 &&
                resolved.queue_roots[0].size() == 2,
            "per-frame overflow accounting is incorrect"
        );
        ExpectNear(
            resolved.queue_roots[0][0].exclusive_duration_ns,
            0.0,
            "partial frame exposed trustworthy exclusive time"
        );
        const GpuScopeStreamStats stats = stream.GetStats();
        Expect(
            stats.scopes_attempted == 3 &&
                stats.scopes_admitted == 2 &&
                stats.scopes_dropped_frame_full == 1 &&
                stats.scopes_dropped_suppressed_subtree == 0 &&
                stats.resident_scopes == 0,
            "per-frame scope capacity counters are inconsistent"
        );
        ReleaseTerminalSubmit(submit);

        GpuScopeFrameHandle recovered = stream.BeginFrame(5102);
        CommandList recovered_list(EQueueType::Graphics);
        recovered_list.SetGpuScopeRecorder(
            recovered.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        RecordTimedScope(recovered_list, "RecoveredFrameScope");
        CmdSubmit recovered_submit = recovered_list.Submit();
        Expect(
            recovered_submit.query_tokens.size() == 1,
            "per-frame capacity did not recover in the next frame"
        );
        Expect(stream.SealFrame(recovered), "recovered frame seal failed");
        ResolveTimestampAt(
            recovered_submit, 0, Timestamp(10, 11)
        );
        Expect(
            stream.TryPopFrame(resolved) && resolved.valid,
            "scope admission did not recover after a bounded frame"
        );
        ReleaseTerminalSubmit(recovered_submit);
    }

    {
        GpuScopeStreamConfig config = SmallConfig();
        config.max_resident_scopes = 2;
        config.max_scopes_per_frame = 2;
        GpuScopeStream stream(config);
        GpuScopeFrameHandle first = stream.BeginFrame(5201);
        GpuScopeFrameHandle second = stream.BeginFrame(5202);

        CommandList first_list(EQueueType::Graphics);
        first_list.SetGpuScopeRecorder(
            first.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        RecordTimedScope(first_list, "GlobalA");
        RecordTimedScope(first_list, "GlobalB");
        CmdSubmit first_submit = first_list.Submit();

        CommandList second_list(EQueueType::Graphics);
        second_list.SetGpuScopeRecorder(
            second.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        RecordTimedScope(second_list, "GlobalOverflow");
        Expect(
            second_list.IsEmpty() == false,
            "overflow visual markers unexpectedly disappeared"
        );

        Expect(stream.SealFrame(first), "global-capacity first seal failed");
        ResolveTimestampAt(first_submit, 0, Timestamp(0, 1));
        ResolveTimestampAt(first_submit, 1, Timestamp(1, 2));
        ResolvedGpuScopeFrame resolved{};
        Expect(
            stream.TryPopFrame(resolved) && resolved.valid,
            "first global-capacity frame did not release scopes"
        );
        ReleaseTerminalSubmit(first_submit);

        RecordTimedScope(second_list, "GlobalRecovered");
        CmdSubmit second_submit = second_list.Submit();
        Expect(
            second_submit.query_tokens.size() == 1,
            "global scope capacity did not recover after pop"
        );
        Expect(stream.SealFrame(second), "global-capacity second seal failed");
        ResolveTimestampAt(second_submit, 0, Timestamp(2, 4));
        Expect(
            stream.TryPopFrame(resolved) &&
                !resolved.valid &&
                resolved.admitted_scope_count == 1 &&
                resolved.dropped_scope_count == 1,
            "global overflow/recovery accounting is incorrect"
        );
        const GpuScopeStreamStats stats = stream.GetStats();
        Expect(
            stats.scopes_dropped_resident_full == 1 &&
                stats.scopes_admitted == 3 &&
                stats.resident_scopes == 0,
            "global scope capacity counters are inconsistent"
        );
        ReleaseTerminalSubmit(second_submit);
    }
}

void CapacityFailureSuppressesTheWholeNestedSubtree() {
    GpuScopeStreamConfig config = SmallConfig();
    config.max_resident_scopes = 4;
    config.max_scopes_per_frame = 1;
    GpuScopeStream stream(config);
    GpuScopeFrameHandle frame = stream.BeginFrame(5301);

    CommandList list(EQueueType::Graphics);
    list.SetGpuScopeRecorder(
        frame.CreateRecorder(
            QueueBinding(EQueueType::Graphics, 0, 0), 0
        )
    );
    RecordTimedScope(list, "Admitted");
    list.PushScopeWithTimeScope("OverflowParent");
    list.PushScopeWithTimeScope("SuppressedChild");
    list.PushScopeWithTimeScope("SuppressedGrandchild");
    list.PopScopeWithTimeScope();
    list.PopScopeWithTimeScope();
    list.PopScopeWithTimeScope();
    CmdSubmit submit = list.Submit();

    Expect(
        submit.query_tokens.size() == 1,
        "suppressed subtree emitted hidden query pairs"
    );
    Expect(
        stream.SealFrame(frame),
        "suppressed-subtree frame could not be sealed"
    );
    ResolveTimestampAt(submit, 0, Timestamp(0, 1));

    ResolvedGpuScopeFrame resolved{};
    Expect(
        stream.TryPopFrame(resolved) &&
            !resolved.valid &&
            resolved.admitted_scope_count == 1 &&
            resolved.dropped_scope_count == 3 &&
            resolved.queue_roots[0].size() == 1 &&
            resolved.queue_roots[0].front().name == "Admitted",
        "suppressed subtree leaked partial hierarchy into the frame"
    );
    const GpuScopeStreamStats stats = stream.GetStats();
    Expect(
        stats.scopes_attempted == 4 &&
            stats.scopes_admitted == 1 &&
            stats.scopes_dropped_frame_full == 1 &&
            stats.scopes_dropped_suppressed_subtree == 2 &&
            stats.resident_scopes == 0,
        "suppressed-subtree accounting or permits are inconsistent"
    );
    ReleaseTerminalSubmit(submit);
}

void DuplicateNamesAndSourceCapacityRemainUnambiguous() {
    GpuScopeStreamConfig config = SmallConfig();
    config.max_sources_per_frame = 2;
    GpuScopeStream stream(config);
    GpuScopeFrameHandle frame = stream.BeginFrame(6001);
    const RHIQueueBinding graphics =
        QueueBinding(EQueueType::Graphics, 0, 0);

    GpuScopeRecorder recorder = frame.CreateRecorder(graphics, 4);
    Expect(recorder.Valid(), "primary source recorder was rejected");
    Expect(
        !frame.CreateRecorder(graphics, 4).Valid(),
        "duplicate queue/source-order recorder was admitted"
    );
    Expect(
        frame.CreateRecorder(graphics, 5).Valid(),
        "second unique source recorder was rejected"
    );
    Expect(
        !frame.CreateRecorder(graphics, 6).Valid(),
        "source capacity was exceeded"
    );

    CommandList list(EQueueType::Graphics);
    list.SetGpuScopeRecorder(std::move(recorder));
    RecordTimedScope(list, "DuplicateName");
    RecordTimedScope(list, "DuplicateName");
    CmdSubmit submit = list.Submit();
    Expect(
        submit.query_tokens.size() == 2 &&
            submit.query_tokens[0].Name() == "DuplicateName" &&
            submit.query_tokens[1].Name() == "DuplicateName",
        "modern duplicate names were rejected or aliased"
    );
    Expect(stream.SealFrame(frame), "duplicate-name frame seal failed");
    ResolveTimestampAt(submit, 1, Timestamp(20, 22));
    ResolveTimestampAt(submit, 0, Timestamp(10, 11));

    ResolvedGpuScopeFrame resolved{};
    Expect(
        stream.TryPopFrame(resolved) && resolved.valid &&
            resolved.queue_roots[0].size() == 2,
        "duplicate-name roots did not materialize independently"
    );
    const GpuScopeNode& first = resolved.queue_roots[0][0];
    const GpuScopeNode& second = resolved.queue_roots[0][1];
    Expect(
        first.name == "DuplicateName" &&
            second.name == "DuplicateName" &&
            first.scope_id != second.scope_id &&
            first.local_order < second.local_order,
        "duplicate names lost independent identity/order"
    );
    const GpuScopeStreamStats stats = stream.GetStats();
    Expect(
        stats.sources_opened == 2 &&
            stats.sources_dropped_duplicate_order == 1 &&
            stats.sources_dropped_capacity == 1,
        "source capacity/duplicate counters are inconsistent"
    );
    ReleaseTerminalSubmit(submit);
}

void CommandListMoveAndReplacementPreserveTerminalOwnership() {
    GpuScopeStream stream(SmallConfig());
    GpuScopeFrameHandle replaced_frame = stream.BeginFrame(7001);
    GpuScopeFrameHandle incoming_frame = stream.BeginFrame(7002);

    CommandList destination(EQueueType::Graphics);
    destination.SetGpuScopeRecorder(
        replaced_frame.CreateRecorder(
            QueueBinding(EQueueType::Graphics, 0, 0), 0
        )
    );
    destination.PushScopeWithTimeScope("ReplacedListScope");

    CommandList source(EQueueType::Graphics);
    source.SetGpuScopeRecorder(
        incoming_frame.CreateRecorder(
            QueueBinding(EQueueType::Graphics, 0, 0), 0
        )
    );
    source.PushScopeWithTimeScope("MovedOpenScope");

    destination = std::move(source);
    Expect(
        destination.HasGpuScopeRecorder() &&
            !source.HasGpuScopeRecorder(),
        "CommandList move assignment lost or duplicated its recorder"
    );
    destination.PopScopeWithTimeScope();
    CmdSubmit incoming_submit = destination.Submit();

    Expect(
        stream.SealFrame(replaced_frame) &&
            stream.SealFrame(incoming_frame),
        "CommandList move frames could not be sealed"
    );
    ResolveTimestampAt(
        incoming_submit, 0, Timestamp(100, 110)
    );

    ResolvedGpuScopeFrame resolved{};
    Expect(
        stream.TryPopFrame(resolved) &&
            resolved.frame_id == 7001 &&
            resolved.error_scope_count == 1 &&
            resolved.queue_roots[0].front().error_reason.find(
                "replaced before submission"
            ) != std::string::npos,
        "replaced CommandList scope was not terminally rejected"
    );
    Expect(
        stream.TryPopFrame(resolved) &&
            resolved.frame_id == 7002 && resolved.valid &&
            resolved.queue_roots[0].front().name ==
                "MovedOpenScope",
        "moved open CommandList scope did not retain its topology"
    );
    ReleaseTerminalSubmit(incoming_submit);

    GpuScopeFrameHandle constructed_frame = stream.BeginFrame(7003);
    CommandList constructed_source(EQueueType::Compute);
    constructed_source.SetGpuScopeRecorder(
        constructed_frame.CreateRecorder(
            QueueBinding(EQueueType::Compute, 8, 3), 1
        )
    );
    constructed_source.PushScopeWithTimeScope("MoveConstructed");
    CommandList constructed(std::move(constructed_source));
    constructed.PopScopeWithTimeScope();
    CmdSubmit constructed_submit = constructed.Submit();
    Expect(
        stream.SealFrame(constructed_frame),
        "move-constructed frame seal failed"
    );
    ResolveTimestampAt(
        constructed_submit, 0, Timestamp(0, 5)
    );
    Expect(
        stream.TryPopFrame(resolved) &&
            resolved.valid &&
            resolved.queue_roots[1].front().name ==
                "MoveConstructed",
        "CommandList move construction lost its scope"
    );
    ReleaseTerminalSubmit(constructed_submit);
}

void CmdSubmitMoveAndReplacementAreTerminalAndOneShot() {
    GpuScopeStream stream(SmallConfig());
    GpuScopeFrameHandle moved_frame = stream.BeginFrame(7101);
    CommandList moved_list(EQueueType::Graphics);
    moved_list.SetGpuScopeRecorder(
        moved_frame.CreateRecorder(
            QueueBinding(EQueueType::Graphics, 0, 0), 0
        )
    );
    RecordTimedScope(moved_list, "MovedSubmitScope");
    CmdSubmit source_submit = moved_list.Submit();
    CmdSubmit moved_submit(std::move(source_submit));
    Expect(
        source_submit.query_tokens.empty() &&
            moved_submit.query_tokens.size() == 1,
        "CmdSubmit move construction duplicated query ownership"
    );
    Expect(stream.SealFrame(moved_frame), "moved submit frame seal failed");
    ResolveTimestampAt(moved_submit, 0, Timestamp(1, 3));

    ResolvedGpuScopeFrame resolved{};
    Expect(
        stream.TryPopFrame(resolved) && resolved.valid &&
            resolved.queue_roots[0].front().name ==
                "MovedSubmitScope",
        "moved CmdSubmit did not retain its completion callback"
    );
    ReleaseTerminalSubmit(moved_submit);

    GpuScopeFrameHandle replaced_frame = stream.BeginFrame(7102);
    CommandList replaced_list(EQueueType::Graphics);
    replaced_list.SetGpuScopeRecorder(
        replaced_frame.CreateRecorder(
            QueueBinding(EQueueType::Graphics, 0, 0), 0
        )
    );
    RecordTimedScope(replaced_list, "ReplacedSubmitScope");
    CmdSubmit replaced_submit = replaced_list.Submit();
    QueryToken replaced_token =
        replaced_submit.query_tokens.front();

    GpuScopeFrameHandle incoming_frame = stream.BeginFrame(7103);
    CommandList incoming_list(EQueueType::Graphics);
    incoming_list.SetGpuScopeRecorder(
        incoming_frame.CreateRecorder(
            QueueBinding(EQueueType::Graphics, 0, 0), 0
        )
    );
    RecordTimedScope(incoming_list, "IncomingSubmitScope");
    CmdSubmit incoming_submit = incoming_list.Submit();

    CmdSubmit destination = CommandList(EQueueType::Graphics).Submit();
    destination = std::move(replaced_submit);
    destination = std::move(incoming_submit);
    Expect(
        stream.SealFrame(replaced_frame) &&
            stream.SealFrame(incoming_frame),
        "submit replacement frames could not be sealed"
    );
    Expect(
        !QueryBackendAccess::ResolveTimestamp(
            replaced_token, Timestamp(0, 1)
        ),
        "replaced CmdSubmit query accepted a late Ready"
    );
    ResolveTimestampAt(destination, 0, Timestamp(2, 7));

    Expect(
        stream.TryPopFrame(resolved) &&
            resolved.frame_id == 7102 &&
            resolved.error_scope_count == 1 &&
            resolved.queue_roots[0].front().error_reason.find(
                "replaced before completion"
            ) != std::string::npos,
        "replaced CmdSubmit did not reject exactly once"
    );
    Expect(
        stream.TryPopFrame(resolved) &&
            resolved.frame_id == 7103 && resolved.valid,
        "incoming CmdSubmit lost ownership during replacement"
    );
    Expect(
        stream.GetStats().scopes_error == 1,
        "CmdSubmit replacement counted more than one Error"
    );
    ReleaseTerminalSubmit(destination);
}

void StreamCloseAndDestructionDoNotInvalidateOutstandingCallbacks() {
    {
        GpuScopeStream stream(SmallConfig());
        GpuScopeFrameHandle frame = stream.BeginFrame(8001);
        CommandList list(EQueueType::Graphics);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        RecordTimedScope(list, "CloseLifetime");
        CmdSubmit submit = list.Submit();
        Expect(stream.SealFrame(frame), "close-lifetime frame seal failed");

        stream.Close();
        const GpuScopeStreamStats closed = stream.GetStats();
        Expect(
            !closed.accepting &&
                closed.frames_abandoned_on_close == 1 &&
                closed.resident_frames == 0 &&
                closed.resident_scopes == 0 &&
                !stream.BeginFrame(8002).Valid(),
            "Close did not detach and reject admission"
        );
        stream.Close();
        ResolveTimestampAt(submit, 0, Timestamp(10, 12));
        ResolvedGpuScopeFrame resolved{};
        Expect(
            !stream.TryPopFrame(resolved),
            "closed stream resurrected an abandoned frame"
        );
        ReleaseTerminalSubmit(submit);
    }

    std::optional<CmdSubmit> outliving_submit{};
    {
        GpuScopeStream stream(SmallConfig());
        GpuScopeFrameHandle frame = stream.BeginFrame(8003);
        CommandList list(EQueueType::Compute);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Compute, 4, 4), 0
            )
        );
        RecordTimedScope(list, "DestroyedStreamLifetime");
        outliving_submit.emplace(list.Submit());
        Expect(
            stream.SealFrame(frame),
            "destroyed-stream lifetime frame seal failed"
        );
    }
    ResolveTimestampAt(
        *outliving_submit, 0, Timestamp(20, 25)
    );
    ReleaseTerminalSubmit(*outliving_submit);
    outliving_submit.reset();

    GpuScopeStream move_source(SmallConfig());
    GpuScopeFrameHandle moved_frame = move_source.BeginFrame(8004);
    GpuScopeStream move_destination(std::move(move_source));
    Expect(
        !move_source.Valid() && move_destination.Valid() &&
            move_destination.SealFrame(moved_frame),
        "GpuScopeStream move lost its resident frame"
    );
    ResolvedGpuScopeFrame resolved{};
    Expect(
        move_destination.TryPopFrame(resolved) &&
            resolved.frame_id == 8004,
        "moved GpuScopeStream could not pop its frame"
    );
}

void CloseRacesCompletionWithoutResurrectingFrames() {
    constexpr int race_iterations = 128;
    for (int iteration = 0; iteration < race_iterations; ++iteration) {
        GpuScopeStream stream(SmallConfig());
        GpuScopeFrameHandle frame =
            stream.BeginFrame(9000 + iteration);
        CommandList list(EQueueType::Graphics);
        list.SetGpuScopeRecorder(
            frame.CreateRecorder(
                QueueBinding(EQueueType::Graphics, 0, 0), 0
            )
        );
        RecordTimedScope(list, "CloseRace");
        CmdSubmit submit = list.Submit();
        QueryToken token = submit.query_tokens.front();
        Expect(stream.SealFrame(frame), "close-race frame seal failed");

        std::atomic_bool start{false};
        bool             resolved = false;
        std::thread close_thread([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            stream.Close();
        });
        std::thread completion_thread([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            resolved = QueryBackendAccess::ResolveTimestamp(
                token, Timestamp(10, 11)
            );
        });
        start.store(true, std::memory_order_release);
        close_thread.join();
        completion_thread.join();

        Expect(
            resolved &&
                token.GetFuture().Get().status == QueryStatus::Ready,
            "Close race prevented the query terminal transition"
        );
        const GpuScopeStreamStats stats = stream.GetStats();
        Expect(
            !stats.accepting &&
                stats.resident_frames == 0 &&
                stats.resident_pending_frames == 0 &&
                stats.resident_ready_frames == 0 &&
                stats.resident_scopes == 0 &&
                stats.frames_abandoned_on_close <= 1,
            "Close race retained or resurrected bounded state"
        );
        ResolvedGpuScopeFrame resolved_frame{};
        Expect(
            !stream.TryPopFrame(resolved_frame),
            "Close race exposed an abandoned frame"
        );
        ReleaseTerminalSubmit(submit);
    }
}

} // namespace

int main() {
    static_assert(!std::is_copy_constructible_v<GpuScopeStream>);
    static_assert(std::is_nothrow_move_constructible_v<GpuScopeStream>);
    static_assert(std::is_nothrow_move_assignable_v<GpuScopeStream>);
    static_assert(!std::is_copy_constructible_v<GpuScopeRecorder>);
    static_assert(std::is_nothrow_move_constructible_v<GpuScopeRecorder>);
    static_assert(std::is_copy_constructible_v<GpuScopeCompletionTicket>);

    try {
        ReverseTerminalOrderBuildsNestedHierarchyAndExclusiveTime();
        FramesPopInBeginOrderAndSealIsOneShot();
        ManagedQueuesRemainIndependentTimestampDomains();
        ErrorCancelAndRejectAreExactlyOnce();
        MalformedTimestampPayloadAndTopologyFailClosed();
        ClosedModernStreamNeverFallsBackToLegacyProfiling();
        RecorderBindingIsImmutableUntilGenerationReset();
        SealWaitsForAdmissionsThatAlreadyStarted();
        FrameAdmissionCapacityIsBoundedAndRecovers();
        ConcurrentDuplicateFrameAdmissionReservesExactlyOneSlot();
        PerFrameAndGlobalScopeCapacityAreBoundedAndRecover();
        CapacityFailureSuppressesTheWholeNestedSubtree();
        DuplicateNamesAndSourceCapacityRemainUnambiguous();
        CommandListMoveAndReplacementPreserveTerminalOwnership();
        CmdSubmitMoveAndReplacementAreTerminalAndOneShot();
        StreamCloseAndDestructionDoNotInvalidateOutstandingCallbacks();
        CloseRacesCompletionWithoutResurrectingFrames();
    } catch (const std::exception& exception) {
        std::cerr << "RHIGpuScopeContract: "
                  << exception.what() << '\n';
        return 1;
    }

    std::cout << "RHIGpuScopeContract: all checks passed\n";
    return 0;
}
