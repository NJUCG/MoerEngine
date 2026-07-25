#pragma once

#include "rhi/RHICommon.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Moer::Render::RHISubmissionPipelinePolicy {

inline constexpr uint32_t DefaultBatchWindow = 2;
inline constexpr uint32_t MinBatchWindow     = 1;
inline constexpr uint32_t MaxBatchWindow     = 8;

[[nodiscard]] constexpr uint32_t ClampBatchWindow(
    uint32_t _configured
) noexcept {
    return _configured < MinBatchWindow ? MinBatchWindow :
           _configured > MaxBatchWindow ? MaxBatchWindow :
                                          _configured;
}

[[nodiscard]] constexpr bool GraphicsComputeShareNativeLane(
    const RHIQueueTopology& _topology
) noexcept {
    return _topology.graphics.available &&
           _topology.compute.available &&
           _topology.graphics.native_queue_id ==
               _topology.compute.native_queue_id;
}

[[nodiscard]] constexpr uint32_t ResolveEffectiveBatchWindow(
    uint32_t                _configured,
    const RHIQueueTopology& _topology
) noexcept {
    return GraphicsComputeShareNativeLane(_topology) ?
               MinBatchWindow :
               ClampBatchWindow(_configured);
}

// A single atomic word owns both the sealed bit and outstanding Submission
// work count. This prevents Seal() and the final FinishWork() from each
// observing stale state in different atomics and losing the terminal edge.
class PipelineBatchWorkState final {
public:
    // Precondition: the batch is not sealed and the outstanding count has
    // capacity. The executor registers each item before publishing it to the
    // Submission worker, so violating this contract is an internal bug.
    void AddWork() noexcept {
        const size_t previous =
            state.fetch_add(1, std::memory_order_relaxed);
        assert(
            (previous & SealedMask) == 0 &&
            "pipeline work cannot be added after the batch is sealed"
        );
        assert(
            (previous & WorkCountMask) != WorkCountMask &&
            "pipeline batch work count overflow"
        );
    }

    // Precondition: this call retires one item previously registered by
    // AddWork(). Returns true exactly for the operation which transitions a
    // sealed batch from one outstanding item to its terminal zero-work state.
    [[nodiscard]] bool FinishWork() noexcept {
        const size_t previous =
            state.fetch_sub(1, std::memory_order_acq_rel);
        const size_t previous_count = previous & WorkCountMask;
        assert(
            previous_count != 0 &&
            "pipeline batch work count underflow"
        );
        return previous_count == 1 &&
               (previous & SealedMask) != 0;
    }

    // Returns true exactly for the operation which seals an already-empty
    // batch. If work is still outstanding, the final FinishWork() owns the
    // terminal transition instead.
    [[nodiscard]] bool Seal() noexcept {
        const size_t previous =
            state.fetch_or(SealedMask, std::memory_order_acq_rel);
        return (previous & SealedMask) == 0 &&
               (previous & WorkCountMask) == 0;
    }

private:
    static constexpr size_t SealedMask =
        size_t{1} << (std::numeric_limits<size_t>::digits - 1);
    static constexpr size_t WorkCountMask = ~SealedMask;

    std::atomic<size_t> state{0};
};

} // namespace Moer::Render::RHISubmissionPipelinePolicy
