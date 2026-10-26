#pragma once

#include "rhi/RHIExecutorBackend.h"
#include "log/LogSystem.h"

#include <cassert>
#include <memory>
#include <mutex>

// Backend-internal source splitting and completion ownership. Shared with the
// integration tests without exposing a test operation on the runtime executor.
namespace Moer::Render::VulkanSubmissionDetail {

inline bool SubmitHasSideEffects(const CmdSubmit& _submit) {
    return !_submit.callbacks.empty() || !_submit.success_callbacks.empty() ||
           !_submit.gpu_completion_tokens.empty() ||
           !_submit.query_tokens.empty() ||
           !_submit.wait_events.empty() || !_submit.signal_events.empty() || _submit.b_sync ||
           _submit.b_tick_profiling || _submit.b_delete_resources;
}

inline void InvokeSourceCallbacksNoexcept(
    Array<std::function<void()>>& _callbacks,
    std::string_view              _context
) noexcept {
    for (std::function<void()>& callback : _callbacks) {
        if (!callback) {
            continue;
        }
        try {
            callback();
        } catch (const std::exception& error) {
            try {
                LOG_ERROR("{} callback threw: {}", _context, error.what());
            } catch (...) {
            }
        } catch (...) {
            try {
                LOG_ERROR("{} callback threw", _context);
            } catch (...) {
            }
        }
    }
    _callbacks.clear();
}

class VulkanMultiSegmentCompletionState final {
public:
    explicit VulkanMultiSegmentCompletionState(Array<FenceRef>&& _segment_outcomes) :
        segment_outcomes(std::move(_segment_outcomes)),
        ordinary_retired(segment_outcomes.size(), uint8{0}),
        success_retired(segment_outcomes.size(), uint8{0}),
        ordinary_remaining(segment_outcomes.size()),
        success_remaining(segment_outcomes.size()) {}

    void AdoptSourceCallbacks(
        Array<std::function<void()>>&& _callbacks,
        Array<std::function<void()>>&& _success_callbacks
    ) noexcept {
        callbacks         = std::move(_callbacks);
        success_callbacks = std::move(_success_callbacks);
    }

    const FenceRef& SegmentOutcome(size_t _segment_index) const noexcept {
        return segment_outcomes[_segment_index];
    }

    void RetireOrdinary(size_t _segment_index) noexcept {
        Array<std::function<void()>> final_callbacks{};
        {
            std::lock_guard lock(mutex);
            if (_segment_index >= ordinary_retired.size() ||
                ordinary_retired[_segment_index] != 0) {
                return;
            }
            ordinary_retired[_segment_index] = 1;
            assert(ordinary_remaining != 0);
            --ordinary_remaining;
            if (ordinary_remaining != 0) {
                return;
            }
            final_callbacks = std::move(callbacks);
        }

        InvokeSourceCallbacksNoexcept(
            final_callbacks,
            "[RHIExecutor][Vulkan][MultiSegment] ordinary"
        );
    }

    void RetireSuccess(size_t _segment_index) noexcept {
        Array<std::function<void()>> final_success_callbacks{};
        {
            std::lock_guard lock(mutex);
            if (_segment_index >= success_retired.size() ||
                success_retired[_segment_index] != 0) {
                return;
            }
            // Every packet runs its ordinary tier before its success tier.
            // Treat a violation as a backend ownership bug instead of
            // allowing the aggregate success tier to overtake ordinary work.
            if (ordinary_retired[_segment_index] == 0) {
                std::terminate();
            }
            success_retired[_segment_index] = 1;
            assert(success_remaining != 0);
            --success_remaining;
            if (success_remaining != 0) {
                return;
            }
            final_success_callbacks = std::move(success_callbacks);
        }

        InvokeSourceCallbacksNoexcept(
            final_success_callbacks,
            "[RHIExecutor][Vulkan][MultiSegment] success"
        );
    }

private:
    Array<FenceRef>              segment_outcomes{};
    Array<uint8>                 ordinary_retired{};
    Array<uint8>                 success_retired{};
    size_t                       ordinary_remaining{0};
    size_t                       success_remaining{0};
    std::mutex                   mutex{};
    Array<std::function<void()>> callbacks{};
    Array<std::function<void()>> success_callbacks{};
};

struct VulkanExecutableSourceMetadata {
    uint32 original_source_index{0};
    uint32 source_segment_index{0};
    uint32 source_segment_count{1};
    bool   cross_native_predecessor_wait{false};
};

struct VulkanSegmentMaterializationResult {
    bool                                  changed{false};
    size_t                                original_source_count{0};
    size_t                                cross_native_wait_count{0};
    Array<VulkanExecutableSourceMetadata> sources{};
};

inline CmdSubmit MakeEmptySubmit() {
    return CmdSubmit(
        Array<UniquePtr<Command>>{},
        Array<std::function<void()>>{},
        Array<std::function<void()>>{},
        TCachedArgArray{}
    );
}

VulkanSegmentMaterializationResult
MaterializeMultiSegmentBatch(RHIBackendSubmissionBatch& _batch, const RHIQueueTopology& _queue_topology);

} // namespace Moer::Render::VulkanSubmissionDetail
