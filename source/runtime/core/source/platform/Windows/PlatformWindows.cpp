#include "PlatformWindows.h"
#include "math/Function.h"
#include "misc/STL.h"

#include <DbgHelp.h>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <processthreadsapi.h>
#include <sstream>
#include <iomanip>
#include <winnt.h>

#pragma comment(lib, "Dbghelp.lib")

namespace {

std::mutex& GetSymbolMutex() {
    static std::mutex mutex;
    return mutex;
}

bool EnsureSymbolsInitialized() {
    static bool initialized = [] {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        return SymInitialize(process, nullptr, TRUE) == TRUE;
    }();
    return initialized;
}

std::filesystem::path MakeCrashDumpPath(uint32_t thread_id) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm    local_time{};
    localtime_s(&local_time, &time);

    std::ostringstream name;
    name << "moer_crash_"
         << std::put_time(&local_time, "%Y%m%d_%H%M%S")
         << "_pid" << GetCurrentProcessId()
         << "_tid" << thread_id
         << ".dump";

    const std::filesystem::path dump_dir = std::filesystem::current_path() / "logs" / "crash";
    std::filesystem::create_directories(dump_dir);
    return dump_dir / name.str();
}

} // namespace

PlatformStackTrace WindowsPlatform::CaptureStackTrace(uint32_t frames_to_skip, uint32_t max_frames) {
    PlatformStackTrace trace{};
    trace.frames.resize(max_frames);
    const USHORT captured = ::CaptureStackBackTrace(
        static_cast<DWORD>(frames_to_skip + 1),
        static_cast<DWORD>(max_frames),
        reinterpret_cast<void**>(trace.frames.data()),
        nullptr
    );
    trace.frames.resize(captured);
    return trace;
}

std::string WindowsPlatform::FormatStackTrace(const PlatformStackTrace& trace) {
    if (trace.frames.empty()) {
        return {};
    }

    std::lock_guard lock(GetSymbolMutex());
    const bool symbols_ready = EnsureSymbolsInitialized();
    HANDLE     process = GetCurrentProcess();

    std::ostringstream stream;
    for (size_t index = 0; index < trace.frames.size(); ++index) {
        const DWORD64 address = static_cast<DWORD64>(trace.frames[index]);
        stream << '#' << index << ' ';

        if (!symbols_ready) {
            stream << "0x" << std::hex << address << std::dec << '\n';
            continue;
        }

        std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbol_storage{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_storage.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        DWORD64 displacement = 0;
        if (::SymFromAddr(process, address, &displacement, symbol) == TRUE) {
            stream << symbol->Name;
        } else {
            stream << "0x" << std::hex << address << std::dec;
        }

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (::SymGetLineFromAddr64(process, address, &line_displacement, &line) == TRUE) {
            stream << " (" << line.FileName << ':' << line.LineNumber << ')';
        }

        stream << '\n';
    }

    return stream.str();
}

PlatformCrashDumpResult WindowsPlatform::WriteCrashDump(const PlatformCrashDumpRequest& request) {
    PlatformCrashDumpResult result{};
    const std::filesystem::path dump_path = MakeCrashDumpPath(request.thread_id);
    HANDLE file = ::CreateFileW(
        dump_path.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        result.error_message = "CreateFileW failed";
        return result;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception_info{};
    exception_info.ThreadId = request.thread_id;
    exception_info.ExceptionPointers = nullptr;
    exception_info.ClientPointers = FALSE;

    const BOOL ok = ::MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        file,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory),
        nullptr,
        nullptr,
        nullptr
    );
    ::CloseHandle(file);

    if (ok != TRUE) {
        result.error_message = "MiniDumpWriteDump failed";
        return result;
    }

    result.written = true;
    result.path = dump_path;
    return result;
}

[[noreturn]] void WindowsPlatform::FailFast(std::string_view reason) {
    std::wstring wide_reason(reason.begin(), reason.end());
    ::OutputDebugStringW(wide_reason.c_str());
    ::RaiseFailFastException(nullptr, nullptr, 0);
    ::TerminateProcess(GetCurrentProcess(), 3);
    std::abort();
}

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
