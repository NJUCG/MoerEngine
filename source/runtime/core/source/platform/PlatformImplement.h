#ifndef PLATFORM_IMPLEMENT_H
#define PLATFORM_IMPLEMENT_H

#include "misc/Types.h"
#include "platform/Platform.h"

class PlatformImplement : public Platform {
public:
    static PlatformImplement* GetInstance();
    virtual void              SetThreadAffinityMask(void* current_thread_handle, Moer::uint64 mask) = 0;
    virtual void              SetCurrentThreadAffinity(Affinity&& _affinity)                    = 0;
    virtual void              SetCurrentThreadName(std::string_view _name)                      = 0;
    virtual void
                         SetThreadGroupAffinity(void* current_thread_handle, Moer::uint16 group_mask, Moer::uint64 affinity_mask) = 0;
    virtual Moer::int32  GetProcessorWorkGroupCount()                                                                             = 0;
    virtual Moer::int32  GetProcessorCoreCountInGroup(Moer::uint32 groupID)                                                       = 0;
    virtual Moer::int32  GetProcessorCoreCount()                                                                                  = 0;
    virtual Moer::uint32 GetCurrentThreadID()                                                                                     = 0;
    virtual void     SetEnv(const char* _name, const char* _value)                                   = 0;

    virtual PlatformStackTrace
                 CaptureStackTrace(Moer::uint32 _frames_to_skip, Moer::uint32 _max_frames) noexcept;
    virtual bool InitializeCrashDiagnostics() noexcept;
    virtual PlatformCrashArtifactResult
                              WriteCrashArtifacts(
                                  const PlatformCrashArtifactRequest& _request,
                                  Moer::uint32                        _timeout_ms
                              ) noexcept;
    [[noreturn]] virtual void FailFast(std::string_view _reason) noexcept;

    virtual const PlatformMemoryInfo& GetMemoryInfo() = 0;
};

#endif
