#ifndef MOER_ENGINE_RHI_QUERY_H
#define MOER_ENGINE_RHI_QUERY_H

#include "RenderAPI.h"
#include "misc/STL.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace Moer::Render {

enum class QueryKind : std::uint8_t {
    Timestamp = 0,
    Occlusion = 1,
};

enum class QueryStatus : std::uint8_t {
    Pending = 0,
    Ready   = 1,
    Error   = 2,
};

struct TimestampQueryResult {
    std::uint64_t begin_tick{0};
    std::uint64_t end_tick{0};
    std::uint32_t valid_bits{0};
    double        tick_period_ns{0.0};
    double        duration_ns{0.0};
};

struct OcclusionQueryResult {
    // Exact only when the backend requested a precise native query. A
    // visibility-only backend leaves this empty instead of exposing an
    // implementation-defined nonzero value as a real sample count.
    std::optional<std::uint64_t> sample_count{};
    bool                         visible{false};
};

struct QueryResult {
    QueryKind   kind{QueryKind::Timestamp};
    QueryStatus status{QueryStatus::Pending};
    std::uint64_t query_id{0};
    std::string name{};
    std::variant<std::monostate, TimestampQueryResult, OcclusionQueryResult> payload{};
    std::string error_reason{};
};

class QueryToken;

// Copyable handle to a single terminal query result. Resolution is one-shot:
// Pending may transition to Ready or Error exactly once and never changes
// afterwards. A terminal result is not a recording-ownership handoff: opaque
// shutdown may publish Error while a producer or commit gate remains Pending.
class RENDER_API QueryFuture {
    struct SharedState;

public:
    using Callback = std::function<void(const QueryResult&)>;

    QueryFuture() = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] QueryStatus Status() const noexcept;

    void Wait() const;
    [[nodiscard]] bool WaitFor(std::chrono::nanoseconds _timeout) const;

    template<typename Rep, typename Period>
    [[nodiscard]] bool WaitFor(std::chrono::duration<Rep, Period> _timeout) const {
        return WaitFor(std::chrono::duration_cast<std::chrono::nanoseconds>(_timeout));
    }

    [[nodiscard]] QueryResult Get() const;
    [[nodiscard]] std::optional<QueryResult> TryGet() const;

    // Every successfully registered callback is invoked exactly once. User
    // callback exceptions are contained and never escape the resolver or this
    // function.
    void Then(Callback _callback) const;

private:
    explicit QueryFuture(std::shared_ptr<SharedState> _state) noexcept;

    static QueryFuture Create(
        QueryKind       _kind,
        std::uint64_t   _query_id,
        std::string_view _name
    );

    bool PublishTimestamp(
        const TimestampQueryResult& _result,
        std::uint64_t               _notification_owner
    ) const noexcept;
    bool PublishOcclusion(
        const OcclusionQueryResult& _result,
        std::uint64_t               _notification_owner
    ) const noexcept;
    bool PublishError(
        std::string_view _reason,
        std::uint64_t    _notification_owner
    ) const noexcept;
    void NotifyTerminal(std::uint64_t _notification_owner) const noexcept;

    std::shared_ptr<SharedState> state_{};

    friend class QueryToken;
};

// Stable query identity carried by CommandList, CmdSubmit, QueryCmd and user
// futures. The Resolve* functions form the backend/completion-owner seam; they
// are idempotent and cannot overwrite an existing terminal result.
class RENDER_API QueryToken {
public:
    QueryToken() = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] std::uint64_t Id() const noexcept;
    [[nodiscard]] QueryKind Kind() const noexcept;
    [[nodiscard]] const std::string& Name() const noexcept;
    [[nodiscard]] QueryFuture GetFuture() const noexcept;

    void Then(QueryFuture::Callback _callback) const;

private:
    QueryToken(
        std::uint64_t   _id,
        QueryKind       _kind,
        std::uint64_t   _owner_id,
        std::string_view _name
    );

    [[nodiscard]] std::uint64_t OwnerId() const noexcept;
    bool PublishTimestamp(
        const TimestampQueryResult& _result,
        std::uint64_t               _notification_owner
    ) const noexcept;
    bool PublishOcclusion(
        const OcclusionQueryResult& _result,
        std::uint64_t               _notification_owner
    ) const noexcept;
    bool PublishErrorIfPending(
        std::string_view _reason,
        std::uint64_t    _notification_owner
    ) const noexcept;
    void NotifyTerminal(std::uint64_t _notification_owner) const noexcept;

    std::uint64_t id_{0};
    QueryKind     kind_{QueryKind::Timestamp};
    std::uint64_t owner_id_{0};
    std::shared_ptr<const std::string> name_{};
    QueryFuture   future_{};

    friend class CommandList;
    friend class QueryBackendAccess;
};

// Opaque ownership ticket for a terminalization transaction. Only the
// transaction that wins Pending -> Ready/Error may release callbacks. This
// preserves Completion-thread ownership when GPU completion races shutdown or
// frontend rejection.
class RENDER_API QueryPublishBatch {
public:
    QueryPublishBatch() = default;

    [[nodiscard]] bool Valid() const noexcept {
        return notification_owner_ != 0;
    }

private:
    explicit QueryPublishBatch(std::uint64_t _notification_owner) noexcept :
        notification_owner_(_notification_owner) {}

    std::uint64_t notification_owner_{0};

