#ifndef MOER_ENGINE_GPU_EVENT_STREAM_H
#define MOER_ENGINE_GPU_EVENT_STREAM_H

#include "RHICommand.h"
#include "RHICommon.h"
#include "misc/STL.h"
#include <array>
#include <mutex>
#include <optional>

namespace Moer::Render {

struct GPUEventNode {
    std::string name;
    EQueueType  queue;
    uint32      depth;
    uint64      start_ns;
    uint64      end_ns;
    uint64      total_busy_ns;
    uint64      exclusive_ns;
    Array<GPUEventNode> children;
};

struct ResolvedGPUFrame {
    uint64 frame_index;
    uint64 boundary_timestamp_ns;
    bool   valid;
    std::array<Array<GPUEventNode>, static_cast<size_t>(EQueueType::Num)> queue_roots;
};

class RENDER_API GPUEventStream {
public:
    static GPUEventStream& Get();

    void EnqueueSubmit(Array<GPUEvent>&& events, EQueueType queue, WaitEvent completion);
    void ResolveCompleted(WaitEvent completion);
    void EndFrame();
    void FlushToProfiler();
    void FlushCrashSafeToProfiler();
    std::string FormatLastResolvedFrame() const;
    void InjectResolvedSubmitForTesting(Array<GPUEvent>&& events, EQueueType queue);
    void ResetForTesting();

private:
    struct PendingSubmit {
        uint64          enqueue_order;
        Array<GPUEvent> events;
        EQueueType      queue;
        WaitEvent       completion;
        bool            resolved;
    };

    struct PendingFrame {
        uint64 frame_index;
        uint64 sealed_after_enqueue_order;
    };

    struct CompletedGPUEvent {
        GPUEvent::EType type;
        std::string     name;
        EQueueType      queue;
        uint32          depth;
        uint64          enqueue_order;
        uint64          timestamp_ns;
    };

    struct TimestampDomain {
        bool   valid{false};
        uint64 anchor_tick{0};
        uint64 anchor_time_ns{0};
        double tick_period_ns{0.0};
    };

    uint64 ConvertTimestampToNsLocked(EQueueType queue, const TimestampQueryResult& result);
    void TryResolveReadyFramesLocked();

    Array<PendingSubmit>               pending_submits;
    Array<PendingFrame>                pending_frames;
    Array<CompletedGPUEvent>           resolved_events;
    Array<ResolvedGPUFrame>            ready_frames;
    std::optional<ResolvedGPUFrame>    last_resolved_frame;
    uint64                             next_enqueue_order{0};
    uint64                             next_frame_index{0};
    uint64                             resolved_enqueue_order_exclusive{0};
    std::array<TimestampDomain, static_cast<size_t>(EQueueType::Num)> timestamp_domains{};
    bool                               testing_injected_submits{false};
    mutable std::mutex                 stream_mutex;
};

}  // namespace Moer::Render

#endif  // MOER_ENGINE_GPU_EVENT_STREAM_H
