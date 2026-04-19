#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Moer::Render {

enum class ERHITraceLevel : int {
    Off     = 0,
    Basic   = 1,
    Verbose = 2
};

inline std::string ReadRHITraceEnvValue(const char* name) {
#if defined(_WIN32)
    char*  buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr || length == 0) {
        return {};
    }
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return {};
    }
    return value;
#endif
}

inline int ParseRHITraceEnvInt(const char* name, int default_value) {
    const std::string value = ReadRHITraceEnvValue(name);
    if (value.empty()) {
        return default_value;
    }
    char* end_ptr    = nullptr;
    long  parsed_val = std::strtol(value.c_str(), &end_ptr, 10);
    if (end_ptr == value.c_str()) {
        return default_value;
    }
    return static_cast<int>(parsed_val);
}

inline uint64_t ParseRHITraceEnvUInt64(const char* name, uint64_t default_value) {
    const std::string value = ReadRHITraceEnvValue(name);
    if (value.empty()) {
        return default_value;
    }
    char* end_ptr = nullptr;
    auto  parsed  = std::strtoull(value.c_str(), &end_ptr, 10);
    if (end_ptr == value.c_str()) {
        return default_value;
    }
    return static_cast<uint64_t>(parsed);
}

inline uint64_t GetConfiguredRHITraceFrame() {
    static uint64_t frame =
        ParseRHITraceEnvUInt64("MOER_RHI_TRACE_FRAME", std::numeric_limits<uint64_t>::max());
    return frame;
}

inline uint64_t& CurrentRHITraceFrameStorage() {
    static thread_local uint64_t current_frame = std::numeric_limits<uint64_t>::max();
    return current_frame;
}

inline std::atomic<uint64_t>& RHITraceFrameCounterStorage() {
    static std::atomic<uint64_t> next_frame{0};
    return next_frame;
}

inline std::atomic<bool>& RHITraceRuntimeEnabledStorage() {
    static std::atomic<bool> enabled{true};
    return enabled;
}

inline uint64_t GetCurrentRHITraceFrame() {
    return CurrentRHITraceFrameStorage();
}

inline void SetCurrentRHITraceFrame(uint64_t frame) {
    CurrentRHITraceFrameStorage() = frame;
}

class ScopedRHITraceFrame {
public:
    explicit ScopedRHITraceFrame(uint64_t frame) : previous_frame(GetCurrentRHITraceFrame()) {
        SetCurrentRHITraceFrame(frame);
    }

    ~ScopedRHITraceFrame() {
        SetCurrentRHITraceFrame(previous_frame);
    }

private:
    uint64_t previous_frame;
};

inline uint64_t NextRHITraceFrameIndex() {
    return RHITraceFrameCounterStorage().fetch_add(1, std::memory_order_relaxed);
}

inline void ResetRHITraceFrameIndex(uint64_t frame = 0) {
    RHITraceFrameCounterStorage().store(frame, std::memory_order_relaxed);
}

inline void SetRHITraceRuntimeEnabled(bool enabled) {
    RHITraceRuntimeEnabledStorage().store(enabled, std::memory_order_relaxed);
}

inline bool IsRHITraceRuntimeEnabled() {
    return RHITraceRuntimeEnabledStorage().load(std::memory_order_relaxed);
}

inline bool IsRHITraceFrameActive() {
    const uint64_t configured_frame = GetConfiguredRHITraceFrame();
    if (configured_frame == std::numeric_limits<uint64_t>::max()) {
        return true;
    }
    return GetCurrentRHITraceFrame() == configured_frame;
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
    return IsRHITraceRuntimeEnabled() &&
           IsRHITraceFrameActive() &&
           static_cast<int>(GetRHITraceLevel()) >= static_cast<int>(level);
}

