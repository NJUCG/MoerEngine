#pragma once

#include "misc/Types.h"
#include "rendergraph/RenderGraph.h"
#include "taskgraph/TaskGraph.h"

#include <algorithm>
#include <span>
#include <vector>

namespace Moer::Render::Detail {

struct RecordingRange {
    uint32 first_unit = 0;
    uint32 unit_count = 0;
};

struct RecordingDispatch {
    uint32   first_unit = 0;
    uint32   unit_count = 0;
    bool     parallel = false;
};

// Compiler groups describe CPU ordering; these ranges describe the actual
// producers for this execution. Disabling concurrency allows neighboring
// groups to share storage without changing their callback order.
inline std::vector<RecordingDispatch> BuildRecordingDispatches(
    const RenderGraph::CompiledPlan& plan, bool parallel_enabled
) {
    const bool can_parallel = parallel_enabled && TaskGraph::IsInitialized() &&
        TaskGraph::GetInterface().GetWorkerThreadCount() > 1;
    std::vector<RecordingDispatch> dispatches;
    dispatches.reserve(plan.frontend_dispatch_groups.size());
    for (const auto& group : plan.frontend_dispatch_groups) {
        const bool parallel = can_parallel &&
            group.record_policy == RenderGraph::PassRecordPolicy::ParallelAllowed;
        if (!parallel && !dispatches.empty() && !dispatches.back().parallel) {
            dispatches.back().unit_count += group.unit_count;
        } else {
            dispatches.push_back({group.first_unit, group.unit_count, parallel});
        }
    }
    return dispatches;
}

// Work units estimate CPU recording, not GPU execution. A normal draw/dispatch
// pass defaults to one; callbacks with loops can declare their operation count.
// Eight units is an initial granularity policy, not a correctness constraint.
inline std::vector<RecordingRange> PartitionRecordingRange(
    std::span<const RenderGraph::CompiledFrontendRecordUnit> units,
    RecordingDispatch                                        dispatch,
    uint32                                                   worker_count,
    std::span<const uint8>                                   split_before = {}
) {
    std::vector<RecordingRange> ranges;
    if (dispatch.unit_count == 0) { return ranges; }
    const uint32 end        = dispatch.first_unit + dispatch.unit_count;
    uint64       total_work = 0;
    for (uint32 index = dispatch.first_unit; index < end; ++index) {
        total_work += units[index].recording_workload;
    }
    const uint64 workers     = std::max(1u, worker_count);
    const uint64 target_work = std::max(uint64{8}, (total_work + workers - 1) / workers);

    uint32 first = dispatch.first_unit;
    uint64 work  = 0;
    for (uint32 index = first; index < end; ++index) {
        if (index != first) {
            const auto& previous = units[index - 1];
            const auto& current = units[index];
            const bool boundary = previous.target_queue != current.target_queue ||
                previous.native_translate_class != ERHITranslateExecutionClass::Parallel ||
                current.native_translate_class != ERHITranslateExecutionClass::Parallel ||
                (!split_before.empty() && split_before[index] != 0);
            if (boundary || (dispatch.parallel &&
                (work >= target_work || work + current.recording_workload > target_work))) {
                ranges.push_back({first, index - first});
                first = index;
                work = 0;
            }
        }
        work += units[index].recording_workload;
    }
    ranges.push_back({first, end - first});
    return ranges;
}

} // namespace Moer::Render::Detail
