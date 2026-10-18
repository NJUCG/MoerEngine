#include "rhi/RHIPresentationCompletion.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace Moer::Render {

struct PresentationCompletionState::Impl {
    mutable std::mutex               mutex{};
    mutable std::condition_variable  cv{};
    Array<PresentationCompletionRef> records{};
    std::uint64_t                    next_issue_sequence{1};
};

PresentationCompletionStateRef PresentationCompletionState::Create() {
    return PresentationCompletionStateRef(new PresentationCompletionState());
}

PresentationCompletionState::PresentationCompletionState() : impl_(std::make_unique<Impl>()) {}
PresentationCompletionState::~PresentationCompletionState() = default;

PresentationCompletionRef
PresentationCompletionState::Reserve(std::uint64_t _presentation_epoch, std::uint64_t _drawable_generation) {
    auto record                  = MakeShared<PresentationCompletion>();
    record->state_               = shared_from_this();
    record->presentation_epoch_  = _presentation_epoch;
    record->drawable_generation_ = _drawable_generation;
    std::scoped_lock lock(impl_->mutex);
    record->issue_sequence_ = impl_->next_issue_sequence++;
    impl_->records.push_back(record);
    return record;
}

bool PresentationCompletionState::Owns(const PresentationCompletionRef& _record) const noexcept {
    return _record && _record->state_.lock().get() == this;
}

// Mutable record fields except the public atomic outcome use this state's
// mutex. A retired record cannot be changed by a late callback.
bool PresentationCompletionState::IsPending(const PresentationCompletionRef& _record) const noexcept {
    return Owns(_record) && !_record->retired_;
}

bool PresentationCompletionState::SetWsiMode(
    const PresentationCompletionRef& _record,
    EPresentationWsiCompletionMode   _mode
) noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (!IsPending(_record)) {
        return false;
    }
    if (_record->wsi_mode_) {
        return _record->wsi_mode_ == _mode;
    }
    _record->wsi_mode_ = _mode;
    if (_mode == EPresentationWsiCompletionMode::Immediate ||
        _mode == EPresentationWsiCompletionMode::Failed) {
        _record->wsi_outcome_.store(
            _mode == EPresentationWsiCompletionMode::Immediate ?
                EPresentationWsiCompletionOutcome::Succeeded :
                EPresentationWsiCompletionOutcome::Failed,
            std::memory_order_release
        );
        impl_->cv.notify_all();
    }
    return true;
}

bool PresentationCompletionState::MarkGpuComplete(const PresentationCompletionRef& _record) noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (!IsPending(_record)) {
        return false;
    }
    _record->gpu_complete_ = true;
    return true;
}

bool PresentationCompletionState::MarkWsiComplete(const PresentationCompletionRef& _record) noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (!IsPending(_record) || !_record->wsi_mode_) {
        return false;
    }
    if (_record->wsi_mode_ == EPresentationWsiCompletionMode::QueueIdleFallback) {
        return false;
    }
    if (_record->wsi_mode_ == EPresentationWsiCompletionMode::PresentFence) {
        _record->wsi_outcome_.store(EPresentationWsiCompletionOutcome::Succeeded, std::memory_order_release);
        impl_->cv.notify_all();
    }
    return true;
}

bool PresentationCompletionState::MarkImageAvailable(const PresentationCompletionRef& _previous_present
) noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (!IsPending(_previous_present) ||
        _previous_present->wsi_mode_ != EPresentationWsiCompletionMode::QueueIdleFallback) {
        return false;
    }
    _previous_present->wsi_outcome_.store(
        EPresentationWsiCompletionOutcome::Succeeded, std::memory_order_release
    );
    impl_->cv.notify_all();
    return true;
}

bool PresentationCompletionState::MarkWsiFailed(const PresentationCompletionRef& _record) noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (!IsPending(_record)) {
        return false;
    }
    _record->wsi_mode_ = EPresentationWsiCompletionMode::Failed;
    _record->wsi_outcome_.store(EPresentationWsiCompletionOutcome::Failed, std::memory_order_release);
    impl_->cv.notify_all();
    return true;
}

