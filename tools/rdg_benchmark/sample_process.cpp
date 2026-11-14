// Standalone Windows sampler for a child benchmark; no system profiling privilege required.
// Build with MSVC: cl /std:c++20 /O2 /EHsc sample_process.cpp /link dbghelp.lib
#define NOMINMAX
#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;

std::wstring Quote(const std::wstring& text) {
    // UE parses its raw command line: leave simple -Key=Value arguments unquoted.
    if (!text.empty() && text.find_first_of(L" \t\"") == std::wstring::npos) { return text; }
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto value : text) {
        if (value == L'\\') { ++slashes; continue; }
        result.append(slashes * (value == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (value == L'"') { result += L'\\'; }
        result += value;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

HANDLE FindThread(DWORD process, std::wstring_view name, DWORD& thread_id) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{sizeof(entry)};
    HANDLE result = nullptr;
    for (BOOL valid = Thread32First(snapshot, &entry); valid; valid = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process) { continue; }
        const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                                        FALSE, entry.th32ThreadID);
        if (!thread) { continue; }
        PWSTR description = nullptr;
        GetThreadDescription(thread, &description);
        const bool matches = description && std::wstring_view(description).find(name) != std::wstring_view::npos;
        if (description) { LocalFree(description); }
        if (matches) { result = thread; thread_id = entry.th32ThreadID; break; }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return result;
}

std::string Symbol(HANDLE process, DWORD64 address) {
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    SymGetModuleInfo64(process, address, &module);
    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol)) {
        return std::string(module.ModuleName) + "!" + symbol->Name;
    }
    std::ostringstream name;
    name << module.ModuleName << "!0x" << std::hex << address;
    return name.str();
}

int wmain(int argc, wchar_t** argv) {
    std::wstring prefix, thread_name;
    int delay_ms = 2000;
    int command_start = 0;
    for (int index = 1; index < argc; ++index) {
        const std::wstring option = argv[index];
        if (option == L"--") { command_start = index + 1; break; }
        if (index + 1 >= argc) { return 1; }
        if (option == L"--output") { prefix = argv[++index]; }
        else if (option == L"--thread") { thread_name = argv[++index]; }
        else if (option == L"--delay-ms") { delay_ms = std::stoi(argv[++index]); }
        else { return 1; }
    }
    if (prefix.empty() || command_start == 0 || command_start >= argc || delay_ms < 0) {
        std::cerr << "sample_process --output prefix [--thread name] [--delay-ms 2000] -- program args...\n";
        return 1;
    }
    std::wstring command;
    for (int index = command_start; index < argc; ++index) {
        if (!command.empty()) { command += L' '; }
        command += Quote(argv[index]);
    }
    { std::wofstream output(std::filesystem::path(prefix + L".command.txt")); output << command << '\n'; }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const HANDLE log = CreateFileW((prefix + L".child.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) { return 2; }
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = startup.hStdError = log;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &child)) {
        CloseHandle(log); return 3;
    }
    std::cout << "Sampling child " << child.dwProcessId << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    DWORD thread_id = child.dwThreadId;
    HANDLE thread = thread_name.empty() ? child.hThread : FindThread(child.dwProcessId, thread_name, thread_id);
    if (!thread) {
        std::cerr << "Requested benchmark thread was not found.\n";
        WaitForSingleObject(child.hProcess, INFINITE);
        CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(log); return 4;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(child.hProcess, nullptr, TRUE)) {
        std::cerr << "SymInitialize failed: " << GetLastError() << '\n';
        WaitForSingleObject(child.hProcess, INFINITE);
        if (thread != child.hThread) { CloseHandle(thread); }
        CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(log); return 5;
    }
    std::map<std::vector<DWORD64>, uint64_t> stacks;
    std::unordered_map<DWORD64, std::string> symbols;
    uint64_t samples = 0, failures = 0, short_stacks = 0;
    double pause_us = 0, max_pause_us = 0;
    const auto begin = Clock::now();
    while (WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) {
        // Jitter avoids always observing the same point in a periodic benchmark.
        std::this_thread::sleep_for(std::chrono::microseconds(1200 + (samples * 617) % 1300));
        std::vector<DWORD64> addresses;
        addresses.reserve(96);
        const auto pause_begin = Clock::now();
        if (SuspendThread(thread) == DWORD(-1)) { ++failures; break; }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
            STACKFRAME64 frame{};
            frame.AddrPC = {context.Rip, 0, AddrModeFlat};
            frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
            frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
            addresses.push_back(context.Rip);
            for (int depth = 0; depth < 95; ++depth) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, child.hProcess, thread, &frame, &context,
                                 nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset) {
                    break;
                }
                if (addresses.back() != frame.AddrPC.Offset) { addresses.push_back(frame.AddrPC.Offset); }
            }
        } else { ++failures; }
        ResumeThread(thread);
        const double elapsed = std::chrono::duration<double, std::micro>(Clock::now() - pause_begin).count();
        pause_us += elapsed;
        max_pause_us = std::max(max_pause_us, elapsed);
        if (addresses.empty()) { continue; }
        ++samples;
        if (addresses.size() < 3) { ++short_stacks; }
        for (const auto address : addresses) {
            if (!symbols.contains(address)) { symbols.emplace(address, Symbol(child.hProcess, address)); }
        }
        ++stacks[std::move(addresses)];
    }
    const double duration = std::chrono::duration<double>(Clock::now() - begin).count();
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(child.hProcess, &exit_code);
    std::ofstream folded(std::filesystem::path(prefix + L".folded"));
    for (const auto& [stack, count] : stacks) {
        bool first = true;
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            if (!first) { folded << ';'; }
            first = false;
            auto name = symbols[*it];
            std::replace(name.begin(), name.end(), ';', ':');
            folded << name;
        }
        folded << ' ' << count << '\n';
    }
    std::ofstream metadata(std::filesystem::path(prefix + L".json"));
    metadata << "{\"pid\":" << child.dwProcessId << ",\"tid\":" << thread_id
             << ",\"samples\":" << samples << ",\"failed_samples\":" << failures
             << ",\"short_stacks\":" << short_stacks << ",\"duration_s\":" << duration
             << ",\"suspend_total_ms\":" << pause_us / 1000 << ",\"suspend_max_ms\":" << max_pause_us / 1000
             << ",\"child_exit_code\":" << exit_code << "}\n";
    SymCleanup(child.hProcess);
    if (thread != child.hThread) { CloseHandle(thread); }
    CloseHandle(child.hThread); CloseHandle(child.hProcess); CloseHandle(log);
    std::cout << samples << " samples, " << short_stacks << " short stacks, pause " << pause_us / 1000 << " ms\n";
    return exit_code == 0 && samples > 0 ? 0 : 6;
}
