#include "platform/Platform.h"
#include "PlatformImplement.h"
#if PLATFORM_WINDOWS
#include "Windows/PlatformWindows.h"
#elif PLATFORM_LINUX
#include "PlatformLinux.h"
#endif

PlatformImplement* Platform::GetInstance() {
#if PLATFORM_WINDOWS
    static WindowsPlatform platform;
#elif PLATFORM_LINUX
    static LinuxPlatform platform;
#endif

    return &platform;
}

void Platform::SetThreadAffinity(void* current_thread_handle, uint64_t mask) {

    GetInstance()->SetThreadAffinityMask(current_thread_handle, mask);
}
void Platform::SetThreadGroupAffinity(void* current_thread_handle, uint16_t group_mask, uint64_t affinity_mask) {
    GetInstance()->SetThreadGroupAffinity(current_thread_handle, group_mask, affinity_mask);
}

int32_t Platform::GetProcessorWorkGroupCount() {
    return GetInstance()->GetProcessorWorkGroupCount();
}

int32_t Platform::GetProcessorCoreCountInGroup(uint32_t groupID) {
    return GetInstance()->GetProcessorCoreCountInGroup(groupID);
}

int32_t Platform::GetProcessorCoreCount() {

    return GetInstance()->GetProcessorCoreCount();
}

uint32_t Platform::GetCurrentThreadID() {
    return GetInstance()->GetCurrentThreadID();
}
