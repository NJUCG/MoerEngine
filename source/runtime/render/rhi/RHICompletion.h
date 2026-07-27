#ifndef MOER_ENGINE_RHI_COMPLETION_H
#define MOER_ENGINE_RHI_COMPLETION_H

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

namespace Moer::Render {

enum class GpuCompletionStatus : std::uint8_t {
    Pending = 0,
    Ready   = 1,
    Error   = 2,
};

struct GpuCompletionResult {
    GpuCompletionStatus status{GpuCompletionStatus::Pending};
    std::uint64_t       completion_id{0};
    std::string         name{};
    std::string         error_reason{};
};

class GpuCompletionToken;

// Copyable host observation handle for one logical GPU submission. It does
// not expose a backend fence: native synchronization remains owned by the
// Submission/Completion pipeline. Pending transitions to Ready or Error once.
// A terminal status is an observation result, not a recording-ownership
// handoff. Opaque shutdown may publish Error while a producer or commit gate
// is still Pending; only all ownership gates becoming terminal permits its
// CommandList to be inspected.
class RENDER_API GpuCompletionFuture {
    struct SharedState;

public:
    using Callback = std::function<void(const GpuCompletionResult&)>;

    GpuCompletionFuture() = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] GpuCompletionStatus Status() const noexcept;

    // Blocking on a pending future from an RHI owner thread would deadlock
    // its own producer, so Wait/WaitFor/Get throw std::logic_error there.
    // Terminal results remain readable from every thread.
    void Wait() const;
    [[nodiscard]] bool WaitFor(std::chrono::nanoseconds _timeout) const;

    template<typename Rep, typename Period>
    [[nodiscard]] bool WaitFor(
        std::chrono::duration<Rep, Period> _timeout
    ) const {
        return WaitFor(
            std::chrono::duration_cast<std::chrono::nanoseconds>(_timeout)
        );
    }

    [[nodiscard]] GpuCompletionResult Get() const;
    [[nodiscard]] std::optional<GpuCompletionResult> TryGet() const;

    // A registered callback runs exactly once. Exceptions from user code are
    // contained by the resolving Completion/rejection owner.
    void Then(Callback _callback) const;

private:
    explicit GpuCompletionFuture(
        std::shared_ptr<SharedState> _state
    ) noexcept;

    static GpuCompletionFuture Create(
        std::uint64_t    _completion_id,
        std::string_view _name
    );

    bool PublishReady(std::uint64_t _notification_owner) const noexcept;
    bool PublishError(
        std::string_view _reason,
        std::uint64_t    _notification_owner
    ) const noexcept;
    void NotifyTerminal(std::uint64_t _notification_owner) const noexcept;

    std::shared_ptr<SharedState> state_{};

    friend class GpuCompletionToken;
};

// Strong identity retained by CommandList, CmdSubmit and the native
// Completion packet. Mutation is restricted to GpuCompletionBackendAccess.
class RENDER_API GpuCompletionToken {
public:
    GpuCompletionToken() = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] std::uint64_t Id() const noexcept;
    [[nodiscard]] const std::string& Name() const noexcept;
    [[nodiscard]] GpuCompletionFuture GetFuture() const noexcept;

    void Then(GpuCompletionFuture::Callback _callback) const;

private:
    GpuCompletionToken(
        std::uint64_t    _id,
        std::string_view _name
    );

    bool PublishReady(std::uint64_t _notification_owner) const noexcept;
    bool PublishErrorIfPending(
        std::string_view _reason,
        std::uint64_t    _notification_owner
    ) const noexcept;
    void NotifyTerminal(std::uint64_t _notification_owner) const noexcept;

    std::uint64_t                    id_{0};
    std::shared_ptr<const std::string> name_{};
    GpuCompletionFuture              future_{};

    friend class CommandList;
    friend class GpuCompletionBackendAccess;
};

// Opaque ticket separating terminal-state publication from callback release.
// A whole backend batch can publish every sibling first, then notify in stable
// source order without callback-induced Completion deadlocks.
class RENDER_API GpuCompletionPublishBatch {
public:
    GpuCompletionPublishBatch() = default;

    [[nodiscard]] bool Valid() const noexcept {
        return notification_owner_ != 0;
    }

private:
    explicit GpuCompletionPublishBatch(
        std::uint64_t _notification_owner
    ) noexcept :
        notification_owner_(_notification_owner) {}

