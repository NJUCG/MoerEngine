#include "rhi/RHIQuery.h"

#include "misc/STL.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace Moer::Render {
namespace {

std::atomic<std::uint64_t> g_query_publish_batch_id{1};

std::uint64_t NextQueryPublishBatchId() noexcept {
    std::uint64_t id =
        g_query_publish_batch_id.fetch_add(1, std::memory_order_relaxed);
    while (id == 0) {
        id = g_query_publish_batch_id.fetch_add(1, std::memory_order_relaxed);
    }
    return id;
}

QueryResult InvalidFutureResult() {
    QueryResult result{};
    result.status       = QueryStatus::Error;
    result.error_reason = "invalid query future";
    return result;
}

void InvokeCallbackNoexcept(
    const QueryFuture::Callback& _callback,
    const QueryResult&           _result
) noexcept {
    if (!_callback) {
        return;
    }
    try {
        _callback(_result);
    } catch (...) {
        // Completion and rejection owners must remain alive even when client
        // notification code is faulty.
    }
}

} // namespace

struct QueryFuture::SharedState {
    mutable std::mutex      mutex{};
    std::condition_variable cv{};
    QueryResult             result{};
    Array<Callback>         callbacks{};
    bool                    notification_released{false};
    std::uint64_t           notification_owner{0};
};

struct QueryCancellationState {
    mutable std::mutex mutex{};
    bool               cancelled{false};
    bool               sealed{false};
    std::string        reason{};
    Array<QueryToken>  tokens{};
    std::uint64_t      notification_owner{0};
};

QueryFuture::QueryFuture(std::shared_ptr<SharedState> _state) noexcept :
    state_(std::move(_state)) {}

QueryFuture QueryFuture::Create(
    QueryKind        _kind,
    std::uint64_t    _query_id,
    std::string_view _name
) {
    auto state             = std::make_shared<SharedState>();
    state->result.kind     = _kind;
    state->result.query_id = _query_id;
    state->result.name     = _name;
    return QueryFuture(std::move(state));
}

bool QueryFuture::Valid() const noexcept {
    return static_cast<bool>(state_);
}

