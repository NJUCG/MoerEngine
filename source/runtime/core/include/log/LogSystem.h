#ifndef MOERENGINE_LOG_SYSTEM_H
#define MOERENGINE_LOG_SYSTEM_H
#include "spdlog/spdlog.h"
#define LOG_INFO(...)    SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARNING(...) SPDLOG_INFO(__VA_ARGS__)
#define LOG_ERROR(...)   SPDLOG_ERROR(__VA_ARGS__)

namespace Moer {
namespace LogSystem {

    enum class ELogLevel {
        LOG_LEVEL_TRACE,
        LOG_LEVEL_INFO,
        LOG_LEVEL_WARNING,
        LOG_LEVEL_ERROR
    };
    // void LogInfo(...);
    // void LogWarning(...);
    // void LogError(...);
    // template<typename... Args>
    // void LogInfo(Args&&... args) {
    // }

    // template<typename... Args>
    // void log(ELogLevel lvl, format_string_t<Args...> fmt, Args&&... args) {
    //     log(source_loc{}, lvl, fmt, std::forward<Args>(args)...);
    // }

    // template<typename T>
    // void log(level::level_enum lvl, const T& msg) {
    //     log(source_loc{}, lvl, msg);
    // }

    // // T cannot be statically converted to format string (including string_view/wstring_view)
    // template<class T, typename std::enable_if<!is_convertible_to_any_format_string<const T&>::value, int>::type = 0>
    // void log(source_loc loc, level::level_enum lvl, const T& msg) {
    //     log(loc, lvl, "{}", msg);
    // }
}

}// namespace Moer::LogSystem
#endif