bool PresentationCompletionState::PublishRetired(const PresentationCompletionRef& _record) noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (!IsPending(_record) || !_record->IsTerminal()) {
        return false;
    }
    _record->retired_ = true;
    std::erase(impl_->records, _record);
    impl_->cv.notify_all();
    return true;
}

std::size_t PresentationCompletionState::PublishRetired() noexcept {
    std::scoped_lock lock(impl_->mutex);
    const auto       count = std::erase_if(impl_->records, [](const PresentationCompletionRef& record) {
        if (!record->IsTerminal()) {
            return false;
        }
        record->retired_ = true;
        return true;
    });
    if (count != 0) {
        impl_->cv.notify_all();
    }
    return count;
}

void PresentationCompletionState::WaitForWsi(const PresentationCompletionRef& _record) const {
    if (!Owns(_record)) {
        return;
    }
    std::unique_lock lock(impl_->mutex);
    impl_->cv.wait(lock, [&] {
        return _record->GetWsiOutcome() != EPresentationWsiCompletionOutcome::Pending;
    });
}

bool PresentationCompletionState::WaitForWsi(
    const PresentationCompletionRef& _record,
    std::chrono::nanoseconds         _timeout
) const {
    if (!Owns(_record)) {
        return false;
    }
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, _timeout, [&] {
        return _record->GetWsiOutcome() != EPresentationWsiCompletionOutcome::Pending;
    });
}

PresentationDrainPoint
PresentationCompletionState::Freeze(std::uint64_t _presentation_epoch, std::uint64_t _drawable_generation) {
    PresentationDrainPoint point;
    point.state_ = shared_from_this();
    std::scoped_lock lock(impl_->mutex);
    for (const auto& record : impl_->records) {
        if ((_presentation_epoch == 0 && _drawable_generation == 0) ||
            (record->presentation_epoch_ == _presentation_epoch &&
             record->drawable_generation_ == _drawable_generation)) {
            point.records_.push_back(record);
        }
    }
    return point;
}

bool PresentationCompletionState::RequiresQueueIdle(const PresentationDrainPoint& _point) const noexcept {
    if (_point.state_.get() != this) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    return std::ranges::any_of(_point.records_, [](const auto& record) {
        return record->wsi_mode_ == EPresentationWsiCompletionMode::QueueIdleFallback &&
               record->GetWsiOutcome() == EPresentationWsiCompletionOutcome::Pending;
    });
}

bool PresentationCompletionState::ResolveQueueIdle(const PresentationDrainPoint& _point) noexcept {
    if (_point.state_.get() != this) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    bool             resolved = false;
    for (const auto& record : _point.records_) {
        if (record->wsi_mode_ == EPresentationWsiCompletionMode::QueueIdleFallback &&
            record->GetWsiOutcome() == EPresentationWsiCompletionOutcome::Pending) {
            record->wsi_outcome_.store(
                EPresentationWsiCompletionOutcome::Succeeded, std::memory_order_release
            );
            resolved = true;
        }
    }
    if (resolved) {
        impl_->cv.notify_all();
    }
    return resolved;
}

void PresentationCompletionState::WaitRetired(const PresentationDrainPoint& _point) const {
    if (_point.state_.get() != this) {
        return;
    }
    std::unique_lock lock(impl_->mutex);
    impl_->cv.wait(lock, [&] {
        return std::ranges::all_of(_point.records_, [](const auto& record) {
            return record->retired_;
        });
    });
}

bool PresentationCompletionState::WaitRetired(
    const PresentationDrainPoint& _point,
    std::chrono::nanoseconds      _timeout
) const {
    if (_point.state_.get() != this) {
        return false;
    }
    std::unique_lock lock(impl_->mutex);
    return impl_->cv.wait_for(lock, _timeout, [&] {
        return std::ranges::all_of(_point.records_, [](const auto& record) {
            return record->retired_;
        });
    });
}

bool PresentationCompletionState::HasOutstanding() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return !impl_->records.empty();
}

std::size_t PresentationCompletionState::OutstandingCount() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->records.size();
}

} // namespace Moer::Render