bool QueryFuture::IsReady() const noexcept {
    if (!state_) {
        return false;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->result.status != QueryStatus::Pending;
}

QueryStatus QueryFuture::Status() const noexcept {
    if (!state_) {
        return QueryStatus::Error;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->result.status;
}

void QueryFuture::Wait() const {
    if (!state_) {
        return;
    }
    std::unique_lock lock(state_->mutex);
    state_->cv.wait(lock, [state = state_] {
        return state->result.status != QueryStatus::Pending;
    });
}

bool QueryFuture::WaitFor(std::chrono::nanoseconds _timeout) const {
    if (!state_) {
        return false;
    }
    std::unique_lock lock(state_->mutex);
    return state_->cv.wait_for(lock, _timeout, [state = state_] {
        return state->result.status != QueryStatus::Pending;
    });
}

QueryResult QueryFuture::Get() const {
    if (!state_) {
        return InvalidFutureResult();
    }
    Wait();
    std::scoped_lock lock(state_->mutex);
    return state_->result;
}

std::optional<QueryResult> QueryFuture::TryGet() const {
    if (!state_) {
        return InvalidFutureResult();
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->result.status == QueryStatus::Pending) {
        return std::nullopt;
    }
    return state_->result;
}

void QueryFuture::Then(Callback _callback) const {
    // A callback may release the QueryFuture/QueryToken that invoked it. Keep
    // the shared state alive until the callback returns so its QueryResult
    // reference and any later callbacks in this notification remain valid.
    const std::shared_ptr<SharedState> state = state_;
    if (!state || !_callback) {
        return;
    }

    bool invoke_now = false;
    {
        std::scoped_lock lock(state->mutex);
        if (state->result.status == QueryStatus::Pending ||
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

bool QueryFuture::PublishTimestamp(
    const TimestampQueryResult& _result,
    std::uint64_t               _notification_owner
) const noexcept {
    if (!state_ || _notification_owner == 0) {
        return false;
    }

    {
        std::scoped_lock lock(state_->mutex);
        if (state_->result.status != QueryStatus::Pending) {
            return false;
        }
        state_->result.status = QueryStatus::Ready;
        state_->result.payload.emplace<TimestampQueryResult>(_result);
        state_->result.error_reason.clear();
        state_->notification_released = false;
        state_->notification_owner    = _notification_owner;
    }
    // Wait/Get observe terminal state independently from callback ownership.
    // In particular, opaque shutdown may defer user callbacks until the
    // producer retires while still guaranteeing bounded waiters.
    state_->cv.notify_all();
    return true;
}

bool QueryFuture::PublishOcclusion(
    const OcclusionQueryResult& _result,
    std::uint64_t               _notification_owner
) const noexcept {
    if (!state_ || _notification_owner == 0) {
        return false;
    }

    {
        std::scoped_lock lock(state_->mutex);
        if (state_->result.status != QueryStatus::Pending) {
            return false;
        }
        state_->result.status = QueryStatus::Ready;
        state_->result.payload.emplace<OcclusionQueryResult>(_result);
        state_->result.error_reason.clear();
        state_->notification_released = false;
        state_->notification_owner    = _notification_owner;
    }
    state_->cv.notify_all();
    return true;
}

bool QueryFuture::PublishError(
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
        // The state must still become terminal under memory pressure. An empty
        // reason remains distinguishable from a successful result by status.
    }

    {
        std::scoped_lock lock(state_->mutex);
        if (state_->result.status != QueryStatus::Pending) {
            return false;
        }
        state_->result.status = QueryStatus::Error;
        state_->result.payload.emplace<std::monostate>();
        state_->result.error_reason = std::move(reason);
        state_->notification_released = false;
        state_->notification_owner    = _notification_owner;
    }
    state_->cv.notify_all();
    return true;
}

void QueryFuture::NotifyTerminal(
    std::uint64_t _notification_owner
) const noexcept {
    const std::shared_ptr<SharedState> state = state_;
    if (!state || _notification_owner == 0) {
        return;
    }

    Array<Callback> callbacks{};
    {
        std::scoped_lock lock(state->mutex);
        if (state->result.status == QueryStatus::Pending ||
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

QueryToken::QueryToken(
    std::uint64_t    _id,
    QueryKind        _kind,
    std::uint64_t    _owner_id,
    std::string_view _name
) :
    id_(_id),
    kind_(_kind),
    owner_id_(_owner_id),
    name_(std::make_shared<const std::string>(_name)),
    future_(QueryFuture::Create(_kind, _id, *name_)) {}

bool QueryToken::Valid() const noexcept {
    return id_ != 0 && owner_id_ != 0 && future_.Valid();
}

bool QueryToken::IsReady() const noexcept {
    return future_.IsReady();
}

std::uint64_t QueryToken::Id() const noexcept {
    return id_;
}

QueryKind QueryToken::Kind() const noexcept {
    return kind_;
}

const std::string& QueryToken::Name() const noexcept {
    static const std::string empty_name{};
    return name_ ? *name_ : empty_name;
}

QueryFuture QueryToken::GetFuture() const noexcept {
    return future_;
}

void QueryToken::Then(QueryFuture::Callback _callback) const {
    future_.Then(std::move(_callback));
}

bool QueryToken::PublishTimestamp(
    const TimestampQueryResult& _result,
    std::uint64_t               _notification_owner
) const noexcept {
    if (!Valid() || kind_ != QueryKind::Timestamp) {
        return false;
    }
    return future_.PublishTimestamp(_result, _notification_owner);
}

bool QueryToken::PublishOcclusion(
    const OcclusionQueryResult& _result,
    std::uint64_t               _notification_owner
) const noexcept {
    if (!Valid() || kind_ != QueryKind::Occlusion) {
        return false;
    }
    return future_.PublishOcclusion(_result, _notification_owner);
}

bool QueryToken::PublishErrorIfPending(
    std::string_view _reason,
    std::uint64_t    _notification_owner
) const noexcept {
    if (!Valid()) {
        return false;
    }
    return future_.PublishError(_reason, _notification_owner);
}

void QueryToken::NotifyTerminal(
    std::uint64_t _notification_owner
) const noexcept {
    future_.NotifyTerminal(_notification_owner);
}

std::uint64_t QueryToken::OwnerId() const noexcept {
    return owner_id_;
}

QueryPublishBatch QueryBackendAccess::BeginPublishBatch() noexcept {
    return QueryPublishBatch(NextQueryPublishBatchId());
}

bool QueryBackendAccess::ResolveTimestamp(
    const QueryToken&           _token,
    const TimestampQueryResult& _result
) noexcept {
    const QueryPublishBatch batch = BeginPublishBatch();
    const bool published =
        _token.PublishTimestamp(_result, batch.notification_owner_);
    if (published) {
        _token.NotifyTerminal(batch.notification_owner_);
    }
    return published;
}

bool QueryBackendAccess::ResolveOcclusion(
    const QueryToken&           _token,
    const OcclusionQueryResult& _result
) noexcept {
    const QueryPublishBatch batch = BeginPublishBatch();
    const bool published =
        _token.PublishOcclusion(_result, batch.notification_owner_);
    if (published) {
        _token.NotifyTerminal(batch.notification_owner_);
    }
    return published;
}

bool QueryBackendAccess::ResolveErrorIfPending(
    const QueryToken& _token,
    std::string_view  _reason
) noexcept {
    const QueryPublishBatch batch = BeginPublishBatch();
    const bool published =
        _token.PublishErrorIfPending(_reason, batch.notification_owner_);
    if (published) {
        _token.NotifyTerminal(batch.notification_owner_);
    }
    return published;
}

bool QueryBackendAccess::PublishTimestamp(
    const QueryToken&           _token,
    const TimestampQueryResult& _result,
    QueryPublishBatch           _batch
) noexcept {
    return _token.PublishTimestamp(_result, _batch.notification_owner_);
}

bool QueryBackendAccess::PublishOcclusion(
    const QueryToken&           _token,
    const OcclusionQueryResult& _result,
    QueryPublishBatch           _batch
) noexcept {
    return _token.PublishOcclusion(_result, _batch.notification_owner_);
}

bool QueryBackendAccess::PublishErrorIfPending(
    const QueryToken& _token,
    std::string_view  _reason,
    QueryPublishBatch _batch
) noexcept {
    return _token.PublishErrorIfPending(
        _reason, _batch.notification_owner_
    );
}

void QueryBackendAccess::NotifyTerminal(
    const QueryToken& _token,
    QueryPublishBatch _batch
) noexcept {
    _token.NotifyTerminal(_batch.notification_owner_);
}

void QueryBackendAccess::PublishErrorsIfPending(
    std::span<const QueryToken> _tokens,
    std::string_view            _reason,
    QueryPublishBatch           _batch
) noexcept {
    for (const QueryToken& token : _tokens) {
        PublishErrorIfPending(token, _reason, _batch);
    }
}

void QueryBackendAccess::NotifyTerminals(
    std::span<const QueryToken> _tokens,
    QueryPublishBatch           _batch
) noexcept {
    for (const QueryToken& token : _tokens) {
        NotifyTerminal(token, _batch);
    }
}

void QueryBackendAccess::ResolveErrorsIfPending(
    std::span<const QueryToken> _tokens,
    std::string_view            _reason
) noexcept {
    const QueryPublishBatch batch = BeginPublishBatch();
    PublishErrorsIfPending(_tokens, _reason, batch);
    NotifyTerminals(_tokens, batch);
}

QueryCancellationView::QueryCancellationView(
    std::shared_ptr<QueryCancellationState> _state
) noexcept :
    state_(std::move(_state)) {}

bool QueryCancellationView::Valid() const noexcept {
    return static_cast<bool>(state_);
}

bool QueryCancellationView::IsCancelled() const noexcept {
    if (!state_) {
        return false;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->cancelled;
}

bool QueryCancellationView::Cancel(std::string_view _reason) const noexcept {
    const QueryPublishBatch batch = QueryBackendAccess::BeginPublishBatch();
    // Opaque shutdown may call Cancel from a thread that does not own the
    // mutable producer. Publish Error immediately so waiters cannot hang, but
    // leave callback notification with the CommandList generation's eventual
    // submission/destruction/rejection owner.
    return PublishCancellation(_reason, batch);
}

bool QueryCancellationView::PublishCancellation(
    std::string_view  _reason,
    QueryPublishBatch _batch
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
        state_->cancelled = true;
        state_->reason    = std::move(reason);
        state_->notification_owner = _batch.notification_owner_;
        QueryBackendAccess::PublishErrorsIfPending(
            state_->tokens, state_->reason, _batch
        );
    }
    return true;
}

void QueryCancellationView::NotifyCancellation(
    QueryPublishBatch _batch
) const noexcept {
    if (!state_ || !_batch.Valid()) {
        return;
    }

    Array<QueryToken> tokens{};
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->notification_owner != _batch.notification_owner_) {
            return;
        }
        state_->notification_owner = 0;
        tokens = std::move(state_->tokens);
    }
    QueryBackendAccess::NotifyTerminals(tokens, _batch);
}

void QueryCancellationView::NotifyPublishedCancellation() const noexcept {
    if (!state_) {
        return;
    }

    Array<QueryToken> tokens{};
    std::uint64_t     notification_owner = 0;
    {
        std::scoped_lock lock(state_->mutex);
        notification_owner = state_->notification_owner;
        if (notification_owner == 0) {
            return;
        }
        state_->notification_owner = 0;
        tokens = std::move(state_->tokens);
    }
    QueryBackendAccess::NotifyTerminals(
        tokens, QueryPublishBatch(notification_owner)
    );
}

QueryPublishBatch QueryCancellationView::TakePublishedCancellationBatch(
    Array<QueryToken>& _tokens
) const noexcept {
    if (!state_) {
        return {};
    }

    std::uint64_t notification_owner = 0;
    {
        std::scoped_lock lock(state_->mutex);
        state_->sealed = true;
        notification_owner = state_->notification_owner;
        if (notification_owner == 0) {
            // CmdSubmit already owns the canonical token array. Do not let a
            // stale cancellation view retain a second copy after ownership
            // has crossed the recording boundary.
            state_->tokens.clear();
            return {};
        }
        state_->notification_owner = 0;
        _tokens.clear();
        _tokens.swap(state_->tokens);
    }
    return QueryPublishBatch(notification_owner);
}

QueryCancellationDomain::QueryCancellationDomain() :
    state_(std::make_shared<QueryCancellationState>()) {}

QueryCancellationView QueryCancellationDomain::GetView() const noexcept {
    return QueryCancellationView(state_);
}

bool QueryCancellationDomain::IsCancelled() const noexcept {
    return GetView().IsCancelled();
}

void QueryCancellationDomain::Register(const QueryToken& _token) const {
    if (!_token.Valid() || !state_) {
        return;
    }

    bool             cancel_now = false;
    std::string_view reason{};
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->cancelled) {
            if (state_->notification_owner != 0) {
                const QueryPublishBatch deferred_batch(
                    state_->notification_owner
                );
                state_->tokens.emplace_back(_token);
                QueryBackendAccess::PublishErrorIfPending(
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
        QueryBackendAccess::ResolveErrorIfPending(_token, reason);
    }
}

} // namespace Moer::Render
