#include <windows.h>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    std::string targetExe = "MoerEditor.exe";
    if (argc >= 2) targetExe = argv[1];

    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    fs::path dllPath = fs::path(path).parent_path() / "moer_profiled.dll";

    if (!fs::exists(dllPath)) {
        std::cerr << "DLL not found: " << dllPath << std::endl;
        return -1;
    }

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(targetExe.c_str(), NULL, NULL, NULL, FALSE, 
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        return -1;
    }

    std::string p = dllPath.string();
    LPVOID remoteMem = VirtualAllocEx(pi.hProcess, NULL, p.length() + 1, MEM_COMMIT, PAGE_READWRITE);
    WriteProcessMemory(pi.hProcess, remoteMem, p.c_str(), p.length() + 1, NULL);
    
    LPVOID loadLib = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE hThread = CreateRemoteThread(pi.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib, remoteMem, 0, NULL);

    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
    }

    std::cout << "[Profiler] Waiting for hooks to settle..." << std::endl;
    Sleep(500); 

    ResumeThread(pi.hThread);
    std::cout << "[Profiler] Engine resumed." << std::endl;

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}