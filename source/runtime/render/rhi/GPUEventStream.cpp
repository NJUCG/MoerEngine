#include "GPUEventStream.h"
#include "trace/Trace.h"
#include <algorithm>

namespace Moer::Render {

GPUEventStream& GPUEventStream::Get() {
    static GPUEventStream instance;
    return instance;
}

void GPUEventStream::RegisterSubmit(Array<GPUEvent>&& events, EQueueType queue, WaitEvent completion) {
    std::lock_guard lock(stream_mutex);
    pending_submits.push(PendingSubmit{
        .events = std::move(events),
        .queue = queue,
        .completion = completion
    });
}

void GPUEventStream::ResolveCompleted(uint64 timeline_value) {
    std::lock_guard lock(stream_mutex);

    Array<PendingSubmit> completed;
    Queue<PendingSubmit> remaining;

    while (!pending_submits.empty()) {
        PendingSubmit& submit = pending_submits.front();
        if (submit.completion.value <= timeline_value) {
            completed.push_back(std::move(submit));
        } else {
            remaining.push(std::move(submit));
        }
        pending_submits.pop();
    }

    pending_submits = std::move(remaining);

    for (PendingSubmit& submit : completed) {
        Stack<GPUEvent*> begin_stack;

        for (GPUEvent& event : submit.events) {
            QueryResult query_result = event.query.GetFuture().Get();
            auto ts_result = std::get<TimestampQueryResult>(query_result.payload);
            uint64 timestamp_ns = ts_result.begin_tick;

            if (event.type == GPUEvent::EType::BeginCommandList || event.type == GPUEvent::EType::BeginEvent) {
                begin_stack.push(&event);
                event.cpu_time_ns = timestamp_ns;

            } else if (event.type == GPUEvent::EType::EndCommandList || event.type == GPUEvent::EType::EndEvent) {
                GPUEvent* begin_event = begin_stack.top();
                begin_stack.pop();

                uint64 begin_ns = begin_event->cpu_time_ns;
                uint64 end_ns = timestamp_ns;

                resolved_events.push_back(ResolvedGPUEvent{
                    .name = begin_event->name,
                    .queue = submit.queue,
                    .depth = begin_event->depth,
                    .timestamp_ns = begin_ns,
                    .duration_ns = end_ns - begin_ns,
                    .is_frame_bound = false
                });

            } else if (event.type == GPUEvent::EType::FrameBound) {
                resolved_events.push_back(ResolvedGPUEvent{
                    .name = "FrameBound",
                    .queue = submit.queue,
                    .depth = 0,
                    .timestamp_ns = timestamp_ns,
                    .duration_ns = 0,
                    .is_frame_bound = true
                });
            }
        }
    }
}

void GPUEventStream::FlushToProfiler() {
    std::lock_guard lock(stream_mutex);

    for (const ResolvedGPUEvent& evt : resolved_events) {
        uint32 queue_type_idx = (evt.queue == EQueueType::Graphics) ? 0u : 1u;
        uint64 track_id = Moer::Trace::MakeGpuQueueTrackId(0u, queue_type_idx);
        std::string track_name = evt.queue == EQueueType::Graphics ? "GPU0/Queue(Graphics)" : "GPU0/Queue(Compute)";

        if (evt.is_frame_bound) {
            Moer::Trace::EmitInstant("FrameBound", "GPU", {});
        } else {
            Moer::Trace::EmitScope(Moer::Trace::EmitScopeDesc{
                .name        = evt.name,
                .category    = "GPU",
                .track_type  = Moer::Trace::TrackType::GPUQueue,
                .track_id    = track_id,
                .depth       = evt.depth,
                .ts_begin_ns = evt.timestamp_ns,
                .ts_end_ns   = evt.timestamp_ns + evt.duration_ns,
                .track_name  = track_name,
            });
        }
    }

    resolved_events.clear();
}

}  // namespace Moer::Render