inline bool IsRHIBarrierTraceEnabled(ERHITraceLevel level = ERHITraceLevel::Basic) {
    return IsRHITraceRuntimeEnabled() &&
           IsRHITraceFrameActive() &&
           static_cast<int>(GetRHIBarrierTraceLevel()) >= static_cast<int>(level);
}

// --- Resource-specific transition tracing ---
// Set MOER_RHI_TRACE_RESOURCES=combine_output,local_light_pdf_tex to trace specific resources.
inline const std::vector<std::string>& GetRHITraceResourceNames() {
    static std::vector<std::string> names = []() {
        std::vector<std::string> result;
        std::string s = ReadRHITraceEnvValue("MOER_RHI_TRACE_RESOURCES");
        if (!s.empty()) {
            size_t pos = 0;
            while ((pos = s.find(',')) != std::string::npos) {
                auto token = s.substr(0, pos);
                if (!token.empty()) result.push_back(token);
                s.erase(0, pos + 1);
            }
            if (!s.empty()) result.push_back(s);
        }
        return result;
    }();
    // return names;
    static std::vector<std::string> default_names = []() {
         std::vector<std::string> result;
         result.push_back("combine_output");
         result.push_back("local_light_pdf_tex");
         return result;
    }();
    return default_names;
}

inline bool IsRHIResourceTraced(std::string_view name) {
    if (name.empty()) return false;
    const auto& names = GetRHITraceResourceNames();
    if (names.empty()) return false;
    for (const auto& n : names) {
        if (name == n) return true;
    }
    return false;
}

inline bool IsRHIResourceTraceEnabled() {
    return !GetRHITraceResourceNames().empty();
}

} // namespace Moer::Render

#define RHITRACE_RESOURCE 0

// RHI trace macros:
//   RHITRACE_LOG(off|basic|verbose, ...)
//   RHITRACE_BARRIER_LOG(off|basic|verbose, ...)
//   RHITRACE_ENABLED(off|basic|verbose)
//   RHITRACE_BARRIER_ENABLED(off|basic|verbose)
#if defined(_DEBUG) || defined(DEBUG)
#define RHITRACE_COMPILETIME_ENABLED 1
#define RHITRACE_RESOURCE_ENABLED RHITRACE_RESOURCE
#else
#define RHITRACE_COMPILETIME_ENABLED 0
#define RHITRACE_RESOURCE_ENABLED 0
#endif

#if RHITRACE_COMPILETIME_ENABLED
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

#else
#define RHITRACE_ENABLED_off() (false)
#define RHITRACE_ENABLED_basic() (false)
#define RHITRACE_ENABLED_verbose() (false)
#define RHITRACE_ENABLED(level) (false)

#define RHITRACE_BARRIER_ENABLED_off() (false)
#define RHITRACE_BARRIER_ENABLED_basic() (false)
#define RHITRACE_BARRIER_ENABLED_verbose() (false)
#define RHITRACE_BARRIER_ENABLED(level) (false)

#define RHITRACE_LOG_off(...)
#define RHITRACE_LOG_basic(...)
#define RHITRACE_LOG_verbose(...)
#define RHITRACE_LOG(level, ...)

#define RHITRACE_BARRIER_LOG_off(...)
#define RHITRACE_BARRIER_LOG_basic(...)
#define RHITRACE_BARRIER_LOG_verbose(...)
#define RHITRACE_BARRIER_LOG(level, ...)

#endif


#if RHITRACE_RESOURCE_ENABLED

// Resource-specific transition trace macros:
//   RHITRACE_RESOURCE_LOG(name, ...)  — log only if `name` is in the traced resource list
//   Short-circuits: GetName() and format args are NOT evaluated when tracing is off.
#define RHITRACE_RESOURCE_LOG(name, ...) \
    do { \
        if (::Moer::Render::IsRHIResourceTraced(name)) { \
            LOG_INFO(__VA_ARGS__); \
        } \
    } while (0)

#else

#define RHITRACE_RESOURCE_LOG(name, ...)
#endif