    std::uint64_t notification_owner_{0};

    friend class GpuCompletionBackendAccess;
    friend class GpuCompletionCancellationDomain;
    friend class GpuCompletionCancellationView;
};

class RENDER_API GpuCompletionBackendAccess final {
public:
    [[nodiscard]] static GpuCompletionPublishBatch
    BeginPublishBatch() noexcept;

    static bool ResolveReady(const GpuCompletionToken& _token) noexcept;
    static bool ResolveErrorIfPending(
        const GpuCompletionToken& _token,
        std::string_view          _reason
    ) noexcept;

    static bool PublishReady(
        const GpuCompletionToken& _token,
        GpuCompletionPublishBatch _batch
    ) noexcept;
    static bool PublishErrorIfPending(
        const GpuCompletionToken& _token,
        std::string_view          _reason,
        GpuCompletionPublishBatch _batch
    ) noexcept;
    static void NotifyTerminal(
        const GpuCompletionToken& _token,
        GpuCompletionPublishBatch _batch
    ) noexcept;

    static void PublishReady(
        std::span<const GpuCompletionToken> _tokens,
        GpuCompletionPublishBatch           _batch
    ) noexcept;
    static void PublishErrorsIfPending(
        std::span<const GpuCompletionToken> _tokens,
        std::string_view                    _reason,
        GpuCompletionPublishBatch           _batch
    ) noexcept;
    static void PublishOutcome(
        std::span<const GpuCompletionToken> _tokens,
        bool                                _gpu_success,
        std::string_view                    _failure_reason,
        GpuCompletionPublishBatch           _batch
    ) noexcept;
    static void NotifyTerminals(
        std::span<const GpuCompletionToken> _tokens,
        GpuCompletionPublishBatch           _batch
    ) noexcept;
    static void ResolveErrorsIfPending(
        std::span<const GpuCompletionToken> _tokens,
        std::string_view                    _reason
    ) noexcept;
};

struct GpuCompletionCancellationState;
class CommandList;

// Thread-safe shutdown view for one mutable CommandList generation. Cancel
// publishes Error immediately so Wait/Get are bounded, while callback release
// stays deferred until the producer reaches a safe ownership boundary. It
// neither completes producer/commit gates nor seals/transfers its CommandList;
// their owners must still stop mutation and Signal()/Fail() those gates.
class RENDER_API GpuCompletionCancellationView {
public:
    GpuCompletionCancellationView() = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] bool IsCancelled() const noexcept;

    bool Cancel(
        std::string_view _reason =
            "GPU completion generation was cancelled"
    ) const noexcept;
    bool PublishCancellation(
        std::string_view          _reason,
        GpuCompletionPublishBatch _batch
    ) const noexcept;
    void NotifyCancellation(
        GpuCompletionPublishBatch _batch
    ) const noexcept;
    void NotifyPublishedCancellation() const noexcept;

private:
    [[nodiscard]] GpuCompletionPublishBatch
    TakePublishedCancellationBatch(
        Array<GpuCompletionToken>& _tokens
    ) const noexcept;

    explicit GpuCompletionCancellationView(
        std::shared_ptr<GpuCompletionCancellationState> _state
    ) noexcept;

    std::shared_ptr<GpuCompletionCancellationState> state_{};

    friend class GpuCompletionCancellationDomain;
    friend class CommandList;
};

class RENDER_API GpuCompletionCancellationDomain {
public:
    GpuCompletionCancellationDomain();

    GpuCompletionCancellationDomain(
        const GpuCompletionCancellationDomain&
    ) = delete;
    GpuCompletionCancellationDomain& operator=(
        const GpuCompletionCancellationDomain&
    ) = delete;
    GpuCompletionCancellationDomain(
        GpuCompletionCancellationDomain&&
    ) noexcept = default;
    GpuCompletionCancellationDomain& operator=(
        GpuCompletionCancellationDomain&&
    ) noexcept = default;

    [[nodiscard]] GpuCompletionCancellationView GetView() const noexcept;
    [[nodiscard]] bool IsCancelled() const noexcept;
    void Register(const GpuCompletionToken& _token) const;

private:
    std::shared_ptr<GpuCompletionCancellationState> state_{};
};

} // namespace Moer::Render

#endif
