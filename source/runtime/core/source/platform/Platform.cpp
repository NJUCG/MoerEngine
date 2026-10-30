#include "platform/Platform.h"
#include "PlatformImplement.h"
#include "misc/STL.h"
#include "misc/Types.h"
#include <cstdlib>
#if PLATFORM_WINDOWS
#include <basetsd.h>
#include "Windows/PlatformWindows.h"
#elif PLATFORM_LINUX
#include "Linux/PlatformLinux.h"
#elif PLATFORM_APPLE
#include "Apple/PlatformApple.h"
#endif

Affinity::Affinity(Affinity&& _other) noexcept {
    cores = std::move(_other.cores);
}
Affinity& Affinity::operator=(Affinity&& _other) noexcept {
    cores = std::move(_other.cores);
    return *this;
}

Affinity::Affinity(std::initializer_list<Core> _cores) {
    cores.reserve(_cores.size());
    for (auto& core : _cores) {
        cores.push_back(core);
    }
}

#if PLATFORM_WINDOWS

#ifndef NOMINMAX
#define NOMINMAX
#endif
namespace Moer {
static constexpr size_t max_core_cnt  = 256;
static constexpr size_t max_group_cnt = 256;

struct ProcessorGroup {
    uint32    cnt;
    KAFFINITY mask;
};
struct ProcessorGroups {
    StaticArray<ProcessorGroup, max_group_cnt> groups{};
    size_t                                     cnt = 0;
};

const ProcessorGroups& GetProcessorGroups() {
    static ProcessorGroups groups = [] {
        ProcessorGroups                         groups;
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info[32]{};
        DWORD                                   length = sizeof(info);
        if (!GetLogicalProcessorInformationEx(RelationGroup, info, &length)) {
            assert(false && "GetLogicalProcessorInformationEx failed");
        }
        DWORD count = length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);
        for (DWORD i = 0; i < count; ++i) {
            if (info[i].Relationship == RelationGroup) {
                auto group_cnt = info[i].Group.ActiveGroupCount;
                for (DWORD j = 0; j < group_cnt; ++j) {
                    groups.groups[j].cnt  = info[i].Group.GroupInfo[j].ActiveProcessorCount;
                    groups.groups[j].mask = info[i].Group.GroupInfo[j].ActiveProcessorMask;
                    groups.cnt++;
                }
            }
        }
        return groups;
    }();
    return groups;
}
} // namespace Moer
#else
#endif

PlatformStackTrace PlatformImplement::CaptureStackTrace(
    Moer::uint32,
    Moer::uint32
) noexcept {
    return {};
}

bool PlatformImplement::InitializeCrashDiagnostics() noexcept {
    return false;
}

PlatformCrashArtifactResult PlatformImplement::WriteCrashArtifacts(
    const PlatformCrashArtifactRequest&,
    Moer::uint32
) noexcept {
    return {};
}

[[noreturn]] void PlatformImplement::FailFast(std::string_view) noexcept {
    std::abort();
}

Affinity Affinity::All() {
#if PLATFORM_WINDOWS
    Affinity affinity;
    auto&    groups = Moer::GetProcessorGroups();
    for (size_t gid = 0; gid < groups.cnt; ++gid) {
        const auto& group = groups.groups[gid];
        Core        core;
        core.windows.group = gid;
        for (size_t cid = 0; cid < group.cnt; ++cid) {
            if (group.mask & (1ull << cid)) {
                core.windows.idx = cid;
                affinity.cores.emplace_back(core);
            }
        }
    }
    return affinity;
#elif PLATFORM_LINUX
    //MARK...
    return Affinity();
#elif PLATFORM_APPLE
    return Affinity();
#endif
}
Affinity Affinity::AnyOf(Moer::uint32 _thread_id, Affinity&& _in_affinity) {
#if PLATFORM_WINDOWS
    Affinity affinity;
    auto     cnt = _in_affinity.GetSize();
    if (cnt == 0) {
        return Affinity(std::move(_in_affinity));
    }
    auto group = _in_affinity[_thread_id % cnt].windows.group;
    for (auto& core : _in_affinity.cores) {
        if (core.windows.group == group) {
            affinity.cores.emplace_back(core);
        }
    }
    return affinity;
#elif PLATFORM_LINUX
    //MARK...
    return Affinity();
#elif PLATFORM_APPLE
    return Affinity();
#endif
}
PlatformImplement* PlatformImplement::GetInstance() {
#if PLATFORM_WINDOWS
    static WindowsPlatform platform;
#elif PLATFORM_LINUX
    static LinuxPlatform platform;
#elif PLATFORM_APPLE
    static ApplePlatform platform;
#endif

    return &platform;
}
void Platform::SetCurrentThreadAffinity(Affinity&& _affinity) {
    PlatformImplement::GetInstance()->SetCurrentThreadAffinity(std::move(_affinity));
}

void Platform::SetCurrentThreadName(std::string_view _name) {
    PlatformImplement::GetInstance()->SetCurrentThreadName(_name);
}
void Platform::SetThreadAffinity(void* current_thread_handle, Moer::uint64 mask) {

    PlatformImplement::GetInstance()->SetThreadAffinityMask(current_thread_handle, mask);
}
void Platform::SetThreadGroupAffinity(
    void*        current_thread_handle,
    Moer::uint16 group_mask,
    Moer::uint64 affinity_mask
) {
    PlatformImplement::GetInstance()->SetThreadGroupAffinity(
        current_thread_handle, group_mask, affinity_mask
    );
}

Moer::int32 Platform::GetProcessorWorkGroupCount() {
    return PlatformImplement::GetInstance()->GetProcessorWorkGroupCount();
}

Moer::int32 Platform::GetProcessorCoreCountInGroup(Moer::uint32 groupID) {
    return PlatformImplement::GetInstance()->GetProcessorCoreCountInGroup(groupID);
}

Moer::int32 Platform::GetProcessorCoreCount() {

    return PlatformImplement::GetInstance()->GetProcessorCoreCount();
}

Moer::uint32 Platform::GetCurrentThreadID() {
    return PlatformImplement::GetInstance()->GetCurrentThreadID();
}

const PlatformMemoryInfo& Platform::GetMemoryInfo() {
    return PlatformImplement::GetInstance()->GetMemoryInfo();
}

void Platform::SetEnv(const char* _name, const char* _value) {
    PlatformImplement::GetInstance()->SetEnv(_name, _value);
}

PlatformStackTrace
Platform::CaptureStackTrace(Moer::uint32 _frames_to_skip, Moer::uint32 _max_frames) noexcept {
    return PlatformImplement::GetInstance()->CaptureStackTrace(_frames_to_skip, _max_frames);
}

bool Platform::InitializeCrashDiagnostics() noexcept {
    return PlatformImplement::GetInstance()->InitializeCrashDiagnostics();
}

PlatformCrashArtifactResult
Platform::WriteCrashArtifacts(
    const PlatformCrashArtifactRequest& _request,
    Moer::uint32                        _timeout_ms
) noexcept {
    return PlatformImplement::GetInstance()->WriteCrashArtifacts(
        _request, _timeout_ms
    );
}

[[noreturn]] void Platform::FailFast(std::string_view _reason) noexcept {
    PlatformImplement::GetInstance()->FailFast(_reason);
}
