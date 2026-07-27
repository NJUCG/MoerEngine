#include "rhi/RHICompletion.h"
#include "rhi/RHIThreadOwnership.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace Moer::Render {
namespace {

std::atomic<std::uint64_t> g_gpu_completion_publish_batch_id{1};

std::uint64_t NextGpuCompletionPublishBatchId() noexcept {
    std::uint64_t id = g_gpu_completion_publish_batch_id.fetch_add(
        1, std::memory_order_relaxed
    );
    while (id == 0) {
        id = g_gpu_completion_publish_batch_id.fetch_add(
            1, std::memory_order_relaxed
        );
    }
    return id;
}

GpuCompletionResult InvalidFutureResult() {
    GpuCompletionResult result{};
    result.status       = GpuCompletionStatus::Error;
    result.error_reason = "invalid GPU completion future";
    return result;
}

void InvokeCallbackNoexcept(
    const GpuCompletionFuture::Callback& _callback,
    const GpuCompletionResult&           _result
) noexcept {
    if (!_callback) {
        return;
    }
    try {
        _callback(_result);
    } catch (...) {
        // A faulty observer must not terminate the Completion owner.
    }
}

} // namespace

struct GpuCompletionFuture::SharedState {
    mutable std::mutex      mutex{};
    std::condition_variable cv{};
    GpuCompletionResult     result{};
    Array<Callback>         callbacks{};
    bool                    notification_released{false};
    std::uint64_t           notification_owner{0};
};

struct GpuCompletionCancellationState {
    mutable std::mutex       mutex{};
    bool                     cancelled{false};
    bool                     sealed{false};
    std::string              reason{};
    Array<GpuCompletionToken> tokens{};
    std::uint64_t            notification_owner{0};
};

GpuCompletionFuture::GpuCompletionFuture(
    std::shared_ptr<SharedState> _state
) noexcept :
    state_(std::move(_state)) {}

GpuCompletionFuture GpuCompletionFuture::Create(
    std::uint64_t    _completion_id,
    std::string_view _name
) {
    auto state                  = std::make_shared<SharedState>();
    state->result.completion_id = _completion_id;
    state->result.name          = _name;
    return GpuCompletionFuture(std::move(state));
}

bool GpuCompletionFuture::Valid() const noexcept {
    return static_cast<bool>(state_);
}

bool GpuCompletionFuture::IsReady() const noexcept {
    if (!state_) {
        return false;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->result.status != GpuCompletionStatus::Pending;
}

GpuCompletionStatus GpuCompletionFuture::Status() const noexcept {
    if (!state_) {
        return GpuCompletionStatus::Error;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->result.status;
}

void GpuCompletionFuture::Wait() const {
    if (!state_) {
        return;
    }
    std::unique_lock lock(state_->mutex);
    if (state_->result.status == GpuCompletionStatus::Pending &&
        !IsRHIBlockingCallAllowedOnCurrentThread()) {
        throw std::logic_error(
            "an RHI owner thread cannot block on a pending GPU completion"
        );
    }
    state_->cv.wait(lock, [state = state_] {
        return state->result.status != GpuCompletionStatus::Pending;
    });
}

bool GpuCompletionFuture::WaitFor(
    std::chrono::nanoseconds _timeout
) const {
    if (!state_) {
        return false;
    }
    std::unique_lock lock(state_->mutex);
    if (state_->result.status == GpuCompletionStatus::Pending &&
        !IsRHIBlockingCallAllowedOnCurrentThread()) {
        throw std::logic_error(
            "an RHI owner thread cannot block on a pending GPU completion"
        );
    }
    return state_->cv.wait_for(lock, _timeout, [state = state_] {
        return state->result.status != GpuCompletionStatus::Pending;
    });
}

GpuCompletionResult GpuCompletionFuture::Get() const {
    if (!state_) {
        return InvalidFutureResult();
    }
    Wait();
    std::scoped_lock lock(state_->mutex);
    return state_->result;
}

std::optional<GpuCompletionResult> GpuCompletionFuture::TryGet() const {
    if (!state_) {
        return InvalidFutureResult();
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->result.status == GpuCompletionStatus::Pending) {
        return std::nullopt;
    }
    return state_->result;
}

void GpuCompletionFuture::Then(Callback _callback) const {
    const std::shared_ptr<SharedState> state = state_;
    if (!state || !_callback) {
        return;
    }

    bool invoke_now = false;
    {
        std::scoped_lock lock(state->mutex);
        if (state->result.status == GpuCompletionStatus::Pending ||
            !state->notification_released) {
            state->callbacks.emplace_back(std::move(_callback));
        } else {
            invoke_now = true;
        }
    }
    if (invoke_now) {
        InvokeCallbackNoexcept(_callback, state->result);
    }
}

bool GpuCompletionFuture::PublishReady(
    std::uint64_t _notification_owner
) const noexcept {
    if (!state_ || _notification_owner == 0) {
        return false;
    }
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->result.status != GpuCompletionStatus::Pending) {
            return false;
        }
        state_->result.status = GpuCompletionStatus::Ready;
        state_->result.error_reason.clear();
        state_->notification_released = false;
        state_->notification_owner    = _notification_owner;
    }
    state_->cv.notify_all();
    return true;
}

