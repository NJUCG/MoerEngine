#ifndef PLATFORM_IMPLEMENT_H
#define PLATFORM_IMPLEMENT_H
#include "platform/Platform.h"

class PlatformImplement : public Platform {
public:
    static PlatformImplement* GetInstance();
    virtual void              SetThreadAffinityMask(void* current_thread_handle, uint64_t mask) = 0;
    virtual void              SetCurrentThreadAffinity(Affinity&& _affinity)                    = 0;
    virtual void              SetCurrentThreadName(std::string_view _name)                      = 0;
    virtual void
    SetThreadGroupAffinity(void* current_thread_handle, uint16_t group_mask, uint64_t affinity_mask) = 0;
    virtual int32_t  GetProcessorWorkGroupCount()                                                    = 0;
    virtual int32_t  GetProcessorCoreCountInGroup(uint32_t groupID)                                  = 0;
    virtual int32_t  GetProcessorCoreCount()                                                         = 0;
    virtual uint32_t GetCurrentThreadID()                                                            = 0;
    virtual void     SetEnv(const char* _name, const char* _value)                                   = 0;

    virtual PlatformStackTrace
    CaptureStackTrace(std::uint32_t _frames_to_skip, std::uint32_t _max_frames) noexcept;
    virtual bool InitializeCrashDiagnostics() noexcept;
    virtual PlatformCrashArtifactResult
    WriteCrashArtifacts(
        const PlatformCrashArtifactRequest& _request,
        std::uint32_t _timeout_ms
    ) noexcept;
    [[noreturn]] virtual void FailFast(std::string_view _reason) noexcept;

    virtual const PlatformMemoryInfo& GetMemoryInfo() = 0;
};

#endif
