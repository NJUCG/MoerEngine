#ifndef PLATFORM_H
#define PLATFORM_H
#include "API_Macro.h"
#include "misc/STL.h"
#include "misc/Types.h"
#include <array>
#include <cstddef>
#include <initializer_list>
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
    Moer::uint64 total_physical_memory = 0;
    Moer::uint64 total_virtual_memory  = 0;

    Moer::uint64 page_size              = 0;
    Moer::uint64 allocation_granularity = 0;

    Moer::uint64 addrress_limit = 0xffffffffffffffff;

    // MB
    Moer::uint32 total_physical_memory_mb = 0;
};

inline constexpr std::size_t kPlatformMaxStackFrames = 64;
inline constexpr std::size_t kPlatformCrashPathCapacity = 1024;

struct PlatformStackTrace {
    std::array<std::uintptr_t, kPlatformMaxStackFrames> frames{};
    Moer::uint32                                        frame_count{0};
};

struct PlatformCrashArtifactRequest {
    std::string_view   failure_kind{};
    std::string_view   expression{};
    std::string_view   file{};
    std::string_view   function{};
    std::string_view   message{};
    std::string_view   profile_flush_status{};
    Moer::uint32       line{0};
    Moer::uint32       thread_id{0};
    bool               message_truncated{false};
    PlatformStackTrace stack{};
};

struct PlatformCrashArtifactResult {
    bool metadata_written{false};
    bool metadata_completed{false};
    bool dump_created{false};
    bool dump_written{false};
    bool dump_flushed{false};
    bool request_completed{false};
    bool timed_out{false};

    Moer::uint32 metadata_error{0};
    Moer::uint32 dump_error{0};

    std::array<char, kPlatformCrashPathCapacity> metadata_path{};
    std::array<char, kPlatformCrashPathCapacity> dump_path{};
};

struct Core {
    struct Windows {
        Moer::uint8 group;
        Moer::uint8 idx;
    };
    struct PThread {
        Moer::uint16 idx;
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
    inline Core operator[](Moer::uint32 _idx) const {
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
    static Affinity AnyOf(Moer::uint32 _thread_id, Affinity&& _in_affinity);
};
class Platform {

protected:
public:
    CORE_API static void SetThreadAffinity(void* current_thread_handle, Moer::uint64 mask);
    CORE_API static void SetCurrentThreadAffinity(Affinity&& _affinity);
    CORE_API static void SetCurrentThreadName(std::string_view _name);
    CORE_API static void
                                 SetThreadGroupAffinity(void* current_thread_handle, Moer::uint16 group_mask, Moer::uint64 affinity_mask);
    CORE_API static Moer::int32  GetProcessorWorkGroupCount();
    CORE_API static Moer::int32  GetProcessorCoreCountInGroup(Moer::uint32 groupID);
    CORE_API static Moer::int32  GetProcessorCoreCount();
    CORE_API static Moer::uint32 GetCurrentThreadID();
    CORE_API static void     SetEnv(const char* _name, const char* _value);

    CORE_API static PlatformStackTrace
    CaptureStackTrace(Moer::uint32 _frames_to_skip = 0, Moer::uint32 _max_frames = 64) noexcept;
    // Startup-only. Prepares the process-lifetime crash worker and its stable
    // output root before any fault path needs them.
    CORE_API static bool InitializeCrashDiagnostics() noexcept;
    // Submits to the prestarted worker and waits at most _timeout_ms. The
    // request describes a controlled fatal snapshot and carries no SEH
    // exception context.
    CORE_API static PlatformCrashArtifactResult
                                      WriteCrashArtifacts(
                                          const PlatformCrashArtifactRequest& _request,
                                          Moer::uint32                        _timeout_ms = 750
                                      ) noexcept;
    [[noreturn]] CORE_API static void FailFast(std::string_view _reason) noexcept;

    CORE_API static const PlatformMemoryInfo& GetMemoryInfo();
};
#endif // !PLATFORM_H
