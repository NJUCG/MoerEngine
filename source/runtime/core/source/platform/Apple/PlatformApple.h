#pragma once

#include "../PlatformImplement.h"
#include "misc/Types.h"

class ApplePlatform final : public PlatformImplement {
public:
    void                      SetThreadAffinityMask(void* current_thread_handle, Moer::uint64 mask) override;
    void SetCurrentThreadAffinity(Affinity&& affinity) override;
    void SetCurrentThreadName(std::string_view name) override;
    void                      SetThreadGroupAffinity(void* current_thread_handle, Moer::uint16 group_mask, Moer::uint64 affinity_mask) override;
    Moer::int32               GetProcessorWorkGroupCount() override;
    Moer::int32               GetProcessorCoreCountInGroup(Moer::uint32 group_id) override;
    Moer::int32               GetProcessorCoreCount() override;
    Moer::uint32              GetCurrentThreadID() override;
    void SetEnv(const char* name, const char* value) override;
    const PlatformMemoryInfo& GetMemoryInfo() override;
};
