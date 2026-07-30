#include "rhi/RHIPresentationCompletion.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace Moer::Render {
namespace {

std::atomic<std::uint64_t> g_presentation_completion_state_id{1};

std::uint64_t NextStateInstanceId() noexcept {
    std::uint64_t id = g_presentation_completion_state_id.fetch_add(
        1, std::memory_order_relaxed
    );
    while (id == 0) {
        id = g_presentation_completion_state_id.fetch_add(
            1, std::memory_order_relaxed
        );
    }
    return id;
}

} // namespace

struct PresentationCompletionState::Impl {
    struct Record {
        PresentationCompletionIdentity            identity{};
        std::uint64_t                              issue_sequence{0};
        std::uint32_t                              fence_slot{0};
        std::uint64_t                              slot_generation{0};
        SharedPtr<
            std::atomic<EPresentationWsiCompletionOutcome>>
            wsi_outcome{};
        std::optional<EPresentationWsiCompletionMode> wsi_mode{};
        bool                                       gpu_complete{false};
        bool                                       wsi_complete{false};

        [[nodiscard]] bool IsTerminal() const noexcept {
            return wsi_mode.has_value() &&
                   gpu_complete &&
                   wsi_complete;
        }
    };

    mutable std::mutex              mutex{};
    mutable std::condition_variable wsi_cv{};
    mutable std::condition_variable retired_cv{};
    std::map<std::uint64_t, Record> records{};
    std::uint64_t                   next_issue_sequence{1};
    std::uint64_t                   last_issue_sequence{0};
};

struct PresentationCompletionStateAccess {
    using Record = PresentationCompletionState::Impl::Record;
    using RecordIterator =
        std::map<std::uint64_t, Record>::iterator;
    using ConstRecordIterator =
        std::map<std::uint64_t, Record>::const_iterator;

    static bool TicketStructurallyBelongs(
        const PresentationCompletionState&  _state,
        const PresentationCompletionTicket& _ticket
    ) noexcept {
        return _ticket.state_.get() == &_state &&
               _ticket.issue_sequence_ != 0 &&
               _ticket.identity_.state_instance_id ==
                   _state.GetStateInstanceId();
    }

    static RecordIterator FindExactRecord(
        PresentationCompletionState::Impl&  _impl,
        const PresentationCompletionState&  _state,
        const PresentationCompletionTicket& _ticket
    ) noexcept {
        if (!TicketStructurallyBelongs(_state, _ticket)) {
            return _impl.records.end();
        }
        const auto record =
            _impl.records.find(_ticket.issue_sequence_);
        if (record == _impl.records.end()) {
            return record;
        }
        if (record->second.identity != _ticket.identity_ ||
            record->second.issue_sequence !=
                _ticket.issue_sequence_ ||
            record->second.fence_slot != _ticket.fence_slot_ ||
            record->second.slot_generation !=
                _ticket.slot_generation_) {
            return _impl.records.end();
        }
        return record;
    }

    static ConstRecordIterator FindExactRecord(
        const PresentationCompletionState::Impl& _impl,
        const PresentationCompletionState&       _state,
        const PresentationCompletionTicket&      _ticket
    ) noexcept {
        if (!TicketStructurallyBelongs(_state, _ticket)) {
            return _impl.records.end();
        }
        const auto record =
            _impl.records.find(_ticket.issue_sequence_);
        if (record == _impl.records.end()) {
            return record;
        }
        if (record->second.identity != _ticket.identity_ ||
            record->second.issue_sequence !=
                _ticket.issue_sequence_ ||
            record->second.fence_slot != _ticket.fence_slot_ ||
            record->second.slot_generation !=
                _ticket.slot_generation_) {
            return _impl.records.end();
        }
        return record;
    }

    static bool DrainPointBelongs(
        const PresentationCompletionState& _state,
        const PresentationDrainPoint&       _point
    ) noexcept {
        return _point.state_.get() == &_state &&
               _point.state_instance_id_ != 0 &&
               _point.state_instance_id_ ==
                   _state.GetStateInstanceId() &&
               (_point.all_identities_ ||
                (_point.presentation_epoch_ != 0 ||
                 _point.drawable_generation_ != 0));
    }

    static bool RecordMatchesDrainPoint(
        const Record&                 _record,
        const PresentationDrainPoint& _point
    ) noexcept {
        if (_record.issue_sequence >
            _point.through_issue_sequence_) {
            return false;
        }
        return _point.all_identities_ ||
               (_record.identity.presentation_epoch ==
                    _point.presentation_epoch_ &&
                _record.identity.drawable_generation ==
                    _point.drawable_generation_);
    }

