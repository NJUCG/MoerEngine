#include "PlatformWindows.h"
#include "Windows.h"
#include "math/Math.h"

void WindowsPlatform::SetThreadAffinityMask(void* current_thread_handle, uint64_t mask) {

    ::SetThreadAffinityMask(current_thread_handle, mask);
}
void WindowsPlatform::SetThreadGroupAffinity(void* current_thread_handle, uint16_t group_mask, uint64_t affinity_mask) {
    GROUP_AFFINITY group_affinity{affinity_mask, group_mask, {0, 0, 0}};
    ::SetThreadGroupAffinity(current_thread_handle, &group_affinity, nullptr);
}

int32_t WindowsPlatform::GetProcessorWorkGroupCount() {
    return ::GetActiveProcessorGroupCount();
}

int32_t WindowsPlatform::GetProcessorCoreCountInGroup(uint32_t groupID) {
    return ::GetActiveProcessorCount(groupID);
}

int32_t WindowsPlatform::GetProcessorCoreCount() {
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    return sys_info.dwNumberOfProcessors;
}

uint32_t WindowsPlatform::GetCurrentThreadID() {
    return ::GetCurrentThreadId();
}

const PlatformMemoryInfo& WindowsPlatform::GetMemoryInfo() {
    static PlatformMemoryInfo memory_info;

    if (memory_info.total_physical_memory == 0) {
        MEMORYSTATUSEX statex{};
        statex.dwLength = sizeof(statex);
        GlobalMemoryStatusEx(&statex);

        SYSTEM_INFO sys_info{};
        GetSystemInfo(&sys_info);

        memory_info.total_physical_memory = statex.ullTotalPhys;
        memory_info.total_virtual_memory  = statex.ullTotalVirtual;

        memory_info.page_size              = sys_info.dwPageSize;
        memory_info.allocation_granularity = sys_info.dwAllocationGranularity;

        memory_info.total_physical_memory_mb = static_cast<uint32_t>((memory_info.total_physical_memory + 1024 * 1024 - 1) / 1024 / 1024);
        //caclulate address limit by physical memory
        memory_info.addrress_limit = Moer::RoundUpToPowerOf2(memory_info.total_physical_memory);
    }
    return memory_info;
}
