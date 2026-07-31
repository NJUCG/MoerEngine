#ifndef MOERENGINE_ASSERT_H
#define MOERENGINE_ASSERT_H

#include "API_Macro.h"
#include "platform/Platform.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

namespace Moer::Diagnostics {

enum class EFailureKind : std::uint8_t {
    Assert,
    Ensure,
};

inline constexpr std::size_t kFailureMessageCapacity = 1024;

struct FailureMessage {
    std::array<char, kFailureMessageCapacity> bytes{};
    std::uint32_t                             size{0};
    bool                                      truncated{false};

    [[nodiscard]] std::string_view View() const noexcept {
        return std::string_view(bytes.data(), std::min<std::size_t>(size, bytes.size() - 1));
    }
};

struct FailureInfo {
    EFailureKind   kind{EFailureKind::Assert};
    const char*    expression{nullptr};
    const char*    file{nullptr};
    const char*    function{nullptr};
    std::uint32_t  line{0};
    std::uint32_t  thread_id{0};
    FailureMessage message{};
};

CORE_API bool HasEnsureFailures() noexcept;
CORE_API void ResetEnsureFailures() noexcept;

[[noreturn]] CORE_API void HandleAssertFailure(const FailureInfo& _info) noexcept;
CORE_API bool              HandleEnsureFailure(const FailureInfo& _info) noexcept;

namespace Detail {

CORE_API void ClaimAssertFailureOwnership(std::uint32_t _thread_id) noexcept;

inline FailureMessage FormattingFailureMessage() noexcept {
    constexpr std::string_view fallback = "failure message formatting failed";
    FailureMessage             message{};
    message.size = static_cast<std::uint32_t>(std::min(fallback.size(), message.bytes.size() - 1));
    std::copy_n(fallback.data(), message.size, message.bytes.data());
    message.bytes[message.size] = '\0';
    return message;
}

template<typename... Args>
FailureMessage FormatFailureMessage(std::format_string<Args...> _format, Args&&... _args) noexcept {
    FailureMessage message{};
    try {
        const auto formatted = std::format_to_n(
            message.bytes.data(), message.bytes.size() - 1, _format, std::forward<Args>(_args)...
        );
        const std::size_t formatted_size = static_cast<std::size_t>(formatted.size);
        const std::size_t stored_size    = std::min(formatted_size, message.bytes.size() - 1);
        message.size                     = static_cast<std::uint32_t>(stored_size);
        message.truncated                = formatted_size > stored_size;
        message.bytes[stored_size]       = '\0';
        return message;
    } catch (...) {
        return FormattingFailureMessage();
    }
}

} // namespace Detail

template<typename... Args>
[[noreturn]] inline void ReportAssertFailure(
    const char*                 _expression,
    const char*                 _file,
    std::uint32_t               _line,
    const char*                 _function,
    std::format_string<Args...> _format,
    Args&&... _args
) noexcept {
    const std::uint32_t thread_id = Platform::GetCurrentThreadID();
    Detail::ClaimAssertFailureOwnership(thread_id);
    FailureInfo info{
        .kind       = EFailureKind::Assert,
        .expression = _expression,
        .file       = _file,
        .function   = _function,
        .line       = _line,
        .thread_id  = thread_id,
        .message    = Detail::FormatFailureMessage(_format, std::forward<Args>(_args)...),
    };
    HandleAssertFailure(info);
}

template<typename... Args>
inline bool ReportEnsureFailure(
    const char*                 _expression,
    const char*                 _file,
    std::uint32_t               _line,
    const char*                 _function,
    std::format_string<Args...> _format,
    Args&&... _args
) noexcept {
    FailureInfo info{
        .kind       = EFailureKind::Ensure,
        .expression = _expression,
        .file       = _file,
        .function   = _function,
        .line       = _line,
        .thread_id  = Platform::GetCurrentThreadID(),
        .message    = Detail::FormatFailureMessage(_format, std::forward<Args>(_args)...),
    };
    return HandleEnsureFailure(info);
}

} // namespace Moer::Diagnostics

// Always-on engine invariant. Unlike the CRT assert macro, this remains active
// in optimized builds so thread/resource ownership violations keep the same
// controlled-fatal diagnostics contract in Debug and Release.
#define MOER_ASSERT(expr, format_str, ...)            \
    do {                                              \
        if (!(expr)) {                                \
            ::Moer::Diagnostics::ReportAssertFailure( \
                #expr,                                \
                __FILE__,                             \
                static_cast<std::uint32_t>(__LINE__), \
                __func__,                             \
                format_str __VA_OPT__(, ) __VA_ARGS__ \
            );                                        \
        }                                             \
    } while (false)

#define MOER_ENSURE(expr, format_str, ...)              \
    ((expr) ? true :                                    \
              ::Moer::Diagnostics::ReportEnsureFailure( \
                  #expr,                                \
                  __FILE__,                             \
                  static_cast<std::uint32_t>(__LINE__), \
                  __func__,                             \
                  format_str __VA_OPT__(, ) __VA_ARGS__ \
              ))

#endif // MOERENGINE_ASSERT_H
