#ifndef MOER_ENGINE_RHI_PRESENTATION_COMPLETION_H
#define MOER_ENGINE_RHI_PRESENTATION_COMPLETION_H

#include "RenderAPI.h"
#include "misc/STL.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace Moer::Render {

// GPU queue completion and presentation completion are separate. Without a
// present fence, wait for the next acquisition's GPU dependency, or use queue
// idle when draining the swapchain.
enum class EPresentationWsiCompletionMode : std::uint8_t {
    Immediate = 0,
    PresentFence,
    QueueIdleFallback,
    Failed,
};

enum class EPresentationWsiCompletionOutcome : std::uint8_t {
    Pending = 0,
    Succeeded,
    Failed,
};

class PresentationCompletionState;
using PresentationCompletionStateRef = SharedPtr<PresentationCompletionState>;

// The actual CPU completion record, shared by the swapchain and Completion
// owner. Native fences and GPU resources stay in the backend's Present batch.
class PresentationCompletion {
public:
    [[nodiscard]] std::uint64_t GetIssueSequence() const noexcept {
        return issue_sequence_;
    }
    [[nodiscard]] EPresentationWsiCompletionOutcome GetWsiOutcome() const noexcept {
        return wsi_outcome_.load(std::memory_order_acquire);
    }

private:
    std::weak_ptr<PresentationCompletionState>     state_{};
    std::uint64_t                                  presentation_epoch_{0};
    std::uint64_t                                  drawable_generation_{0};
    std::uint64_t                                  issue_sequence_{0};
    std::atomic<EPresentationWsiCompletionOutcome> wsi_outcome_{EPresentationWsiCompletionOutcome::Pending};
    std::optional<EPresentationWsiCompletionMode>  wsi_mode_{};
    bool                                           gpu_complete_{false};
    bool                                           retired_{false};

    [[nodiscard]] bool IsTerminal() const noexcept {
        return gpu_complete_ && GetWsiOutcome() != EPresentationWsiCompletionOutcome::Pending;
    }
    friend class PresentationCompletionState;
};
using PresentationCompletionRef = SharedPtr<PresentationCompletion>;

// A snapshot of the matching pending records. Later Presents cannot extend it.
class PresentationDrainPoint {
public:
    [[nodiscard]] bool IsValid() const noexcept {
        return bool(state_);
    }
    [[nodiscard]] const PresentationCompletionStateRef& GetState() const noexcept {
        return state_;
    }

private:
    PresentationCompletionStateRef   state_{};
    Array<PresentationCompletionRef> records_{};
    friend class PresentationCompletionState;
};

class RENDER_API PresentationCompletionState final
    : public std::enable_shared_from_this<PresentationCompletionState> {
public:
    [[nodiscard]] static PresentationCompletionStateRef Create();
    ~PresentationCompletionState();

    PresentationCompletionState(const PresentationCompletionState&)            = delete;
    PresentationCompletionState& operator=(const PresentationCompletionState&) = delete;

    [[nodiscard]] PresentationCompletionRef
                       Reserve(std::uint64_t _presentation_epoch, std::uint64_t _drawable_generation);
    [[nodiscard]] bool Owns(const PresentationCompletionRef& _record) const noexcept;
    bool SetWsiMode(const PresentationCompletionRef& _record, EPresentationWsiCompletionMode _mode) noexcept;
    bool MarkGpuComplete(const PresentationCompletionRef& _record) noexcept;
    bool MarkWsiComplete(const PresentationCompletionRef& _record) noexcept;
    // Call only after the next acquisition's semaphore/fence wait completes.
    bool MarkImageAvailable(const PresentationCompletionRef& _previous_present) noexcept;
    bool MarkWsiFailed(const PresentationCompletionRef& _record) noexcept;

    // Release native objects and resource references before publishing retirement
    // to the drain waiter.
    bool                      PublishRetired(const PresentationCompletionRef& _record) noexcept;
    [[nodiscard]] std::size_t PublishRetired() noexcept;

    void WaitForWsi(const PresentationCompletionRef& _record) const;
    [[nodiscard]] bool
    WaitForWsi(const PresentationCompletionRef& _record, std::chrono::nanoseconds _timeout) const;

    [[nodiscard]] PresentationDrainPoint
                       Freeze(std::uint64_t _presentation_epoch = 0, std::uint64_t _drawable_generation = 0);
    [[nodiscard]] bool RequiresQueueIdle(const PresentationDrainPoint& _point) const noexcept;
    bool               ResolveQueueIdle(const PresentationDrainPoint& _point) noexcept;
    void               WaitRetired(const PresentationDrainPoint& _point) const;
    [[nodiscard]] bool
    WaitRetired(const PresentationDrainPoint& _point, std::chrono::nanoseconds _timeout) const;

    [[nodiscard]] bool        HasOutstanding() const noexcept;
    [[nodiscard]] std::size_t OutstandingCount() const noexcept;

private:
    struct Impl;
    PresentationCompletionState();
    [[nodiscard]] bool    IsPending(const PresentationCompletionRef& _record) const noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace Moer::Render

#endif
