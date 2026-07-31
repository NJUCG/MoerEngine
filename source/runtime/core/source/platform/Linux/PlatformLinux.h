#ifndef LINUX_PLATFORM_H
#define LINUX_PLATFORM_H

#include "../PlatformImplement.h"
#include "platform/Platform.h"
class LinuxPlatform : public PlatformImplement {
    virtual void SetThreadAffinityMask(void* current_thread_handle, uint64_t mask) override;
    virtual void
    SetThreadGroupAffinity(void* current_thread_handle, uint16_t group_mask, uint64_t affinity_mask) override;
    virtual int32_t GetProcessorWorkGroupCount() override;
    virtual int32_t GetProcessorCoreCountInGroup(uint32_t groupID) override;
    virtual int32_t GetProcessorCoreCount() override;
    uint32_t        GetCurrentThreadID() override;

    virtual const PlatformMemoryInfo& GetMemoryInfo() override;
};
#endif
