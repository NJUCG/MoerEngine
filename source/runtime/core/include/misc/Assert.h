#ifndef MOERENGINE_ASSERT_H
#define MOERENGINE_ASSERT_H

#include "API_Macro.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace Moer::Diagnostics {

enum class EFailureKind : uint8_t {
    Assert,
    Ensure,
};

struct FailureInfo {
    EFailureKind kind = EFailureKind::Assert;
    const char*  expression = nullptr;
    const char*  file = nullptr;
    const char*  function = nullptr;
    uint32_t     line = 0;
    uint32_t     thread_id = 0;
    std::string  message;
};

using CrashFlushHook = void (*)();

CORE_API void SetCrashFlushHook(CrashFlushHook hook);
CORE_API void SetEnsureFailureEscalation(bool enabled);
CORE_API bool HasEnsureFailures();
CORE_API void ResetEnsureFailures();

[[noreturn]] CORE_API void HandleAssertFailure(FailureInfo&& info) noexcept;
CORE_API bool HandleEnsureFailure(FailureInfo&& info) noexcept;

template<typename... Args>
[[noreturn]] inline void ReportAssertFailure(
    const char*      expression,
    const char*      file,
    uint32_t         line,
    const char*      function,
    uint32_t         thread_id,
    std::string_view format_str,
    Args&&...        args
) {
    FailureInfo info{};
    info.kind = EFailureKind::Assert;
    info.expression = expression;
    info.file = file;
    info.function = function;
    info.line = line;
    info.thread_id = thread_id;
    info.message = std::vformat(format_str, std::make_format_args(args...));
    HandleAssertFailure(std::move(info));
}

template<typename... Args>
inline bool ReportEnsureFailure(
    const char*      expression,
    const char*      file,
    uint32_t         line,
    const char*      function,
    uint32_t         thread_id,
    std::string_view format_str,
    Args&&...        args
) {
    FailureInfo info{};
    info.kind = EFailureKind::Ensure;
    info.expression = expression;
    info.file = file;
    info.function = function;
    info.line = line;
    info.thread_id = thread_id;
    info.message = std::vformat(format_str, std::make_format_args(args...));
    return HandleEnsureFailure(std::move(info));
}

} // namespace Moer::Diagnostics

#define MOER_ASSERT(expr, format_str, ...)                                                          \
    do {                                                                                            \
        if (!(expr)) {                                                                              \
            ::Moer::Diagnostics::ReportAssertFailure(                                               \
                #expr,                                                                              \
                __FILE__,                                                                           \
                static_cast<uint32_t>(__LINE__),                                                    \
                __func__,                                                                           \
                ::Platform::GetCurrentThreadID(),                                                   \
                format_str                                                                          \
                __VA_OPT__(, ) __VA_ARGS__                                                          \
            );                                                                                      \
        }                                                                                           \
    } while (0)

#define MOER_ENSURE(expr, format_str, ...)                                                          \
    ((expr) ? true                                                                                  \
            : ::Moer::Diagnostics::ReportEnsureFailure(                                             \
                  #expr,                                                                            \
                  __FILE__,                                                                         \
                  static_cast<uint32_t>(__LINE__),                                                  \
                  __func__,                                                                         \
                  ::Platform::GetCurrentThreadID(),                                                 \
                  format_str                                                                        \
                  __VA_OPT__(, ) __VA_ARGS__))

#endif // MOERENGINE_ASSERT_H