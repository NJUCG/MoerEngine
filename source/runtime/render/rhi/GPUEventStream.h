#ifndef MOER_ENGINE_GPU_EVENT_STREAM_H
#define MOER_ENGINE_GPU_EVENT_STREAM_H

#include "RHICommand.h"
#include "RHICommon.h"
#include "misc/STL.h"
#include <mutex>

namespace Moer::Render {

struct ResolvedGPUEvent {
    std::string name;
    EQueueType  queue;
    uint32      depth;
    uint64      timestamp_ns;
    uint64      duration_ns;
    bool        is_frame_bound;
};

class GPUEventStream {
public:
    static GPUEventStream& Get();

    void RegisterSubmit(Array<GPUEvent>&& events, EQueueType queue, WaitEvent completion);
    void ResolveCompleted(uint64 timeline_value);
    void FlushToProfiler();

private:
    struct PendingSubmit {
        Array<GPUEvent> events;
        EQueueType      queue;
        WaitEvent       completion;
    };

    Queue<PendingSubmit>    pending_submits;
    Array<ResolvedGPUEvent> resolved_events;
    std::mutex              stream_mutex;
};

}  // namespace Moer::Render

#endif  // MOER_ENGINE_GPU_EVENT_STREAM_H
