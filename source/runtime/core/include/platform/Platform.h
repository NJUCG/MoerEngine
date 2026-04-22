#ifndef PLATFORM_H
#define PLATFORM_H
#include "API_Macro.h"
#include "misc/STL.h"
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <variant>
#if defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || \
    defined(_WIN64_)

#define PLATFORM_WINDOWS 1
#elif defined(ANDROID) || defined(_ANDROID_)
#define PLATFORM_ANDROID 1
#elif defined(__linux__)
#define PLATFORM_LINUX 1
#elif defined(__APPLE__) || defined(TARGET_OS_IPHONE) || defined(TARGET_IPHONE_SIMULATOR) || \
    defined(TARGET_OS_MAC)
#define PLATFORM_APPLE 1
#else
#define PLATFORM_UNKNOWN 1
#endif
#define PLATFORM_CACHELINE_SIZE 64
struct PlatformMemoryInfo {
    uint64_t total_physical_memory = 0;
    uint64_t total_virtual_memory  = 0;

    uint64_t page_size              = 0;
    uint64_t allocation_granularity = 0;

    uint64_t addrress_limit = 0xffffffffffffffff;

    // MB
    uint32_t total_physical_memory_mb = 0;
};

struct PlatformStackTrace {
    Moer::Array<uintptr_t> frames;
};

struct PlatformCrashDumpRequest {
    std::string failure_kind;
    std::string message;
    uint32_t    thread_id = 0;
};

struct PlatformCrashDumpResult {
    bool                  written = false;
    std::filesystem::path path;
    std::string           error_message;
};

struct Core {
    struct Windows {
        uint8_t group;
        uint8_t idx;
    };
    struct PThread {
        uint16_t idx;
    };
    union {
        Windows windows;
        PThread pthread;
    };

    inline bool operator==(Core _other) const {
        return pthread.idx == _other.pthread.idx;
    }
    inline bool operator<(Core _other) const {
        return pthread.idx < _other.pthread.idx;
    }
};

struct CORE_API Affinity {
#if defined(PLATFORM_WINDOWS) || defined(PLATFORM_LINUX)
    static constexpr bool supported = true;
#else
    static constexpr bool supported = false;
#endif
    inline Core operator[](uint32_t _idx) const {
        return cores[_idx];
    }
    inline size_t GetSize() const {
        return cores.size();
    }
    Affinity(std::initializer_list<Core>);
    Affinity() = default;
    Affinity(Affinity&&) noexcept;
    Affinity& operator=(Affinity&&) noexcept;
    Affinity(const Affinity&) = default;
    Moer::Array<Core> cores;
    ~Affinity() = default;
    static Affinity All();
    static Affinity AnyOf(uint32_t _thread_id, Affinity&& _in_affinity);
};
class Platform {

protected:
public:
    CORE_API static void SetThreadAffinity(void* current_thread_handle, uint64_t mask);
    CORE_API static void SetCurrentThreadAffinity(Affinity&& _affinity);
    CORE_API static void SetCurrentThreadName(std::string_view _name);
    CORE_API static void
    SetThreadGroupAffinity(void* current_thread_handle, uint16_t group_mask, uint64_t affinity_mask);
    CORE_API static int32_t  GetProcessorWorkGroupCount();
    CORE_API static int32_t  GetProcessorCoreCountInGroup(uint32_t groupID);
    CORE_API static int32_t  GetProcessorCoreCount();
    CORE_API static uint32_t GetCurrentThreadID();
    CORE_API static void     SetEnv(const char* _name, const char* _value);

    CORE_API static PlatformStackTrace     CaptureStackTrace(uint32_t frames_to_skip = 0, uint32_t max_frames = 64);
    CORE_API static std::string            FormatStackTrace(const PlatformStackTrace& trace);
    CORE_API static PlatformCrashDumpResult WriteCrashDump(const PlatformCrashDumpRequest& request);
    [[noreturn]] CORE_API static void      FailFast(std::string_view reason);

    CORE_API static const PlatformMemoryInfo& GetMemoryInfo();
};
#endif // !PLATFORM_H