    friend class QueryBackendAccess;
    friend class QueryCancellationDomain;
    friend class QueryCancellationView;
};

// Narrow internal seam used by native query runtimes, completion ownership,
// and terminal rejection paths. QueryToken intentionally exposes no direct
// Ready/Error mutation to normal query consumers.
class RENDER_API QueryBackendAccess final {
public:
    [[nodiscard]] static QueryPublishBatch BeginPublishBatch() noexcept;

    // Single-token convenience APIs publish and notify immediately.
    static bool ResolveTimestamp(
        const QueryToken&           _token,
        const TimestampQueryResult& _result
    ) noexcept;
    static bool ResolveOcclusion(
        const QueryToken&           _token,
        const OcclusionQueryResult& _result
    ) noexcept;
    static bool ResolveErrorIfPending(
        const QueryToken& _token,
        std::string_view  _reason
    ) noexcept;

    // Batch owners use a two-phase transition: publish every result first,
    // then notify. This prevents one query callback from blocking on another
    // query in the same completion/cancellation transaction while it is still
    // Pending.
    static bool PublishTimestamp(
        const QueryToken&           _token,
        const TimestampQueryResult& _result,
        QueryPublishBatch           _batch
    ) noexcept;
    static bool PublishOcclusion(
        const QueryToken&           _token,
        const OcclusionQueryResult& _result,
        QueryPublishBatch           _batch
    ) noexcept;
    static bool PublishErrorIfPending(
        const QueryToken& _token,
        std::string_view  _reason,
        QueryPublishBatch _batch
    ) noexcept;
    static void NotifyTerminal(
        const QueryToken& _token,
        QueryPublishBatch _batch
    ) noexcept;
    static void PublishErrorsIfPending(
        std::span<const QueryToken> _tokens,
        std::string_view            _reason,
        QueryPublishBatch           _batch
    ) noexcept;
    static void NotifyTerminals(
        std::span<const QueryToken> _tokens,
        QueryPublishBatch           _batch
    ) noexcept;
    static void ResolveErrorsIfPending(
        std::span<const QueryToken> _tokens,
        std::string_view            _reason
    ) noexcept;
};

struct QueryCancellationState;
class CommandList;

// Copyable shutdown handle for one CommandList generation. Views remain valid
// for identity/diagnostics after the originating list submits or drains, but
// Cancel can win only while that generation is still owned by a mutable
// producer.
class RENDER_API QueryCancellationView {
public:
    QueryCancellationView() = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] bool IsCancelled() const noexcept;

    // Returns true only for the thread that performs the one-shot transition;
    // stale views return false once Submit has sealed the generation.
    // All registered tokens become Error before this function returns, but
    // user callbacks remain deferred until the owning CommandList reaches a
    // submission/destruction/rejection boundary. This makes the view safe for
    // opaque-producer shutdown without releasing client code on the arbitrary
    // cancellation thread. Error publication does not complete the recording
    // gate or transfer CommandList ownership; the producer/graph remains the
    // sole mutable owner until every producer and commit gate is terminal.
    bool Cancel(std::string_view _reason = "query generation was cancelled") const noexcept;

    // Cross-source rejection uses the same two-phase transaction as native
    // completion: publish every cancellation first, then release callbacks for
    // every source. These transaction calls are internal ownership seams;
    // Cancel() deliberately performs only the publish phase.
    bool PublishCancellation(
        std::string_view _reason,
        QueryPublishBatch _batch
    ) const noexcept;
    void NotifyCancellation(QueryPublishBatch _batch) const noexcept;

    // Used by the owning CommandList after an opaque shutdown cancellation.
    // Shutdown may publish Error without running callbacks while the producer
    // is still mutable; once the producer reaches submission, destruction, or
    // rejection it transfers or releases the stored notification owner behind
    // its signals.
    void NotifyPublishedCancellation() const noexcept;

private:
    // Seals this generation and transfers a deferred cancellation transaction
    // into an immutable submit.
    // A valid result owns every token registered with this generation; the
    // caller must retain that token set and use the returned batch when it
    // eventually releases terminal callbacks.
    [[nodiscard]] QueryPublishBatch TakePublishedCancellationBatch(
        Array<QueryToken>& _tokens
    ) const noexcept;

    explicit QueryCancellationView(
        std::shared_ptr<QueryCancellationState> _state
    ) noexcept;

    std::shared_ptr<QueryCancellationState> state_{};

    friend class QueryCancellationDomain;
    friend class CommandList;
};

// Mutable registration owner held by CommandList. A fresh domain is created
// after every Submit/rejection drain so cancellation cannot leak into a later
// recording generation.
class RENDER_API QueryCancellationDomain {
public:
    QueryCancellationDomain();

    QueryCancellationDomain(const QueryCancellationDomain&)            = delete;
    QueryCancellationDomain& operator=(const QueryCancellationDomain&) = delete;
    QueryCancellationDomain(QueryCancellationDomain&&) noexcept        = default;
    QueryCancellationDomain& operator=(QueryCancellationDomain&&) noexcept = default;

    [[nodiscard]] QueryCancellationView GetView() const noexcept;
    [[nodiscard]] bool IsCancelled() const noexcept;
    void Register(const QueryToken& _token) const;

private:
    std::shared_ptr<QueryCancellationState> state_{};
};

} // namespace Moer::Render

#endif
