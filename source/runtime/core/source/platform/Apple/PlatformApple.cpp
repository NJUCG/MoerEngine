#include "PlatformApple.h"

#include <algorithm>
#include <cstdlib>
#include <pthread.h>
#include <string>
#include <sys/sysctl.h>
#include <thread>
#include <unistd.h>

void ApplePlatform::SetThreadAffinityMask(void*, uint64_t) {
    // macOS has no equivalent of the Windows hard processor affinity mask.
}

void ApplePlatform::SetCurrentThreadAffinity(Affinity&&) {}

void ApplePlatform::SetCurrentThreadName(std::string_view name) {
    const std::string truncated(name.substr(0, 63));
    pthread_setname_np(truncated.c_str());
}

void ApplePlatform::SetThreadGroupAffinity(void*, uint16_t, uint64_t) {}

int32_t ApplePlatform::GetProcessorWorkGroupCount() {
    return 1;
}

int32_t ApplePlatform::GetProcessorCoreCountInGroup(uint32_t group_id) {
    return group_id == 0 ? GetProcessorCoreCount() : 0;
}

int32_t ApplePlatform::GetProcessorCoreCount() {
    return static_cast<int32_t>(std::max(1u, std::thread::hardware_concurrency()));
}

uint32_t ApplePlatform::GetCurrentThreadID() {
    uint64_t thread_id = 0;
    pthread_threadid_np(nullptr, &thread_id);
    return static_cast<uint32_t>(thread_id);
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
        result.page_size = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
        result.allocation_granularity = result.page_size;
        result.total_physical_memory_mb =
            static_cast<uint32_t>((result.total_physical_memory + (1ull << 20) - 1) >> 20);
        return result;
    }();
    return info;
}
