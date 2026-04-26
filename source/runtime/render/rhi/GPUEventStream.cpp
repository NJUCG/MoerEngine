#include "GPUEventStream.h"
#include "log/LogSystem.h"
#include "misc/Assert.h"
#include "profile/ProfileDump.h"
#include "profile/ProfileDumpTemplates.h"
#include "trace/Trace.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <sstream>

namespace Moer::Render {

namespace {

constexpr size_t kQueueCount = static_cast<size_t>(EQueueType::Num);
constexpr size_t kInvalidIndex = static_cast<size_t>(-1);

uint64 SteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

template<GPUEvent::EType... Types>
bool IsEventType(GPUEvent::EType type) {
    return ((type == Types) || ...);
}

const char* QueueTypeName(EQueueType queue) {
    switch (queue) {
        case EQueueType::Graphics: return "Graphics";
        case EQueueType::Compute:  return "Compute";
        case EQueueType::Copy:     return "Copy";
        case EQueueType::Ignore:   return "Ignore";
        case EQueueType::Num:
        default:                   return "Num";
    }
}

uint32 QueueTrackIndex(EQueueType queue) {
    switch (queue) {
        case EQueueType::Graphics: return 0u;
        case EQueueType::Compute:  return 1u;
        case EQueueType::Copy:     return 2u;
        case EQueueType::Ignore:
        case EQueueType::Num:
        default:                   return 3u;
    }
}

bool IsBeginEvent(GPUEvent::EType type) {
    return IsEventType<GPUEvent::EType::BeginGPU, GPUEvent::EType::BeginEvent>(type);
}

bool IsEndEvent(GPUEvent::EType type) {
    return IsEventType<GPUEvent::EType::EndGPU, GPUEvent::EType::EndEvent>(type);
}

bool IsMatchingEndEvent(GPUEvent::EType begin_type, GPUEvent::EType end_type) {
    return (begin_type == GPUEvent::EType::BeginGPU && end_type == GPUEvent::EType::EndGPU) ||
           (begin_type == GPUEvent::EType::BeginEvent && end_type == GPUEvent::EType::EndEvent);
}

struct BuildNode {
    GPUEventNode      node{};
    GPUEvent::EType   begin_type{GPUEvent::EType::BeginGPU};
    Array<size_t>     child_indices{};
};

ResolvedGPUFrame MakeInvalidFrame(uint64 frame_index, uint64 boundary_timestamp_ns) {
    ResolvedGPUFrame frame{};
    frame.frame_index = frame_index;
    frame.boundary_timestamp_ns = boundary_timestamp_ns;
    frame.valid = false;
    return frame;
}

template<typename Visitor>
void VisitNodes(const Array<GPUEventNode>& nodes, Visitor&& visitor) {
    for (const GPUEventNode& node : nodes) {
        visitor(node);
        VisitNodes(node.children, visitor);
    }
}

template<typename Fn>
void ForEachQueueRoot(
    const std::array<Array<GPUEventNode>, kQueueCount>& queue_roots,
    Fn&&                                                fn
) {
    for (size_t queue_index = 0; queue_index < queue_roots.size(); ++queue_index) {
        if (queue_roots[queue_index].empty()) {
            continue;
        }
        fn(static_cast<EQueueType>(queue_index), queue_roots[queue_index]);
    }
}

GPUEventNode MaterializeNode(const Array<BuildNode>& arena, size_t node_index) {
    GPUEventNode node = arena[node_index].node;
    node.children.reserve(arena[node_index].child_indices.size());
    for (size_t child_index : arena[node_index].child_indices) {
        node.children.emplace_back(MaterializeNode(arena, child_index));
    }

    node.total_busy_ns = node.end_ns >= node.start_ns ? node.end_ns - node.start_ns : 0;
    uint64 child_total_busy_ns = 0;
    for (const GPUEventNode& child : node.children) {
        child_total_busy_ns += child.total_busy_ns;
    }
    node.exclusive_ns =
        node.total_busy_ns >= child_total_busy_ns ? node.total_busy_ns - child_total_busy_ns : 0;
    return node;
}

ResolvedGPUFrame BuildResolvedFrame(
    const auto& frame_events,
    uint64      frame_index,
    uint64      boundary_timestamp_ns
) {
    std::array<Array<BuildNode>, kQueueCount> build_arenas{};
    std::array<Array<size_t>, kQueueCount> open_stacks{};
    std::array<Array<size_t>, kQueueCount> root_indices{};
    bool frame_valid = true;

    for (const auto& event : frame_events) {
        if (event.queue == EQueueType::Ignore || event.queue == EQueueType::Num) {
            LOG_ERROR(MOER_TEXT("GPUEventStream got invalid queue type {} while building frame {}"), int(event.queue), frame_index);
            frame_valid = false;
            continue;
        }

        const size_t queue_index = static_cast<size_t>(event.queue);
        auto& arena = build_arenas[queue_index];
        auto& stack = open_stacks[queue_index];
        auto& roots = root_indices[queue_index];

        if (IsBeginEvent(event.type)) {
            const uint32 expected_depth = static_cast<uint32>(stack.size());
            if (event.depth != expected_depth) {
                LOG_ERROR(
                    MOER_TEXT("GPUEventStream begin depth mismatch in frame {} on queue {}: event depth={}, expected={}"),
                    frame_index,
                    QueueTypeName(event.queue),
                    event.depth,
                    expected_depth
                );
                frame_valid = false;
                continue;
            }

            BuildNode build_node{};
            build_node.begin_type = event.type;
            build_node.node.name = event.name;
            build_node.node.queue = event.queue;
            build_node.node.depth = event.depth;
            build_node.node.start_ns = event.timestamp_ns;
            build_node.node.end_ns = event.timestamp_ns;
            build_node.node.total_busy_ns = 0;
            build_node.node.exclusive_ns = 0;

            const size_t node_index = arena.size();
            arena.emplace_back(std::move(build_node));
            if (stack.empty()) {
                roots.emplace_back(node_index);
            } else {
                arena[stack.back()].child_indices.emplace_back(node_index);
            }
            stack.emplace_back(node_index);
            continue;
        }

        if (!IsEndEvent(event.type)) {
            LOG_ERROR(MOER_TEXT("GPUEventStream encountered non-scope event inside frame {}"), frame_index);
            frame_valid = false;
            continue;
        }

        if (stack.empty()) {
            LOG_ERROR(
                MOER_TEXT("GPUEventStream end event underflow in frame {} on queue {}"),
                frame_index,
                QueueTypeName(event.queue)
            );
            frame_valid = false;
            continue;
        }

        const size_t node_index = stack.back();
        BuildNode& node = arena[node_index];
        if (!IsMatchingEndEvent(node.begin_type, event.type)) {
            LOG_ERROR(
                MOER_TEXT("GPUEventStream mismatched begin/end type in frame {} on queue {}"),
                frame_index,
                QueueTypeName(event.queue)
            );
            frame_valid = false;
            continue;
        }
        if (event.depth != node.node.depth) {
            LOG_ERROR(
                MOER_TEXT("GPUEventStream end depth mismatch in frame {} on queue {}: event depth={}, begin depth={}"),
                frame_index,
                QueueTypeName(event.queue),
                event.depth,
                node.node.depth
            );
                frame_valid = false;
                continue;
        }
        node.node.end_ns = event.timestamp_ns;
        stack.pop_back();
    }

    for (size_t queue_index = 0; queue_index < open_stacks.size(); ++queue_index) {
        if (!open_stacks[queue_index].empty()) {
            LOG_ERROR(
                MOER_TEXT("GPUEventStream frame boundary crossed active scopes in frame {} on queue {}"),
                frame_index,
                QueueTypeName(static_cast<EQueueType>(queue_index))
            );
            frame_valid = false;
        }
    }

    ResolvedGPUFrame frame{};
    frame.frame_index = frame_index;
    frame.boundary_timestamp_ns = boundary_timestamp_ns;
    frame.valid = frame_valid;
    if (!frame_valid) {
        return frame;
    }
    for (size_t queue_index = 0; queue_index < build_arenas.size(); ++queue_index) {
        auto& roots = frame.queue_roots[queue_index];
        roots.reserve(root_indices[queue_index].size());
        for (size_t root_index : root_indices[queue_index]) {
            roots.emplace_back(MaterializeNode(build_arenas[queue_index], root_index));
        }
    }
    return frame;
}

template<typename TCompletedEvent>
ResolvedGPUFrame ResolveFrameFromPrefix(
    std::span<const TCompletedEvent> event_prefix,
    uint64                           frame_index,
    bool                             log_as_error
) {
    size_t boundary_index = kInvalidIndex;
    uint64 boundary_timestamp_ns = 0;
    bool   frame_valid = true;

    for (size_t event_index = 0; event_index < event_prefix.size(); ++event_index) {
        const auto& event = event_prefix[event_index];
        if (event.type == GPUEvent::EType::FrameBoundary) {
            if (boundary_index != kInvalidIndex) {
                if (log_as_error) {
                    LOG_ERROR(
                        "GPUEventStream found multiple frame boundaries in frame {} before EndFrame resolution",
                        frame_index
                    );
                } else {
                    LOG_WARNING(
                        "GPUEventStream found multiple frame boundaries in frame {} before EndFrame resolution",
                        frame_index
                    );
                }
                frame_valid = false;
                continue;
            }
            boundary_index = event_index;
            boundary_timestamp_ns = event.timestamp_ns;
            continue;
        }

        if (boundary_index != kInvalidIndex) {
            if (log_as_error) {
                LOG_ERROR(
                    "GPUEventStream found event '{}' after frame boundary in frame {}",
                    event.name,
                    frame_index
                );
            } else {
                LOG_WARNING(
                    "GPUEventStream found event '{}' after frame boundary in frame {}",
                    event.name,
                    frame_index
                );
            }
            frame_valid = false;
        }
    }

    if (boundary_index == kInvalidIndex) {
        if (log_as_error) {
            LOG_ERROR("GPUEventStream sealed frame {} without any FrameBoundary event", frame_index);
        } else {
            LOG_WARNING("GPUEventStream sealed frame {} without any FrameBoundary event", frame_index);
        }
        return MakeInvalidFrame(frame_index, 0);
    }

    if (!frame_valid) {
        return MakeInvalidFrame(frame_index, boundary_timestamp_ns);
    }

    Array<TCompletedEvent> frame_events{};
    frame_events.reserve(boundary_index);
    for (size_t event_index = 0; event_index < boundary_index; ++event_index) {
        frame_events.emplace_back(event_prefix[event_index]);
    }

    return BuildResolvedFrame(frame_events, frame_index, boundary_timestamp_ns);
}

void EmitFrameToTrace(const ResolvedGPUFrame& frame) {
    if (!frame.valid) {
        return;
    }
    ForEachQueueRoot(frame.queue_roots, [](EQueueType queue, const Array<GPUEventNode>& roots) {
        const uint64 track_id = Moer::Trace::MakeGpuQueueTrackId(0u, QueueTrackIndex(queue));
        std::string track_name = std::string("GPU0/Queue(") + QueueTypeName(queue) + ")";
        VisitNodes(roots, [&](const GPUEventNode& node) {
            Moer::Trace::EmitScope(Moer::Trace::EmitScopeDesc{
                .name        = node.name,
                .category    = "GPU",
                .track_type  = Moer::Trace::TrackType::GPUQueue,
                .track_id    = track_id,
                .depth       = node.depth,
                .ts_begin_ns = node.start_ns,
                .ts_end_ns   = node.end_ns,
                .track_name  = track_name,
            });
        });
    });
}

void EmitFrameToProfileDump(const ResolvedGPUFrame& frame) {
    if (!frame.valid) {
        return;
    }

    ForEachQueueRoot(frame.queue_roots, [&](EQueueType queue, const Array<GPUEventNode>& roots) {
        VisitNodes(roots, [&](const GPUEventNode& node) {
            DUMP_STREAM(Moer::ProfileDump::Templates::GpuScopeTemplate)
                << frame.frame_index
                << QueueTypeName(queue)
                << node.name
                << node.start_ns
                << node.end_ns
                << node.depth
                << node.total_busy_ns
                << node.exclusive_ns;
        });
    });
}

void AppendFrameText(std::ostringstream& stream, const GPUEventNode& node) {
    for (uint32 indent = 0; indent < node.depth + 1; ++indent) {
        stream << "  ";
    }
    stream << node.name
           << " [queue=" << QueueTypeName(node.queue)
           << ", start_ns=" << node.start_ns
           << ", end_ns=" << node.end_ns
           << ", total_busy_ns=" << node.total_busy_ns
           << ", exclusive_ns=" << node.exclusive_ns
           << "]\n";
    for (const GPUEventNode& child : node.children) {
        AppendFrameText(stream, child);
    }
}

std::string BuildFrameDebugText(const ResolvedGPUFrame& frame) {
    std::ostringstream stream{};
    stream << "Frame " << frame.frame_index
           << " boundary_ns=" << frame.boundary_timestamp_ns
           << " valid=" << (frame.valid ? "true" : "false") << "\n";
    ForEachQueueRoot(frame.queue_roots, [&](EQueueType queue, const Array<GPUEventNode>& roots) {
        stream << "Queue " << QueueTypeName(queue) << "\n";
        for (const GPUEventNode& node : roots) {
            AppendFrameText(stream, node);
        }
    });
    return stream.str();
}

void FlushGpuEventStreamCrashHook() {
    GPUEventStream::Get().FlushCrashSafeToProfiler();
}

} // namespace

GPUEventStream& GPUEventStream::Get() {
    static GPUEventStream instance;
    static const bool     registered_crash_hook = [] {
        Moer::Diagnostics::SetCrashFlushHook(&FlushGpuEventStreamCrashHook);
        return true;
    }();
    (void)registered_crash_hook;
    return instance;
}

void GPUEventStream::EnqueueSubmit(Array<GPUEvent>&& events, EQueueType queue, WaitEvent completion) {
    if (events.empty()) {
        return;
    }

    std::lock_guard lock(stream_mutex);
    testing_injected_submits = false;
    pending_submits.emplace_back(PendingSubmit{
        .enqueue_order = next_enqueue_order++,
        .events = std::move(events),
        .queue = queue,
        .completion = completion,
        .resolved = false,
    });
}

void GPUEventStream::ResolveCompleted(WaitEvent completion) {
    std::lock_guard lock(stream_mutex);

    for (PendingSubmit& submit : pending_submits) {
        if (submit.resolved) {
            continue;
        }
        if (submit.completion.timeline_handle != completion.timeline_handle ||
            submit.completion.value > completion.value) {
            continue;
        }

        for (GPUEvent& event : submit.events) {
            if (!event.query.IsReady()) {
                LOG_ERROR(
                    MOER_TEXT("GPUEventStream saw unresolved query future for event '{}' on queue {}"),
                    event.name,
                    int(submit.queue)
                );
                continue;
            }
            QueryResult query_result = event.query.GetFuture().Get();
            if (query_result.status != QueryStatus::Ready ||
                !std::holds_alternative<TimestampQueryResult>(query_result.payload)) {
                LOG_ERROR(
                    MOER_TEXT("GPUEventStream failed to resolve timestamp query for event '{}' on queue {}"),
                    event.name,
                    int(submit.queue)
                );
                submit.resolved = true;
                continue;
            }
            auto ts_result = std::get<TimestampQueryResult>(query_result.payload);
            event.timestamp_ns = ConvertTimestampToNsLocked(submit.queue, ts_result);
        }
        submit.resolved = true;
    }

    TryResolveReadyFramesLocked();
}

uint64 GPUEventStream::ConvertTimestampToNsLocked(EQueueType queue, const TimestampQueryResult& result) {
    const size_t queue_index = static_cast<size_t>(queue);
    if (queue_index >= timestamp_domains.size() || result.tick_period_ns <= 0.0) {
        return result.begin_tick;
    }

    TimestampDomain& domain = timestamp_domains[queue_index];
    if (!domain.valid || result.begin_tick < domain.anchor_tick || domain.tick_period_ns != result.tick_period_ns) {
        domain.valid = true;
        domain.anchor_tick = result.begin_tick;
        domain.anchor_time_ns = SteadyNowNs();
        domain.tick_period_ns = result.tick_period_ns;
    }

    const long double delta_ticks = static_cast<long double>(result.begin_tick - domain.anchor_tick);
    const long double delta_ns = delta_ticks * static_cast<long double>(domain.tick_period_ns);
    return domain.anchor_time_ns + static_cast<uint64>(std::max<long double>(0.0, delta_ns));
}

void GPUEventStream::EndFrame() {
    std::lock_guard lock(stream_mutex);

    if (!pending_frames.empty() &&
        pending_frames.back().sealed_after_enqueue_order == next_enqueue_order) {
        LOG_ERROR(MOER_TEXT("GPUEventStream received EndFrame twice without new enqueued submits"));
        return;
    }

    pending_frames.emplace_back(PendingFrame{
        .frame_index = next_frame_index++,
        .sealed_after_enqueue_order = next_enqueue_order,
    });
    TryResolveReadyFramesLocked();
}

void GPUEventStream::FlushToProfiler() {
    Array<ResolvedGPUFrame> frames_to_emit;
    {
        std::lock_guard lock(stream_mutex);
        frames_to_emit = ready_frames;
        ready_frames.clear();
    }

    for (const ResolvedGPUFrame& frame : frames_to_emit) {
        EmitFrameToTrace(frame);
        EmitFrameToProfileDump(frame);
    }
    if (!frames_to_emit.empty()) {
        Moer::ProfileDump::FlushThreadLocal();
    }
}

void GPUEventStream::FlushCrashSafeToProfiler() {
    Array<ResolvedGPUFrame> frames_to_emit;
    std::unique_lock lock(stream_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }

    frames_to_emit = ready_frames;
    ready_frames.clear();
    lock.unlock();

    for (const ResolvedGPUFrame& frame : frames_to_emit) {
        EmitFrameToProfileDump(frame);
    }
    if (!frames_to_emit.empty()) {
        Moer::ProfileDump::FlushThreadLocal();
    }
}

std::string GPUEventStream::FormatLastResolvedFrame() const {
    std::lock_guard lock(stream_mutex);
    if (!last_resolved_frame.has_value()) {
        return {};
    }
    return BuildFrameDebugText(last_resolved_frame.value());
}

void GPUEventStream::ResetForTesting() {
    std::lock_guard lock(stream_mutex);
    testing_injected_submits = true;
    pending_submits.clear();
    pending_frames.clear();
    resolved_events.clear();
    ready_frames.clear();
    last_resolved_frame.reset();
    next_enqueue_order = 0;
    next_frame_index = 0;
    resolved_enqueue_order_exclusive = 0;
    timestamp_domains = {};
}

void GPUEventStream::InjectResolvedSubmitForTesting(Array<GPUEvent>&& events, EQueueType queue) {
    std::lock_guard lock(stream_mutex);
    testing_injected_submits = true;

    const uint64 enqueue_order = next_enqueue_order++;
    for (GPUEvent& event : events) {
        resolved_events.emplace_back(CompletedGPUEvent{
            .type = event.type,
            .name = std::move(event.name),
            .queue = queue,
            .depth = event.depth,
            .enqueue_order = enqueue_order,
            .timestamp_ns = event.timestamp_ns,
        });
    }
    resolved_enqueue_order_exclusive = next_enqueue_order;
    TryResolveReadyFramesLocked();
}

void GPUEventStream::TryResolveReadyFramesLocked() {
    size_t ready_prefix_count = 0;
    while (ready_prefix_count < pending_submits.size() && pending_submits[ready_prefix_count].resolved) {
        PendingSubmit& submit = pending_submits[ready_prefix_count];
        for (const GPUEvent& event : submit.events) {
            resolved_events.emplace_back(CompletedGPUEvent{
                .type = event.type,
                .name = event.name,
                .queue = submit.queue,
                .depth = event.depth,
                .enqueue_order = submit.enqueue_order,
                .timestamp_ns = event.timestamp_ns,
            });
        }
        resolved_enqueue_order_exclusive = submit.enqueue_order + 1;
        ++ready_prefix_count;
    }

    if (ready_prefix_count > 0) {
        pending_submits.erase(
            pending_submits.begin(),
            pending_submits.begin() + static_cast<std::ptrdiff_t>(ready_prefix_count)
        );
    }

    while (!pending_frames.empty()) {
        const PendingFrame& pending_frame = pending_frames.front();
        if (resolved_enqueue_order_exclusive < pending_frame.sealed_after_enqueue_order) {
            break;
        }

        size_t resolved_prefix_count = 0;
        while (resolved_prefix_count < resolved_events.size() &&
               resolved_events[resolved_prefix_count].enqueue_order < pending_frame.sealed_after_enqueue_order) {
            ++resolved_prefix_count;
        }

        ResolvedGPUFrame frame = ResolveFrameFromPrefix(
            std::span<const CompletedGPUEvent>(resolved_events.data(), resolved_prefix_count),
            pending_frame.frame_index,
            !testing_injected_submits
        );
        last_resolved_frame = frame;
        ready_frames.emplace_back(std::move(frame));
        resolved_events.erase(
            resolved_events.begin(),
            resolved_events.begin() + static_cast<std::ptrdiff_t>(resolved_prefix_count)
        );
        pending_frames.erase(pending_frames.begin());
    }
}

}  // namespace Moer::Render