    static bool HasOutstandingAtPoint(
        const PresentationCompletionState::Impl& _impl,
        const PresentationDrainPoint&             _point
    ) noexcept {
        for (const auto& [issue_sequence, record] :
             _impl.records) {
            if (issue_sequence >
                _point.through_issue_sequence_) {
                break;
            }
            if (RecordMatchesDrainPoint(record, _point)) {
                return true;
            }
        }
        return false;
    }
};

PresentationCompletionTicket::PresentationCompletionTicket(
    PresentationCompletionStateRef _state,
    PresentationCompletionIdentity _identity,
    std::uint64_t                  _issue_sequence,
    std::uint32_t                  _fence_slot,
    std::uint64_t                  _slot_generation,
    SharedPtr<std::atomic<EPresentationWsiCompletionOutcome>>
        _wsi_outcome
) noexcept :
    state_(std::move(_state)),
    identity_(_identity),
    issue_sequence_(_issue_sequence),
    fence_slot_(_fence_slot),
    slot_generation_(_slot_generation),
    wsi_outcome_(std::move(_wsi_outcome)) {}

bool PresentationCompletionTicket::IsValid() const noexcept {
    return state_ &&
           identity_.state_instance_id != 0 &&
           identity_.state_instance_id ==
               state_->GetStateInstanceId() &&
           issue_sequence_ != 0 &&
           wsi_outcome_;
}

const PresentationCompletionIdentity&
PresentationCompletionTicket::GetIdentity() const noexcept {
    return identity_;
}

std::uint64_t
PresentationCompletionTicket::GetIssueSequence() const noexcept {
    return issue_sequence_;
}

std::uint32_t
PresentationCompletionTicket::GetFenceSlot() const noexcept {
    return fence_slot_;
}

std::uint64_t
PresentationCompletionTicket::GetSlotGeneration() const noexcept {
    return slot_generation_;
}

EPresentationWsiCompletionOutcome
PresentationCompletionTicket::GetWsiOutcome() const noexcept {
    return wsi_outcome_ ?
               wsi_outcome_->load(std::memory_order_acquire) :
               EPresentationWsiCompletionOutcome::Pending;
}

bool PresentationDrainPoint::IsValid() const noexcept {
    return state_ &&
           state_instance_id_ != 0 &&
           state_instance_id_ == state_->GetStateInstanceId();
}

std::uint64_t
PresentationDrainPoint::GetStateInstanceId() const noexcept {
    return state_instance_id_;
}

std::uint64_t
PresentationDrainPoint::GetThroughIssueSequence() const noexcept {
    return through_issue_sequence_;
}

std::uint64_t
PresentationDrainPoint::GetPresentationEpoch() const noexcept {
    return presentation_epoch_;
}

std::uint64_t
PresentationDrainPoint::GetDrawableGeneration() const noexcept {
    return drawable_generation_;
}

bool PresentationDrainPoint::IncludesAllIdentities() const noexcept {
    return all_identities_;
}

const PresentationCompletionStateRef&
PresentationDrainPoint::GetState() const noexcept {
    return state_;
}

PresentationCompletionStateRef PresentationCompletionState::Create() {
    return PresentationCompletionStateRef(
        new PresentationCompletionState(NextStateInstanceId())
    );
}

PresentationCompletionState::PresentationCompletionState(
    std::uint64_t _state_instance_id
) :
    state_instance_id_(_state_instance_id),
    impl_(std::make_shared<Impl>()) {}

PresentationCompletionState::~PresentationCompletionState() = default;

std::uint64_t
PresentationCompletionState::GetInstanceId() const noexcept {
    return state_instance_id_;
}

PresentationCompletionTicket PresentationCompletionState::Reserve(
    PresentationCompletionIdentity _identity,
    std::uint32_t                   _fence_slot,
    std::uint64_t                   _slot_generation
) {
    if (_identity.state_instance_id != state_instance_id_) {
        return {};
    }

    std::uint64_t issue_sequence = 0;
    auto wsi_outcome = MakeShared<
        std::atomic<EPresentationWsiCompletionOutcome>>(
        EPresentationWsiCompletionOutcome::Pending
    );
    {
        std::scoped_lock lock(impl_->mutex);
        do {
            issue_sequence = impl_->next_issue_sequence++;
        } while (issue_sequence == 0 ||
                 impl_->records.contains(issue_sequence));

        Impl::Record record{};
        record.identity        = _identity;
        record.issue_sequence  = issue_sequence;
        record.fence_slot      = _fence_slot;
        record.slot_generation = _slot_generation;
        record.wsi_outcome     = wsi_outcome;
        impl_->records.emplace(issue_sequence, std::move(record));
        impl_->last_issue_sequence = issue_sequence;
    }

    return PresentationCompletionTicket(
        shared_from_this(),
        _identity,
        issue_sequence,
        _fence_slot,
        _slot_generation,
        std::move(wsi_outcome)
    );
}

