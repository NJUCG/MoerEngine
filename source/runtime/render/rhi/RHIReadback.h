#ifndef MOER_ENGINE_RHI_READBACK_H
#define MOER_ENGINE_RHI_READBACK_H

#include "RenderAPI.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHICompletion.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace Moer::Render {

enum class ReadbackStatus : std::uint8_t {
    Pending = 0,
    Ready   = 1,
    Error   = 2,
};

// An owning immutable snapshot. Keeping the result alive keeps its byte
// storage alive even after the originating ReadbackFuture is released.
struct ReadbackResult {
    ReadbackStatus                    status{ReadbackStatus::Pending};
    std::uint64_t                     readback_id{0};
    std::string                       name{};
    std::shared_ptr<const Array<byte>> data{};
    std::string                       error_reason{};

    [[nodiscard]] std::span<const byte> Bytes() const noexcept {
        if (!data) {
            return {};
        }
        return std::span<const byte>(data->data(), data->size());
    }

    [[nodiscard]] std::size_t ByteSize() const noexcept {
        return data ? data->size() : 0;
    }

    template<typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] std::optional<T> ReadValue(
        std::size_t _byte_offset = 0
    ) const noexcept {
        if (status != ReadbackStatus::Ready || !data) {
            return std::nullopt;
        }
        const std::span<const byte> bytes = Bytes();
        if (_byte_offset > bytes.size() ||
            sizeof(T) > bytes.size() - _byte_offset) {
            return std::nullopt;
        }
        T value{};
        std::memcpy(
            &value, bytes.data() + _byte_offset, sizeof(T)
        );
        return value;
    }

    template<typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] std::optional<Array<T>> CopyAs() const {
        if (status != ReadbackStatus::Ready || !data) {
            return std::nullopt;
        }
        const std::span<const byte> bytes = Bytes();
        if (bytes.size() % sizeof(T) != 0) {
            return std::nullopt;
        }
        Array<T> values(bytes.size() / sizeof(T));
        if (!bytes.empty()) {
            std::memcpy(values.data(), bytes.data(), bytes.size());
        }
        return values;
    }
};

class ReadbackToken;
struct ReadbackPayloadState;
struct ReadbackProducerLease;

// Copyable host handle for one owning readback. Public readiness is the
// conjunction of logical GPU completion and payload materialization:
// observing the GPU completion alone is not enough because main publishes
// that state before staging-to-host completion callbacks run.
class RENDER_API ReadbackFuture {
public:
    using Callback = std::function<void(const ReadbackResult&)>;

    ReadbackFuture() = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] ReadbackStatus Status() const noexcept;
    [[nodiscard]] std::size_t ExpectedByteSize() const noexcept;

    // As with GpuCompletionFuture, an RHI owner cannot block on a component
    // that is still Pending. Terminal results remain readable everywhere.
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

    [[nodiscard]] ReadbackResult Get() const;
    [[nodiscard]] std::optional<ReadbackResult> TryGet() const;

    // Notification is delegated to the logical GpuCompletionFuture. The
    // token registers one internal payload guard before any Future escapes,
    // so callbacks never observe a successful GPU completion with an
    // unmaterialized payload.
    void Then(Callback _callback) const;

private:
    ReadbackFuture(
        std::uint64_t                         _readback_id,
        std::shared_ptr<const std::string>    _name,
        std::shared_ptr<ReadbackPayloadState> _state,
        GpuCompletionFuture                   _completion
    ) noexcept;

    [[nodiscard]] ReadbackResult BuildResult(
        const GpuCompletionResult& _completion_result
    ) const;

    std::uint64_t                         readback_id_{0};
    std::shared_ptr<const std::string>    name_{};
    std::shared_ptr<ReadbackPayloadState> state_{};
    GpuCompletionFuture                   completion_{};

    friend class ReadbackToken;
    friend class ReadbackBackendAccess;
};

// Producer-side payload identity retained by CopyBack commands and native
// allocator callbacks. It intentionally exposes no user callback API.
class RENDER_API ReadbackToken {
public:
    ReadbackToken() = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] std::uint64_t Id() const noexcept;
    [[nodiscard]] std::size_t ByteSize() const noexcept;
    [[nodiscard]] const std::string& Name() const noexcept;
    [[nodiscard]] ReadbackFuture GetFuture() const noexcept;

private:
    ReadbackToken(
        std::uint64_t        _readback_id,
        std::size_t          _byte_size,
        std::string_view     _name,
        GpuCompletionFuture  _completion
    );

    std::uint64_t                              readback_id_{0};
    std::shared_ptr<const std::string>         name_{};
    std::shared_ptr<ReadbackPayloadState>      state_{};
    std::shared_ptr<ReadbackProducerLease>     producer_lease_{};
    GpuCompletionFuture                        completion_{};

    friend class ReadbackBackendAccess;
};

// Narrow seam used by CommandList construction and native readback recorders.
// Payload mutation remains separate from GPU terminal publication.
class RENDER_API ReadbackBackendAccess final {
public:
    [[nodiscard]] static ReadbackToken Create(
        std::uint64_t        _readback_id,
        std::size_t          _byte_size,
        std::string_view     _name,
        GpuCompletionFuture  _completion
    );

    using PayloadWriter = void (*)(void*, std::span<byte>);

    // Invokes the trusted backend writer synchronously while the payload is
    // exclusively Pending, then atomically publishes an immutable snapshot.
    // The writable span is valid only for the duration of _writer.
    static bool MaterializePayload(
        const ReadbackToken& _token,
        void*                _context,
        PayloadWriter        _writer
    ) noexcept;
    static bool PublishPayloadErrorIfPending(
        const ReadbackToken& _token,
        std::string_view     _reason
    ) noexcept;
};

} // namespace Moer::Render

#endif
