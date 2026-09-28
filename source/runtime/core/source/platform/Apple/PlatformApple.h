#pragma once

#include "../PlatformImplement.h"

class ApplePlatform final : public PlatformImplement {
public:
    void SetThreadAffinityMask(void* current_thread_handle, uint64_t mask) override;
    void SetCurrentThreadAffinity(Affinity&& affinity) override;
    void SetCurrentThreadName(std::string_view name) override;
    void SetThreadGroupAffinity(void* current_thread_handle, uint16_t group_mask, uint64_t affinity_mask) override;
    int32_t GetProcessorWorkGroupCount() override;
    int32_t GetProcessorCoreCountInGroup(uint32_t group_id) override;
    int32_t GetProcessorCoreCount() override;
    uint32_t GetCurrentThreadID() override;
    void SetEnv(const char* name, const char* value) override;
    const PlatformMemoryInfo& GetMemoryInfo() override;
};
