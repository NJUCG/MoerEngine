#ifndef MOER_ENGINE_RHI_PRESENTATION_COMPLETION_H
#define MOER_ENGINE_RHI_PRESENTATION_COMPLETION_H

#include "RenderAPI.h"
#include "misc/STL.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace Moer::Render {

struct PresentationCompletionStateAccess;

// WSI completion is deliberately separate from GPU queue completion. A
// backend selects exactly one mode after attempting Present:
// - Immediate/Failed have no outstanding native WSI wait.
// - PresentFence is completed by the Completion owner.
// - QueueIdleFallback is resolved only after the Submission owner performs
//   the backend's queue-idle fallback.
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

struct PresentationCompletionIdentity {
    std::uint64_t state_instance_id{0};
    std::uint64_t presentation_epoch{0};
    std::uint64_t drawable_generation{0};
    std::uint64_t request_serial{0};

    [[nodiscard]] friend constexpr bool operator==(
        const PresentationCompletionIdentity&,
        const PresentationCompletionIdentity&
    ) noexcept = default;
};

class PresentationCompletionState;
using PresentationCompletionStateRef =
    SharedPtr<PresentationCompletionState>;

// Strong producer/completion identity. issue_sequence is assigned by the
// reducer and therefore does not inherit ordering, reuse, or wrap assumptions
// from a caller-owned Present receipt serial.
class RENDER_API PresentationCompletionTicket {
public:
    PresentationCompletionTicket() = default;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] const PresentationCompletionIdentity&
    GetIdentity() const noexcept;
    [[nodiscard]] std::uint64_t GetIssueSequence() const noexcept;
    [[nodiscard]] std::uint32_t GetFenceSlot() const noexcept;
    [[nodiscard]] std::uint64_t GetSlotGeneration() const noexcept;
    [[nodiscard]] EPresentationWsiCompletionOutcome
    GetWsiOutcome() const noexcept;

private:
    PresentationCompletionTicket(
        PresentationCompletionStateRef _state,
        PresentationCompletionIdentity _identity,
        std::uint64_t                  _issue_sequence,
        std::uint32_t                  _fence_slot,
        std::uint64_t                  _slot_generation,
        SharedPtr<std::atomic<EPresentationWsiCompletionOutcome>>
            _wsi_outcome
    ) noexcept;

    PresentationCompletionStateRef state_{};
    PresentationCompletionIdentity identity_{};
    std::uint64_t                   issue_sequence_{0};
    std::uint32_t                   fence_slot_{0};
    std::uint64_t                   slot_generation_{0};
    SharedPtr<std::atomic<EPresentationWsiCompletionOutcome>>
        wsi_outcome_{};

    friend class PresentationCompletionState;
    friend struct PresentationCompletionStateAccess;
};

// A strong, immutable snapshot of the records that a targeted drain may wait
// for. Records issued after this point never extend the wait. epoch/generation
// zero/zero selects every identity owned by this state.
struct RENDER_API PresentationDrainPoint {
public:
    PresentationDrainPoint() = default;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] std::uint64_t GetStateInstanceId() const noexcept;
    [[nodiscard]] std::uint64_t GetThroughIssueSequence() const noexcept;
    [[nodiscard]] std::uint64_t GetPresentationEpoch() const noexcept;
    [[nodiscard]] std::uint64_t GetDrawableGeneration() const noexcept;
    [[nodiscard]] bool IncludesAllIdentities() const noexcept;
    [[nodiscard]] const PresentationCompletionStateRef&
    GetState() const noexcept;

private:
    PresentationCompletionStateRef state_{};
    std::uint64_t                   state_instance_id_{0};
    std::uint64_t                   through_issue_sequence_{0};
    std::uint64_t                   presentation_epoch_{0};
    std::uint64_t                   drawable_generation_{0};
    bool                            all_identities_{false};

    friend class PresentationCompletionState;
    friend struct PresentationCompletionStateAccess;
};

