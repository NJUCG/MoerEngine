#ifndef PLATFORM_H
#define PLATFORM_H
#include <cstdint>
#if defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)

#define PLATFORM_WINDOWS 1
#elif defined(ANDROID) || defined(_ANDROID_)
#define PLATFORM_ANDROID 1
#elif defined(__linux__)
#define PLATFORM_LINUX 1
#elif defined(__APPLE__) || defined(TARGET_OS_IPHONE) || defined(TARGET_IPHONE_SIMULATOR) || defined(TARGET_OS_MAC)
#define PLATFORM_IOS 1
#else
#define PLATFORM_UNKNOWN 1
#endif

class Platform {
protected:
    static class PlatformImplement* GetInstance();

public:
    static void     SetThreadAffinity(void* current_thread_handle, uint64_t mask);
    static void     SetThreadGroupAffinity(void* current_thread_handle, uint16_t group_mask, uint64_t affinity_mask);
    static int32_t  GetProcessorWorkGroupCount();
    static int32_t  GetProcessorCoreCountInGroup(uint32_t groupID);
    static int32_t  GetProcessorCoreCount();
    static uint32_t GetCurrentThreadID();
};
#endif// !PLATFORM_H
