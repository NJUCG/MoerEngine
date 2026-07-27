#include "rhi/RHIReadback.h"

#include "rhi/RHIThreadOwnership.h"

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace Moer::Render {
namespace {

enum class ReadbackPayloadStatus : std::uint8_t {
    Pending = 0,
    Ready   = 1,
    Error   = 2,
};

ReadbackResult InvalidReadbackResult() {
    ReadbackResult result{};
    result.status       = ReadbackStatus::Error;
    result.error_reason = "invalid readback future";
    return result;
}

void InvokeReadbackCallbackNoexcept(
    const ReadbackFuture::Callback& _callback,
    const ReadbackResult&           _result
) noexcept {
    if (!_callback) {
        return;
    }
    try {
        _callback(_result);
    } catch (...) {
        // A faulty observer must not terminate a Completion/rejection owner.
    }
}

} // namespace

struct ReadbackPayloadState {
    explicit ReadbackPayloadState(std::size_t _byte_size) :
        bytes(std::make_shared<Array<byte>>(_byte_size)) {}

    mutable std::mutex           mutex{};
    std::condition_variable      cv{};
    std::shared_ptr<Array<byte>> bytes{};
    ReadbackPayloadStatus        status{ReadbackPayloadStatus::Pending};
    std::string                  error_reason{};
};

struct ReadbackProducerLease {
    explicit ReadbackProducerLease(
        std::weak_ptr<ReadbackPayloadState> _state
    ) noexcept :
        state(std::move(_state)) {}

    ~ReadbackProducerLease() {
        const std::shared_ptr<ReadbackPayloadState> payload = state.lock();
        if (!payload) {
            return;
        }

        bool notify = false;
        {
            std::scoped_lock lock(payload->mutex);
            if (payload->status == ReadbackPayloadStatus::Pending) {
                payload->status = ReadbackPayloadStatus::Error;
                try {
                    payload->error_reason =
                        "readback producer retired without materializing payload";
                } catch (...) {
                }
                notify = true;
            }
        }
        if (notify) {
            payload->cv.notify_all();
        }
    }

    std::weak_ptr<ReadbackPayloadState> state{};
};

ReadbackFuture::ReadbackFuture(
    std::uint64_t                         _readback_id,
    std::shared_ptr<const std::string>    _name,
    std::shared_ptr<ReadbackPayloadState> _state,
    GpuCompletionFuture                   _completion
) noexcept :
    readback_id_(_readback_id),
    name_(std::move(_name)),
    state_(std::move(_state)),
    completion_(std::move(_completion)) {}

bool ReadbackFuture::Valid() const noexcept {
    return readback_id_ != 0 && name_ && state_ && completion_.Valid();
}

ReadbackStatus ReadbackFuture::Status() const noexcept {
    if (!Valid()) {
        return ReadbackStatus::Error;
    }

    const GpuCompletionStatus completion_status = completion_.Status();
    if (completion_status == GpuCompletionStatus::Pending) {
        return ReadbackStatus::Pending;
    }
    if (completion_status == GpuCompletionStatus::Error) {
        return ReadbackStatus::Error;
    }

    std::scoped_lock lock(state_->mutex);
    switch (state_->status) {
        case ReadbackPayloadStatus::Ready:
            return ReadbackStatus::Ready;
        case ReadbackPayloadStatus::Error:
            return ReadbackStatus::Error;
        case ReadbackPayloadStatus::Pending:
        default:
            return ReadbackStatus::Pending;
    }
}

bool ReadbackFuture::IsReady() const noexcept {
    return Valid() && Status() != ReadbackStatus::Pending;
}

std::size_t ReadbackFuture::ExpectedByteSize() const noexcept {
    if (!state_ || !state_->bytes) {
        return 0;
    }
    return state_->bytes->size();
}

void ReadbackFuture::Wait() const {
    if (!Valid()) {
        return;
    }

    const GpuCompletionResult completion_result = completion_.Get();
    if (completion_result.status != GpuCompletionStatus::Ready) {
        return;
    }

    std::unique_lock lock(state_->mutex);
    if (state_->status == ReadbackPayloadStatus::Pending &&
        !IsRHIBlockingCallAllowedOnCurrentThread()) {
        throw std::logic_error(
            "an RHI owner thread cannot block on a pending readback payload"
        );
    }
    state_->cv.wait(lock, [state = state_] {
        return state->status != ReadbackPayloadStatus::Pending;
    });
}

bool ReadbackFuture::WaitFor(
    std::chrono::nanoseconds _timeout
) const {
    if (!Valid()) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + _timeout;
    if (!completion_.WaitFor(_timeout)) {
        return false;
    }
    const std::optional<GpuCompletionResult> completion_result =
        completion_.TryGet();
    if (!completion_result.has_value()) {
        return false;
    }
    if (completion_result->status != GpuCompletionStatus::Ready) {
        return true;
    }

    std::unique_lock lock(state_->mutex);
    if (state_->status == ReadbackPayloadStatus::Pending &&
        !IsRHIBlockingCallAllowedOnCurrentThread()) {
        throw std::logic_error(
            "an RHI owner thread cannot block on a pending readback payload"
        );
    }
    return state_->cv.wait_until(lock, deadline, [state = state_] {
        return state->status != ReadbackPayloadStatus::Pending;
    });
}

ReadbackResult ReadbackFuture::BuildResult(
    const GpuCompletionResult& _completion_result
) const {
    if (!Valid()) {
        return InvalidReadbackResult();
    }

    ReadbackResult result{};
    result.readback_id = readback_id_;
    result.name        = name_ ? *name_ : std::string{};

    if (_completion_result.status == GpuCompletionStatus::Pending) {
        result.status = ReadbackStatus::Pending;
        return result;
    }
    if (_completion_result.status == GpuCompletionStatus::Error) {
        result.status       = ReadbackStatus::Error;
        result.error_reason = _completion_result.error_reason;
        return result;
    }

    std::scoped_lock lock(state_->mutex);
    switch (state_->status) {
        case ReadbackPayloadStatus::Ready:
            result.status = ReadbackStatus::Ready;
            result.data = std::static_pointer_cast<const Array<byte>>(
                state_->bytes
            );
            break;
        case ReadbackPayloadStatus::Error:
            result.status       = ReadbackStatus::Error;
            result.error_reason = state_->error_reason;
            break;
        case ReadbackPayloadStatus::Pending:
        default:
            result.status = ReadbackStatus::Pending;
            break;
    }
    return result;
}

ReadbackResult ReadbackFuture::Get() const {
    if (!Valid()) {
        return InvalidReadbackResult();
    }
    Wait();
    const GpuCompletionResult completion_result = completion_.Get();
    return BuildResult(completion_result);
}

std::optional<ReadbackResult> ReadbackFuture::TryGet() const {
    if (!Valid()) {
        return InvalidReadbackResult();
    }

    const std::optional<GpuCompletionResult> completion_result =
        completion_.TryGet();
    if (!completion_result.has_value()) {
        return std::nullopt;
    }
    const ReadbackResult result = BuildResult(*completion_result);
    if (result.status == ReadbackStatus::Pending) {
        return std::nullopt;
    }
    return result;
}

void ReadbackFuture::Then(Callback _callback) const {
    if (!_callback) {
        return;
    }
    if (!Valid()) {
        InvokeReadbackCallbackNoexcept(
            _callback, InvalidReadbackResult()
        );
        return;
    }

    const ReadbackFuture self = *this;
    completion_.Then(
        [self, callback = std::move(_callback)](
            const GpuCompletionResult& _result
        ) {
            try {
                ReadbackResult result = self.BuildResult(_result);
                if (result.status == ReadbackStatus::Pending) {
                    // The token's internal guard is registered before this
                    // adapter and terminalizes a missing payload first.
                    result.status = ReadbackStatus::Error;
                    result.error_reason =
                        "readback notification observed a pending payload";
                }
                InvokeReadbackCallbackNoexcept(callback, result);
            } catch (...) {
                // Preserve exactly-once callback delivery even if assembling
                // the diagnostic/name snapshot runs out of memory.
                ReadbackResult fallback{};
                fallback.status      = ReadbackStatus::Error;
                fallback.readback_id = self.readback_id_;
                InvokeReadbackCallbackNoexcept(callback, fallback);
            }
        }
    );
}

ReadbackToken::ReadbackToken(
    std::uint64_t       _readback_id,
    std::size_t         _byte_size,
    std::string_view    _name,
    GpuCompletionFuture _completion
) :
    readback_id_(_readback_id),
    name_(std::make_shared<const std::string>(_name)),
    state_(std::make_shared<ReadbackPayloadState>(_byte_size)),
    producer_lease_(
        std::make_shared<ReadbackProducerLease>(state_)
    ),
    completion_(std::move(_completion)) {
    // Register exactly one guard before the token or a derived Future can
    // escape. Vulkan publishes logical GPU completion before running
    // allocator completion callbacks, but releases notifications only after
    // those callbacks. A still-Pending payload at notification is therefore
    // a backend failure.
    const std::shared_ptr<ReadbackPayloadState> state = state_;
    completion_.Then(
        [state](const GpuCompletionResult& _result) noexcept {
            if (!state ||
                _result.status != GpuCompletionStatus::Ready) {
                return;
            }

            bool notify = false;
            {
                std::scoped_lock lock(state->mutex);
                if (state->status == ReadbackPayloadStatus::Pending) {
                    state->status = ReadbackPayloadStatus::Error;
                    try {
                        state->error_reason =
                            "GPU completed without materializing readback payload";
                    } catch (...) {
                    }
                    notify = true;
                }
            }
            if (notify) {
                state->cv.notify_all();
            }
        }
    );
}

bool ReadbackToken::Valid() const noexcept {
    return readback_id_ != 0 && name_ && state_ && producer_lease_ &&
           completion_.Valid();
}

std::uint64_t ReadbackToken::Id() const noexcept {
    return readback_id_;
}

std::size_t ReadbackToken::ByteSize() const noexcept {
    return state_ && state_->bytes ? state_->bytes->size() : 0;
}

const std::string& ReadbackToken::Name() const noexcept {
    static const std::string empty_name{};
    return name_ ? *name_ : empty_name;
}

ReadbackFuture ReadbackToken::GetFuture() const noexcept {
    if (!Valid()) {
        return {};
    }
    return ReadbackFuture(
        readback_id_, name_, state_, completion_
    );
}

ReadbackToken ReadbackBackendAccess::Create(
    std::uint64_t       _readback_id,
    std::size_t         _byte_size,
    std::string_view    _name,
    GpuCompletionFuture _completion
) {
    if (_readback_id == 0 || _byte_size == 0 || !_completion.Valid()) {
        return {};
    }
    return ReadbackToken(
        _readback_id, _byte_size, _name, std::move(_completion)
    );
}

bool ReadbackBackendAccess::MaterializePayload(
    const ReadbackToken& _token,
    void*                _context,
    PayloadWriter        _writer
) noexcept {
    if (!_token.Valid() || !_writer) {
        return false;
    }

    bool succeeded = false;
    {
        std::scoped_lock lock(_token.state_->mutex);
        if (_token.state_->status != ReadbackPayloadStatus::Pending) {
            return false;
        }
        try {
            _writer(
                _context,
                std::span<byte>(
                    _token.state_->bytes->data(),
                    _token.state_->bytes->size()
                )
            );
            _token.state_->status = ReadbackPayloadStatus::Ready;
            _token.state_->error_reason.clear();
            succeeded = true;
        } catch (const std::exception& error) {
            _token.state_->status = ReadbackPayloadStatus::Error;
            try {
                _token.state_->error_reason =
                    std::string("readback payload materialization failed: ") +
                    error.what();
            } catch (...) {
            }
        } catch (...) {
            _token.state_->status = ReadbackPayloadStatus::Error;
            try {
                _token.state_->error_reason =
                    "readback payload materialization failed";
            } catch (...) {
            }
        }
    }
    _token.state_->cv.notify_all();
    return succeeded;
}

bool ReadbackBackendAccess::PublishPayloadErrorIfPending(
    const ReadbackToken& _token,
    std::string_view     _reason
) noexcept {
    if (!_token.Valid()) {
        return false;
    }

    std::string reason{};
    try {
        reason.assign(_reason);
    } catch (...) {
    }

    {
        std::scoped_lock lock(_token.state_->mutex);
        if (_token.state_->status != ReadbackPayloadStatus::Pending) {
            return false;
        }
        _token.state_->status       = ReadbackPayloadStatus::Error;
        _token.state_->error_reason = std::move(reason);
    }
    _token.state_->cv.notify_all();
    return true;
}

} // namespace Moer::Render
