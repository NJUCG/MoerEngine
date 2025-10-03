#include "platform/Platform.h"
#include "PlatformImplement.h"
#include "misc/STL.h"
#include <basetsd.h>
#include <cstdint>
#if PLATFORM_WINDOWS
#include "Windows/PlatformWindows.h"
#elif PLATFORM_LINUX
#include "PlatformLinux.h"
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
    uint32_t  cnt;
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
#endif
}
Affinity Affinity::AnyOf(uint32_t _thread_id, Affinity&& _in_affinity) {
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
#endif
}
PlatformImplement* PlatformImplement::GetInstance() {
#if PLATFORM_WINDOWS
    static WindowsPlatform platform;
#elif PLATFORM_LINUX
    static LinuxPlatform platform;
#endif

    return &platform;
}
void Platform::SetCurrentThreadAffinity(Affinity&& _affinity) {
    PlatformImplement::GetInstance()->SetCurrentThreadAffinity(std::move(_affinity));
}

void Platform::SetCurrentThreadName(std::string_view _name) {
    PlatformImplement::GetInstance()->SetCurrentThreadName(_name);
}
void Platform::SetThreadAffinity(void* current_thread_handle, uint64_t mask) {

    PlatformImplement::GetInstance()->SetThreadAffinityMask(current_thread_handle, mask);
}
void Platform::SetThreadGroupAffinity(
    void*    current_thread_handle,
    uint16_t group_mask,
    uint64_t affinity_mask
) {
    PlatformImplement::GetInstance()->SetThreadGroupAffinity(
        current_thread_handle, group_mask, affinity_mask
    );
}

int32_t Platform::GetProcessorWorkGroupCount() {
    return PlatformImplement::GetInstance()->GetProcessorWorkGroupCount();
}

int32_t Platform::GetProcessorCoreCountInGroup(uint32_t groupID) {
    return PlatformImplement::GetInstance()->GetProcessorCoreCountInGroup(groupID);
}

int32_t Platform::GetProcessorCoreCount() {

    return PlatformImplement::GetInstance()->GetProcessorCoreCount();
}

uint32_t Platform::GetCurrentThreadID() {
    return PlatformImplement::GetInstance()->GetCurrentThreadID();
}

void Platform::SetEnv(const char* _name, const char* _value) {
    PlatformImplement::GetInstance()->SetEnv(_name, _value);
}