bool PresentationCompletionState::SetWsiMode(
    const PresentationCompletionTicket& _ticket,
    EPresentationWsiCompletionMode      _mode
) noexcept {
    bool notify_wsi = false;
    {
        std::scoped_lock lock(impl_->mutex);
        const auto record =
            PresentationCompletionStateAccess::FindExactRecord(
                *impl_, *this, _ticket
            );
        if (record == impl_->records.end()) {
            return false;
        }
        if (record->second.wsi_mode.has_value()) {
            return record->second.wsi_mode.value() == _mode;
        }
        record->second.wsi_mode = _mode;
        if (_mode == EPresentationWsiCompletionMode::Immediate ||
            _mode == EPresentationWsiCompletionMode::Failed) {
            record->second.wsi_complete = true;
            record->second.wsi_outcome->store(
                _mode == EPresentationWsiCompletionMode::Immediate ?
                    EPresentationWsiCompletionOutcome::Succeeded :
                    EPresentationWsiCompletionOutcome::Failed,
                std::memory_order_release
            );
            notify_wsi                  = true;
        }
    }
    if (notify_wsi) {
        impl_->wsi_cv.notify_all();
    }
    return true;
}

bool PresentationCompletionState::MarkGpuComplete(
    const PresentationCompletionTicket& _ticket
) noexcept {
    std::scoped_lock lock(impl_->mutex);
    const auto record =
        PresentationCompletionStateAccess::FindExactRecord(
            *impl_, *this, _ticket
        );
    if (record == impl_->records.end()) {
        return false;
    }
    record->second.gpu_complete = true;
    return true;
}

bool PresentationCompletionState::MarkWsiComplete(
    const PresentationCompletionTicket& _ticket
) noexcept {
    {
        std::scoped_lock lock(impl_->mutex);
        const auto record =
            PresentationCompletionStateAccess::FindExactRecord(
                *impl_, *this, _ticket
            );
        if (record == impl_->records.end() ||
            !record->second.wsi_mode.has_value()) {
            return false;
        }
        switch (record->second.wsi_mode.value()) {
            case EPresentationWsiCompletionMode::PresentFence:
                record->second.wsi_complete = true;
                record->second.wsi_outcome->store(
                    EPresentationWsiCompletionOutcome::Succeeded,
                    std::memory_order_release
                );
                break;
            case EPresentationWsiCompletionMode::Immediate:
            case EPresentationWsiCompletionMode::Failed:
                return true;
            case EPresentationWsiCompletionMode::QueueIdleFallback:
                return false;
        }
    }
    impl_->wsi_cv.notify_all();
    return true;
}

bool PresentationCompletionState::MarkWsiFailed(
    const PresentationCompletionTicket& _ticket
) noexcept {
    {
        std::scoped_lock lock(impl_->mutex);
        const auto record =
            PresentationCompletionStateAccess::FindExactRecord(
                *impl_, *this, _ticket
            );
        if (record == impl_->records.end()) {
            return false;
        }
        record->second.wsi_mode =
            EPresentationWsiCompletionMode::Failed;
        record->second.wsi_complete = true;
        record->second.wsi_outcome->store(
            EPresentationWsiCompletionOutcome::Failed,
            std::memory_order_release
        );
    }
    impl_->wsi_cv.notify_all();
    return true;
}

bool PresentationCompletionState::PublishRetired(
    const PresentationCompletionTicket& _ticket
) noexcept {
    {
        std::scoped_lock lock(impl_->mutex);
        const auto record =
            PresentationCompletionStateAccess::FindExactRecord(
                *impl_, *this, _ticket
            );
        if (record == impl_->records.end() ||
            !record->second.IsTerminal()) {
            return false;
        }
        impl_->records.erase(record);
    }
    impl_->wsi_cv.notify_all();
    impl_->retired_cv.notify_all();
    return true;
}

std::size_t PresentationCompletionState::PublishRetired() noexcept {
    std::size_t retired_count = 0;
    {
        std::scoped_lock lock(impl_->mutex);
        for (auto record = impl_->records.begin();
             record != impl_->records.end();) {
            if (record->second.IsTerminal()) {
                record = impl_->records.erase(record);
                ++retired_count;
            } else {
                ++record;
            }
        }
    }
    if (retired_count != 0) {
        impl_->wsi_cv.notify_all();
        impl_->retired_cv.notify_all();
    }
    return retired_count;
}