bool GpuCompletionFuture::PublishError(
    std::string_view _reason,
    std::uint64_t    _notification_owner
) const noexcept {
    if (!state_ || _notification_owner == 0) {
        return false;
    }

    std::string reason{};
    try {
        reason.assign(_reason);
    } catch (...) {
        // Error is still a valid terminal classification under allocation
        // pressure even when its diagnostic text cannot be retained.
    }

    {
        std::scoped_lock lock(state_->mutex);
        if (state_->result.status != GpuCompletionStatus::Pending) {
            return false;
        }
        state_->result.status       = GpuCompletionStatus::Error;
        state_->result.error_reason = std::move(reason);
        state_->notification_released = false;
        state_->notification_owner    = _notification_owner;
    }
    state_->cv.notify_all();
    return true;
}

void GpuCompletionFuture::NotifyTerminal(
    std::uint64_t _notification_owner
) const noexcept {
    const std::shared_ptr<SharedState> state = state_;
    if (!state || _notification_owner == 0) {
        return;
    }

    Array<Callback> callbacks{};
    {
        std::scoped_lock lock(state->mutex);
        if (state->result.status == GpuCompletionStatus::Pending ||
            state->notification_released ||
            state->notification_owner != _notification_owner) {
            return;
        }
        state->notification_released = true;
        callbacks = std::move(state->callbacks);
    }
    for (const Callback& callback : callbacks) {
        InvokeCallbackNoexcept(callback, state->result);
    }
}

GpuCompletionToken::GpuCompletionToken(
    std::uint64_t    _id,
    std::string_view _name
) :
    id_(_id),
    name_(std::make_shared<const std::string>(_name)),
    future_(GpuCompletionFuture::Create(_id, *name_)) {}

bool GpuCompletionToken::Valid() const noexcept {
    return id_ != 0 && future_.Valid();
}

bool GpuCompletionToken::IsReady() const noexcept {
    return future_.IsReady();
}

std::uint64_t GpuCompletionToken::Id() const noexcept {
    return id_;
}

const std::string& GpuCompletionToken::Name() const noexcept {
    static const std::string empty_name{};
    return name_ ? *name_ : empty_name;
}

GpuCompletionFuture GpuCompletionToken::GetFuture() const noexcept {
    return future_;
}

void GpuCompletionToken::Then(
    GpuCompletionFuture::Callback _callback
) const {
    future_.Then(std::move(_callback));
}

bool GpuCompletionToken::PublishReady(
    std::uint64_t _notification_owner
) const noexcept {
    return Valid() && future_.PublishReady(_notification_owner);
}

bool GpuCompletionToken::PublishErrorIfPending(
    std::string_view _reason,
    std::uint64_t    _notification_owner
) const noexcept {
    return Valid() &&
           future_.PublishError(_reason, _notification_owner);
}