// Pure CPU reducer for one strongly-owned PresentationSurface lifetime.
// Native objects and raw-pointer keys never enter this type. A record is
// reclaimable only after both its GPU and WSI components are complete, and is
// erased only when its retirement is explicitly published.
class RENDER_API PresentationCompletionState final :
    public std::enable_shared_from_this<PresentationCompletionState> {
public:
    [[nodiscard]] static PresentationCompletionStateRef Create();
    ~PresentationCompletionState();

    PresentationCompletionState(const PresentationCompletionState&) = delete;
    PresentationCompletionState& operator=(
        const PresentationCompletionState&
    ) = delete;

    [[nodiscard]] std::uint64_t GetInstanceId() const noexcept;
    [[nodiscard]] std::uint64_t GetStateInstanceId() const noexcept {
        return GetInstanceId();
    }

    [[nodiscard]] PresentationCompletionTicket Reserve(
        PresentationCompletionIdentity _identity,
        std::uint32_t                   _fence_slot,
        std::uint64_t                   _slot_generation
    );

    // Every mutation performs an exact ticket lookup. A late callback from a
    // retired state/epoch/generation/slot generation returns false and cannot
    // mutate a newer record.
    bool SetWsiMode(
        const PresentationCompletionTicket& _ticket,
        EPresentationWsiCompletionMode      _mode
    ) noexcept;
    bool MarkGpuComplete(
        const PresentationCompletionTicket& _ticket
    ) noexcept;
    bool MarkWsiComplete(
        const PresentationCompletionTicket& _ticket
    ) noexcept;
    bool MarkWsiFailed(
        const PresentationCompletionTicket& _ticket
    ) noexcept;

    // Publish one terminal record, or all currently terminal records. Unlike a
    // prefix watermark, publication can retire a fast failure behind an older
    // accepted Present without falsely retiring that older operation.
    bool PublishRetired(
        const PresentationCompletionTicket& _ticket
    ) noexcept;
    [[nodiscard]] std::size_t PublishRetired() noexcept;

    void WaitForWsi(
        const PresentationCompletionTicket& _ticket
    ) const;
    [[nodiscard]] bool WaitForWsi(
        const PresentationCompletionTicket& _ticket,
        std::chrono::nanoseconds             _timeout
    ) const;

    template<typename Rep, typename Period>
    [[nodiscard]] bool WaitForWsi(
        const PresentationCompletionTicket&       _ticket,
        std::chrono::duration<Rep, Period>         _timeout
    ) const {
        return WaitForWsi(
            _ticket,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                _timeout
            )
        );
    }

    [[nodiscard]] PresentationDrainPoint Freeze(
        std::uint64_t _presentation_epoch = 0,
        std::uint64_t _drawable_generation = 0
    );
    [[nodiscard]] bool RequiresQueueIdle(
        const PresentationDrainPoint& _point
    ) const noexcept;
    bool ResolveQueueIdle(
        const PresentationDrainPoint& _point
    ) noexcept;

    void WaitRetired(const PresentationDrainPoint& _point) const;
    [[nodiscard]] bool WaitRetired(
        const PresentationDrainPoint& _point,
        std::chrono::nanoseconds       _timeout
    ) const;

    template<typename Rep, typename Period>
    [[nodiscard]] bool WaitRetired(
        const PresentationDrainPoint&          _point,
        std::chrono::duration<Rep, Period>      _timeout
    ) const {
        return WaitRetired(
            _point,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                _timeout
            )
        );
    }

    [[nodiscard]] bool HasOutstanding() const noexcept;
    [[nodiscard]] std::size_t OutstandingCount() const noexcept;

private:
    struct Impl;

    explicit PresentationCompletionState(
        std::uint64_t _state_instance_id
    );

    const std::uint64_t state_instance_id_{0};
    SharedPtr<Impl>     impl_{};

    friend struct PresentationCompletionStateAccess;
};

} // namespace Moer::Render

#endif
