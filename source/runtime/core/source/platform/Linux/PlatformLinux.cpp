#include "PlatformLinux.h"

#include <cstdlib>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

cpu_set_t BuildCpuSet(uint64_t mask) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (uint32_t bit = 0; bit < 64; ++bit) {
        if ((mask & (1ull << bit)) != 0) {
            CPU_SET(bit, &cpu_set);
        }
    }
    return cpu_set;
}

cpu_set_t BuildCpuSet(const Affinity& affinity) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (const Core& core : affinity.cores) {
        CPU_SET(core.pthread.idx, &cpu_set);
    }
    return cpu_set;
}

pthread_t ToPThread(void* current_thread_handle) {
    return static_cast<pthread_t>(reinterpret_cast<uintptr_t>(current_thread_handle));
}

} // namespace

void LinuxPlatform::SetCurrentThreadAffinity(Affinity&& _affinity) {
    if (_affinity.GetSize() == 0) {
        return;
    }

    cpu_set_t cpu_set = BuildCpuSet(_affinity);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
}

void LinuxPlatform::SetCurrentThreadName(std::string_view _name) {
    // pthread_setname_np accepts at most 16 bytes including the trailing null terminator.
    std::string thread_name(_name.substr(0, 15));
    pthread_setname_np(pthread_self(), thread_name.c_str());
}

void LinuxPlatform::SetThreadAffinityMask(void* current_thread_handle, uint64_t mask) {
    cpu_set_t cpu_set = BuildCpuSet(mask);
    pthread_setaffinity_np(ToPThread(current_thread_handle), sizeof(cpu_set), &cpu_set);
}

void LinuxPlatform::SetThreadGroupAffinity(
    void*    current_thread_handle,
    uint16_t group_mask,
    uint64_t affinity_mask
) {
    (void)group_mask;
    SetThreadAffinityMask(current_thread_handle, affinity_mask);
}

int32_t LinuxPlatform::GetProcessorWorkGroupCount() {
    return 1;
}

int32_t LinuxPlatform::GetProcessorCoreCountInGroup(uint32_t groupID) {
    if (groupID != 0) {
        return 0;
    }

    return GetProcessorCoreCount();
}

int32_t LinuxPlatform::GetProcessorCoreCount() {
    return static_cast<int32_t>(sysconf(_SC_NPROCESSORS_ONLN));
}

uint32_t LinuxPlatform::GetCurrentThreadID() {
    return static_cast<uint32_t>(syscall(SYS_gettid));
}

void LinuxPlatform::SetEnv(const char* _name, const char* _value) {
    setenv(_name, _value, 1);
}

const PlatformMemoryInfo& LinuxPlatform::GetMemoryInfo() {
    static PlatformMemoryInfo memory_info = [] {
        constexpr uint64_t kBytesPerMb = 1024ull * 1024ull;

        PlatformMemoryInfo info;
        const auto page_size = static_cast<uint64_t>(sysconf(_SC_PAGE_SIZE));
        const auto total_pages = static_cast<uint64_t>(sysconf(_SC_PHYS_PAGES));

        info.page_size = page_size;
        info.allocation_granularity = page_size;
        info.total_physical_memory = total_pages * page_size;
        info.total_virtual_memory = info.total_physical_memory;
        info.total_physical_memory_mb = static_cast<uint32_t>(
            (info.total_physical_memory + kBytesPerMb - 1) / kBytesPerMb
        );
        return info;
    }();

    return memory_info;
}
