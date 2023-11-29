#include "platform/Platform.h"
#include "PlatformImplement.h"
#if PLATFORM_WINDOWS
#include "Windows/PlatformWindows.h"
#elif PLATFORM_LINUX
#include "PlatformLinux.h"
#endif

PlatformImplement* PlatformImplement::GetInstance() {
#if PLATFORM_WINDOWS
    static WindowsPlatform platform;
#elif PLATFORM_LINUX
    static LinuxPlatform platform;
#endif

    return &platform;
}

void Platform::SetThreadAffinity(void* current_thread_handle, uint64_t mask) {

    PlatformImplement::GetInstance()->SetThreadAffinityMask(current_thread_handle, mask);
}
void Platform::SetThreadGroupAffinity(void* current_thread_handle, uint16_t group_mask, uint64_t affinity_mask) {
    PlatformImplement::GetInstance()->SetThreadGroupAffinity(current_thread_handle, group_mask, affinity_mask);
}

int32_t Platform::GetProcessorWorkGroupCount() {
    return PlatformImplement::GetInstance()->GetProcessorWorkGroupCount();
}

int32_t Platform::GetProcessorCoreCountInGroup(uint32_t groupID) {
    return PlatformImplement::GetInstance()->GetProcessorCoreCountInGroup(groupID);
}

int32_t Platform::GetProcessorCoreCount() {

    return PlatformImplement::GetInstance()->GetProcessorCoreCount();
}

uint32_t Platform::GetCurrentThreadID() {
    return PlatformImplement::GetInstance()->GetCurrentThreadID();
}