void PresentationCompletionState::WaitForWsi(
    const PresentationCompletionTicket& _ticket
) const {
    std::unique_lock lock(impl_->mutex);
    if (!PresentationCompletionStateAccess::
            TicketStructurallyBelongs(*this, _ticket)) {
        return;
    }
    impl_->wsi_cv.wait(lock, [&] {
        const auto record =
            PresentationCompletionStateAccess::FindExactRecord(
                *impl_, *this, _ticket
            );
        return record == impl_->records.end() ||
               record->second.wsi_complete;
    });
}

bool PresentationCompletionState::WaitForWsi(
    const PresentationCompletionTicket& _ticket,
    std::chrono::nanoseconds             _timeout
) const {
    std::unique_lock lock(impl_->mutex);
    if (!PresentationCompletionStateAccess::
            TicketStructurallyBelongs(*this, _ticket)) {
        return false;
    }
    const bool reached_terminal = impl_->wsi_cv.wait_for(
        lock,
        _timeout,
        [&] {
            const auto record =
                PresentationCompletionStateAccess::FindExactRecord(
                    *impl_, *this, _ticket
                );
            return record == impl_->records.end() ||
                   record->second.wsi_complete;
        }
    );
    if (!reached_terminal) {
        return false;
    }
    const auto record =
        PresentationCompletionStateAccess::FindExactRecord(
            *impl_, *this, _ticket
        );
    return record == impl_->records.end() ||
           record->second.wsi_complete;
}

PresentationDrainPoint PresentationCompletionState::Freeze(
    std::uint64_t _presentation_epoch,
    std::uint64_t _drawable_generation
) {
    PresentationDrainPoint point{};
    point.state_               = shared_from_this();
    point.state_instance_id_   = state_instance_id_;
    point.presentation_epoch_  = _presentation_epoch;
    point.drawable_generation_ = _drawable_generation;
    point.all_identities_ =
        _presentation_epoch == 0 && _drawable_generation == 0;
    {
        std::scoped_lock lock(impl_->mutex);
        point.through_issue_sequence_ =
            impl_->last_issue_sequence;
    }
    return point;
}

bool PresentationCompletionState::RequiresQueueIdle(
    const PresentationDrainPoint& _point
) const noexcept {
    if (!PresentationCompletionStateAccess::DrainPointBelongs(
            *this, _point
        )) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    for (const auto& [issue_sequence, record] : impl_->records) {
        if (issue_sequence > _point.through_issue_sequence_) {
            break;
        }
        if (PresentationCompletionStateAccess::
                RecordMatchesDrainPoint(record, _point) &&
            record.wsi_mode ==
                EPresentationWsiCompletionMode::QueueIdleFallback &&
            !record.wsi_complete) {
            return true;
        }
    }
    return false;
}

bool PresentationCompletionState::ResolveQueueIdle(
    const PresentationDrainPoint& _point
) noexcept {
    if (!PresentationCompletionStateAccess::DrainPointBelongs(
            *this, _point
        )) {
        return false;
    }

    bool resolved_any = false;
    {
        std::scoped_lock lock(impl_->mutex);
        for (auto& [issue_sequence, record] : impl_->records) {
            if (issue_sequence > _point.through_issue_sequence_) {
                break;
            }
            if (PresentationCompletionStateAccess::
                    RecordMatchesDrainPoint(record, _point) &&
                record.wsi_mode ==
                    EPresentationWsiCompletionMode::QueueIdleFallback &&
                !record.wsi_complete) {
                record.wsi_complete = true;
                record.wsi_outcome->store(
                    EPresentationWsiCompletionOutcome::Succeeded,
                    std::memory_order_release
                );
                resolved_any        = true;
            }
        }
    }
    if (resolved_any) {
        impl_->wsi_cv.notify_all();
    }
    return resolved_any;
}

void PresentationCompletionState::WaitRetired(
    const PresentationDrainPoint& _point
) const {
    if (!PresentationCompletionStateAccess::DrainPointBelongs(
            *this, _point
        )) {
        return;
    }
    std::unique_lock lock(impl_->mutex);
    impl_->retired_cv.wait(lock, [&] {
        return !PresentationCompletionStateAccess::
            HasOutstandingAtPoint(*impl_, _point);
    });
}

bool PresentationCompletionState::WaitRetired(
    const PresentationDrainPoint& _point,
    std::chrono::nanoseconds       _timeout
) const {
    if (!PresentationCompletionStateAccess::DrainPointBelongs(
            *this, _point
        )) {
        return false;
    }
    std::unique_lock lock(impl_->mutex);
    return impl_->retired_cv.wait_for(lock, _timeout, [&] {
        return !PresentationCompletionStateAccess::
            HasOutstandingAtPoint(*impl_, _point);
    });
}

bool PresentationCompletionState::HasOutstanding() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return !impl_->records.empty();
}

std::size_t
PresentationCompletionState::OutstandingCount() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->records.size();
}

} // namespace Moer::Render
