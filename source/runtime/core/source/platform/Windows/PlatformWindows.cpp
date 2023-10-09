#include "PlatformWindows.h"
#include "Windows.h"

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
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return sysInfo.dwNumberOfProcessors;
}

uint32_t WindowsPlatform::GetCurrentThreadID() {
    return ::GetCurrentThreadId();
}
