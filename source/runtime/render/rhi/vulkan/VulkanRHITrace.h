#pragma once

#include <algorithm>
#include <cstdlib>

namespace Moer::Render {

enum class ERHITraceLevel : int {
    Off     = 0,
    Basic   = 1,
    Verbose = 2
};

inline int ParseRHITraceEnvInt(const char* name, int default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return default_value;
    }
    char* end_ptr    = nullptr;
    long  parsed_val = std::strtol(value, &end_ptr, 10);
    if (end_ptr == value) {
        return default_value;
    }
    return static_cast<int>(parsed_val);
}

inline ERHITraceLevel GetRHITraceLevel() {
    static ERHITraceLevel level = []() {
        int v = ParseRHITraceEnvInt("MOER_RHI_TRACE_LEVEL", -1);
        if (v < 0) {
            v = ParseRHITraceEnvInt("MOER_RHI_TRACE", 0);
        }
        v = std::clamp(v, 0, 2);
        return static_cast<ERHITraceLevel>(v);
    }();
    return level;
}

inline ERHITraceLevel GetRHIBarrierTraceLevel() {
    static ERHITraceLevel level = []() {
        int v = ParseRHITraceEnvInt("MOER_RHI_TRACE_BARRIER_LEVEL", -1);
        if (v < 0) {
            v = ParseRHITraceEnvInt("MOER_RHI_TRACE_BARRIER", -1);
        }
        if (v < 0) {
            v = static_cast<int>(GetRHITraceLevel());
        }
        v = std::clamp(v, 0, 2);
        return static_cast<ERHITraceLevel>(v);
    }();
    return level;
}

inline bool IsRHITraceEnabled(ERHITraceLevel level = ERHITraceLevel::Basic) {
    return static_cast<int>(GetRHITraceLevel()) >= static_cast<int>(level);
}

inline bool IsRHIBarrierTraceEnabled(ERHITraceLevel level = ERHITraceLevel::Basic) {
    return static_cast<int>(GetRHIBarrierTraceLevel()) >= static_cast<int>(level);
}

} // namespace Moer::Render

// RHI trace macros:
//   RHITRACE_LOG(off|basic|verbose, ...)
//   RHITRACE_BARRIER_LOG(off|basic|verbose, ...)
//   RHITRACE_ENABLED(off|basic|verbose)
//   RHITRACE_BARRIER_ENABLED(off|basic|verbose)
#define RHITRACE_ENABLED_off() (false)
#define RHITRACE_ENABLED_basic() (::Moer::Render::IsRHITraceEnabled(::Moer::Render::ERHITraceLevel::Basic))
#define RHITRACE_ENABLED_verbose() (::Moer::Render::IsRHITraceEnabled(::Moer::Render::ERHITraceLevel::Verbose))
#define RHITRACE_ENABLED(level) RHITRACE_ENABLED_##level()

#define RHITRACE_BARRIER_ENABLED_off() (false)
#define RHITRACE_BARRIER_ENABLED_basic() (::Moer::Render::IsRHIBarrierTraceEnabled(::Moer::Render::ERHITraceLevel::Basic))
#define RHITRACE_BARRIER_ENABLED_verbose() (::Moer::Render::IsRHIBarrierTraceEnabled(::Moer::Render::ERHITraceLevel::Verbose))
#define RHITRACE_BARRIER_ENABLED(level) RHITRACE_BARRIER_ENABLED_##level()

#define RHITRACE_LOG_off(...)
#define RHITRACE_LOG_basic(...) do { if (RHITRACE_ENABLED_basic()) { LOG_INFO(__VA_ARGS__); } } while (0)
#define RHITRACE_LOG_verbose(...) do { if (RHITRACE_ENABLED_verbose()) { LOG_INFO(__VA_ARGS__); } } while (0)
#define RHITRACE_LOG(level, ...) RHITRACE_LOG_##level(__VA_ARGS__)

#define RHITRACE_BARRIER_LOG_off(...)
#define RHITRACE_BARRIER_LOG_basic(...) do { if (RHITRACE_BARRIER_ENABLED_basic()) { LOG_INFO(__VA_ARGS__); } } while (0)
#define RHITRACE_BARRIER_LOG_verbose(...) do { if (RHITRACE_BARRIER_ENABLED_verbose()) { LOG_INFO(__VA_ARGS__); } } while (0)
#define RHITRACE_BARRIER_LOG(level, ...) RHITRACE_BARRIER_LOG_##level(__VA_ARGS__)
