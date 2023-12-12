#ifndef PLATFORM_H
#define PLATFORM_H
#include "API_Macro.h"
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
class Platform {

protected:
public:
    CORE_API static void     SetThreadAffinity(void* current_thread_handle, uint64_t mask);
    CORE_API static void     SetThreadGroupAffinity(void* current_thread_handle, uint16_t group_mask, uint64_t affinity_mask);
    CORE_API static int32_t  GetProcessorWorkGroupCount();
    CORE_API static int32_t  GetProcessorCoreCountInGroup(uint32_t groupID);
    CORE_API static int32_t  GetProcessorCoreCount();
    CORE_API static uint32_t GetCurrentThreadID();

    CORE_API static const PlatformMemoryInfo& GetMemoryInfo();
};
#endif// !PLATFORM_H