void GpuCompletionToken::NotifyTerminal(
    std::uint64_t _notification_owner
) const noexcept {
    future_.NotifyTerminal(_notification_owner);
}

GpuCompletionPublishBatch
GpuCompletionBackendAccess::BeginPublishBatch() noexcept {
    return GpuCompletionPublishBatch(
        NextGpuCompletionPublishBatchId()
    );
}

bool GpuCompletionBackendAccess::ResolveReady(
    const GpuCompletionToken& _token
) noexcept {
    const GpuCompletionPublishBatch batch = BeginPublishBatch();
    const bool published = PublishReady(_token, batch);
    if (published) {
        NotifyTerminal(_token, batch);
    }
    return published;
}

bool GpuCompletionBackendAccess::ResolveErrorIfPending(
    const GpuCompletionToken& _token,
    std::string_view          _reason
) noexcept {
    const GpuCompletionPublishBatch batch = BeginPublishBatch();
    const bool published =
        PublishErrorIfPending(_token, _reason, batch);
    if (published) {
        NotifyTerminal(_token, batch);
    }
    return published;
}

bool GpuCompletionBackendAccess::PublishReady(
    const GpuCompletionToken& _token,
    GpuCompletionPublishBatch _batch
) noexcept {
    return _token.PublishReady(_batch.notification_owner_);
}

bool GpuCompletionBackendAccess::PublishErrorIfPending(
    const GpuCompletionToken& _token,
    std::string_view          _reason,
    GpuCompletionPublishBatch _batch
) noexcept {
    return _token.PublishErrorIfPending(
        _reason, _batch.notification_owner_
    );
}

void GpuCompletionBackendAccess::NotifyTerminal(
    const GpuCompletionToken& _token,
    GpuCompletionPublishBatch _batch
) noexcept {
    _token.NotifyTerminal(_batch.notification_owner_);
}

void GpuCompletionBackendAccess::PublishReady(
    std::span<const GpuCompletionToken> _tokens,
    GpuCompletionPublishBatch           _batch
) noexcept {
    for (const GpuCompletionToken& token : _tokens) {
        (void)PublishReady(token, _batch);
    }
}

void GpuCompletionBackendAccess::PublishErrorsIfPending(
    std::span<const GpuCompletionToken> _tokens,
    std::string_view                    _reason,
    GpuCompletionPublishBatch           _batch
) noexcept {
    for (const GpuCompletionToken& token : _tokens) {
        (void)PublishErrorIfPending(token, _reason, _batch);
    }
}

void GpuCompletionBackendAccess::PublishOutcome(
    std::span<const GpuCompletionToken> _tokens,
    bool                                _gpu_success,
    std::string_view                    _failure_reason,
    GpuCompletionPublishBatch           _batch
) noexcept {
    if (_gpu_success) {
        PublishReady(_tokens, _batch);
    } else {
        PublishErrorsIfPending(_tokens, _failure_reason, _batch);
    }
}

void GpuCompletionBackendAccess::NotifyTerminals(
    std::span<const GpuCompletionToken> _tokens,
    GpuCompletionPublishBatch           _batch
) noexcept {
    for (const GpuCompletionToken& token : _tokens) {
        NotifyTerminal(token, _batch);
    }
}

void GpuCompletionBackendAccess::ResolveErrorsIfPending(
    std::span<const GpuCompletionToken> _tokens,
    std::string_view                    _reason
) noexcept {
    const GpuCompletionPublishBatch batch = BeginPublishBatch();
    PublishErrorsIfPending(_tokens, _reason, batch);
    NotifyTerminals(_tokens, batch);
}

GpuCompletionCancellationView::GpuCompletionCancellationView(
    std::shared_ptr<GpuCompletionCancellationState> _state
) noexcept :
    state_(std::move(_state)) {}

bool GpuCompletionCancellationView::Valid() const noexcept {
    return static_cast<bool>(state_);
}

