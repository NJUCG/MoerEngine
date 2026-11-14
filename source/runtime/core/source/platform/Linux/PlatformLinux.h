#ifndef LINUX_PLATFORM_H
#define LINUX_PLATFORM_H

#include "../PlatformImplement.h"
#include "misc/Types.h"
#include "platform/Platform.h"
class LinuxPlatform : public PlatformImplement {
    virtual void SetThreadAffinityMask(void* current_thread_handle, Moer::uint64 mask) override;
    virtual void
                        SetThreadGroupAffinity(void* current_thread_handle, Moer::uint16 group_mask, Moer::uint64 affinity_mask) override;
    virtual Moer::int32 GetProcessorWorkGroupCount() override;
    virtual Moer::int32 GetProcessorCoreCountInGroup(Moer::uint32 groupID) override;
    virtual Moer::int32 GetProcessorCoreCount() override;
    Moer::uint32        GetCurrentThreadID() override;

    virtual const PlatformMemoryInfo& GetMemoryInfo() override;
};
#endif
