#include "PlatformApple.h"
#include "misc/Types.h"
#include <algorithm>
#include <cstdlib>
#include <pthread.h>
#include <string>
#include <sys/sysctl.h>
#include <thread>
#include <unistd.h>

void ApplePlatform::SetThreadAffinityMask(void*, Moer::uint64) {
    // macOS has no equivalent of the Windows hard processor affinity mask.
}

void ApplePlatform::SetCurrentThreadAffinity(Affinity&&) {}

void ApplePlatform::SetCurrentThreadName(std::string_view name) {
    const std::string truncated(name.substr(0, 63));
    pthread_setname_np(truncated.c_str());
}

void ApplePlatform::SetThreadGroupAffinity(void*, Moer::uint16, Moer::uint64) {}

Moer::int32 ApplePlatform::GetProcessorWorkGroupCount() {
    return 1;
}

Moer::int32 ApplePlatform::GetProcessorCoreCountInGroup(Moer::uint32 group_id) {
    return group_id == 0 ? GetProcessorCoreCount() : 0;
}

Moer::int32 ApplePlatform::GetProcessorCoreCount() {
    return static_cast<Moer::int32>(std::max(1u, std::thread::hardware_concurrency()));
}

Moer::uint32 ApplePlatform::GetCurrentThreadID() {
    Moer::uint64 thread_id = 0;
    pthread_threadid_np(nullptr, &thread_id);
    return static_cast<Moer::uint32>(thread_id);
}

void ApplePlatform::SetEnv(const char* name, const char* value) {
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
}

const PlatformMemoryInfo& ApplePlatform::GetMemoryInfo() {
    static const PlatformMemoryInfo info = [] {
        PlatformMemoryInfo result{};
        size_t             value_size = sizeof(result.total_physical_memory);
        sysctlbyname("hw.memsize", &result.total_physical_memory, &value_size, nullptr, 0);
        result.page_size              = static_cast<Moer::uint64>(sysconf(_SC_PAGESIZE));
        result.allocation_granularity = result.page_size;
        result.total_physical_memory_mb =
            static_cast<Moer::uint32>((result.total_physical_memory + (1ull << 20) - 1) >> 20);
        return result;
    }();
    return info;
}