bool GpuCompletionCancellationView::IsCancelled() const noexcept {
    if (!state_) {
        return false;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->cancelled;
}

bool GpuCompletionCancellationView::Cancel(
    std::string_view _reason
) const noexcept {
    return PublishCancellation(
        _reason, GpuCompletionBackendAccess::BeginPublishBatch()
    );
}

bool GpuCompletionCancellationView::PublishCancellation(
    std::string_view          _reason,
    GpuCompletionPublishBatch _batch
) const noexcept {
    if (!state_ || !_batch.Valid()) {
        return false;
    }

    std::string reason{};
    try {
        reason.assign(_reason);
    } catch (...) {
    }

    {
        std::scoped_lock lock(state_->mutex);
        if (state_->cancelled || state_->sealed) {
            return false;
        }
        state_->cancelled          = true;
        state_->reason             = std::move(reason);
        state_->notification_owner = _batch.notification_owner_;
        GpuCompletionBackendAccess::PublishErrorsIfPending(
            state_->tokens, state_->reason, _batch
        );
    }
    return true;
}

void GpuCompletionCancellationView::NotifyCancellation(
    GpuCompletionPublishBatch _batch
) const noexcept {
    if (!state_ || !_batch.Valid()) {
        return;
    }

    Array<GpuCompletionToken> tokens{};
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->notification_owner !=
            _batch.notification_owner_) {
            return;
        }
        state_->notification_owner = 0;
        tokens = std::move(state_->tokens);
    }
    GpuCompletionBackendAccess::NotifyTerminals(tokens, _batch);
}

void GpuCompletionCancellationView::NotifyPublishedCancellation()
    const noexcept {
    if (!state_) {
        return;
    }

    Array<GpuCompletionToken> tokens{};
    std::uint64_t             notification_owner = 0;
    {
        std::scoped_lock lock(state_->mutex);
        notification_owner = state_->notification_owner;
        if (notification_owner == 0) {
            return;
        }
        state_->notification_owner = 0;
        tokens = std::move(state_->tokens);
    }
    GpuCompletionBackendAccess::NotifyTerminals(
        tokens, GpuCompletionPublishBatch(notification_owner)
    );
}

GpuCompletionPublishBatch
GpuCompletionCancellationView::TakePublishedCancellationBatch(
    Array<GpuCompletionToken>& _tokens
) const noexcept {
    if (!state_) {
        return {};
    }

    std::uint64_t notification_owner = 0;
    {
        std::scoped_lock lock(state_->mutex);
        state_->sealed     = true;
        notification_owner = state_->notification_owner;
        if (notification_owner == 0) {
            state_->tokens.clear();
            return {};
        }
        state_->notification_owner = 0;
        _tokens.clear();
        _tokens.swap(state_->tokens);
    }
    return GpuCompletionPublishBatch(notification_owner);
}

GpuCompletionCancellationDomain::GpuCompletionCancellationDomain() :
    state_(std::make_shared<GpuCompletionCancellationState>()) {}

GpuCompletionCancellationView
GpuCompletionCancellationDomain::GetView() const noexcept {
    return GpuCompletionCancellationView(state_);
}

bool GpuCompletionCancellationDomain::IsCancelled() const noexcept {
    return GetView().IsCancelled();
}

void GpuCompletionCancellationDomain::Register(
    const GpuCompletionToken& _token
) const {
    if (!_token.Valid() || !state_) {
        return;
    }

    bool             cancel_now = false;
    std::string_view reason{};
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->cancelled) {
            if (state_->notification_owner != 0) {
                const GpuCompletionPublishBatch deferred_batch(
                    state_->notification_owner
                );
                state_->tokens.emplace_back(_token);
                GpuCompletionBackendAccess::PublishErrorIfPending(
                    _token, state_->reason, deferred_batch
                );
                return;
            }
            cancel_now = true;
            reason     = state_->reason;
        } else {
            state_->tokens.emplace_back(_token);
        }
    }
    if (cancel_now) {
        GpuCompletionBackendAccess::ResolveErrorIfPending(
            _token, reason
        );
    }
}

} // namespace Moer::Render
