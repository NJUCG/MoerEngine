#include "PlatformWindows.h"
#include "math/Function.h"
#include "misc/STL.h"

#include <processthreadsapi.h>
#include <winnt.h>

void WindowsPlatform::SetThreadAffinityMask(void* current_thread_handle, uint64_t mask) {

    ::SetThreadAffinityMask(current_thread_handle, mask);
}
void WindowsPlatform::SetCurrentThreadAffinity(Affinity&& _affinity) {
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    assert(size > 0 && "InitializeProcThreadAttributeList failed");
    Moer::Array<uint8_t> buffer(size);
    auto*                attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    auto                 res       = ::InitializeProcThreadAttributeList(attr_list, 1, 0, &size);
    assert(res && "InitializeProcThreadAttributeList failed");
    GROUP_AFFINITY group_affinity{};
    auto           cnt = _affinity.GetSize();
    if (cnt > 0) {
        group_affinity.Group = _affinity[0].windows.group;
        for (auto i = 0; i < cnt; ++i) {
            auto core = _affinity[i];
            assert(core.windows.group == group_affinity.Group && "Group must be the same");
            group_affinity.Mask |= 1ull << core.windows.idx;
        }
        ::UpdateProcThreadAttribute(
            attr_list,
            0,
            PROC_THREAD_ATTRIBUTE_GROUP_AFFINITY,
            &group_affinity,
            sizeof(group_affinity),
            nullptr,
            nullptr
        );
    }

    ::DeleteProcThreadAttributeList(attr_list);
}
void WindowsPlatform::SetCurrentThreadName(std::string_view _name) {
    static auto set_thread_description = reinterpret_cast<HRESULT(WINAPI*)(HANDLE, PCWSTR)>(
        GetProcAddress(GetModuleHandleA("kernelbase.dll"), "SetThreadDescription")
    );
    if (set_thread_description == nullptr) {
        return;
    }
    std::wstring wname(_name.begin(), _name.end());
    set_thread_description(GetCurrentThread(), wname.data());
}
void WindowsPlatform::SetThreadGroupAffinity(
    void*    current_thread_handle,
    uint16_t group_mask,
    uint64_t affinity_mask
) {
    GROUP_AFFINITY group_affinity{affinity_mask, group_mask, {0, 0, 0}};
    ::SetThreadGroupAffinity(current_thread_handle, &group_affinity, nullptr);
}

int32_t WindowsPlatform::GetProcessorWorkGroupCount() {
    return ::GetActiveProcessorGroupCount();
}

int32_t WindowsPlatform::GetProcessorCoreCountInGroup(uint32_t groupID) {
    return ::GetActiveProcessorCount(groupID);
}

int32_t WindowsPlatform::GetProcessorCoreCount() {
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    return sys_info.dwNumberOfProcessors;
}

uint32_t WindowsPlatform::GetCurrentThreadID() {
    return ::GetCurrentThreadId();
}

void WindowsPlatform::SetEnv(const char* _name, const char* _value) {
    SetEnvironmentVariableA(_name, _value);
}

const PlatformMemoryInfo& WindowsPlatform::GetMemoryInfo() {
    static PlatformMemoryInfo memory_info;

    if (memory_info.total_physical_memory == 0) {
        MEMORYSTATUSEX statex{};
        statex.dwLength = sizeof(statex);
        GlobalMemoryStatusEx(&statex);

        SYSTEM_INFO sys_info{};
        GetSystemInfo(&sys_info);

        memory_info.total_physical_memory = statex.ullTotalPhys;
        memory_info.total_virtual_memory  = statex.ullTotalVirtual;

        memory_info.page_size              = sys_info.dwPageSize;
        memory_info.allocation_granularity = sys_info.dwAllocationGranularity;

        memory_info.total_physical_memory_mb =
            static_cast<uint32_t>((memory_info.total_physical_memory + 1024 * 1024 - 1) / 1024 / 1024);
        //caclulate address limit by physical memory
        memory_info.addrress_limit = Moer::RoundUpToPowerOf2(memory_info.total_physical_memory);
    }
    return memory_info;
}
