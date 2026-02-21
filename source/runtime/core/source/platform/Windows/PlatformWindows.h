#ifndef WINDOWS_PLATFORM_H
#define WINDOWS_PLATFORM_H
#include "../PlatformImplement.h"
#include "platform/Platform.h"

// ref directx-graphics-samples
// though here we include a lighter windows, but many includings use win directly, especially 3rd-party, still easy to meet some redefinition

#include <winsdkver.h>
#define _WIN32_WINNT 0x0A00
#include <sdkddkver.h>

// Use the C++ standard templated min/max
#ifndef NOMINMAX
#define NOMINMAX
#endif

// DirectX apps don't need GDI
#define NODRAWTEXT
#define NOGDI
#define NOBITMAP

// Include <mcx.h> if you need this
#define NOMCX

// Include <winsvc.h> if you need this
#define NOSERVICE

// WinHelp is deprecated
#define NOHELP

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

class WindowsPlatform : public PlatformImplement {
public:
    virtual void SetThreadAffinityMask(void* current_thread_handle, uint64_t mask) override;
    virtual void SetCurrentThreadAffinity(Affinity&& _affinity) override;
    virtual void SetCurrentThreadName(std::string_view _name) override;
    virtual void
    SetThreadGroupAffinity(void* current_thread_handle, uint16_t group_mask, uint64_t affinity_mask) override;
    virtual int32_t GetProcessorWorkGroupCount() override;
    virtual int32_t GetProcessorCoreCountInGroup(uint32_t groupID) override;
    virtual int32_t GetProcessorCoreCount() override;
    uint32_t        GetCurrentThreadID() override;
    virtual void    SetEnv(const char* _name, const char* _value) override;

    virtual const PlatformMemoryInfo& GetMemoryInfo() override;
};
#endif // !WINDOWS_PLATFORM_H
