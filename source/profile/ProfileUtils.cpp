#include "ProfileUtils.h"


void capture_frames_fast(
    void**   out_frames,
    int      max_frames,
    uint16_t& out_count,
    uint16_t  skip)
{
    out_count = (uint16_t)CaptureStackBackTrace(
        skip, max_frames, out_frames, NULL);
}

uint64_t now_us()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(
        system_clock::now().time_since_epoch()
    ).count();
}

static CRITICAL_SECTION s_sym_cs;
static bool             s_sym_inited = false;

bool InitSymbolEngine() {
    HANDLE hProcess = GetCurrentProcess();
    
    SymCleanup(hProcess);

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);

    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string searchPath = exePath;
    searchPath = searchPath.substr(0, searchPath.find_last_of("\\/"));

    if (!SymInitialize(hProcess, searchPath.c_str(), TRUE)) {
        printf("[Profiler] SymInitialize Fatal Error: %lu\n", GetLastError());
        return false;
    }
    return true;
}

void* GetProcAddressFromPdb(const char* mangledName, const char* readableName) 
{
    HANDLE hProcess = GetCurrentProcess();
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
    pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSymbol->MaxNameLen = MAX_SYM_NAME;

    if (SymFromName(hProcess, mangledName, pSymbol)) {
        printf("[Profiler] Found by mangledname:%s\n", mangledName);
        return reinterpret_cast<void*>(pSymbol->Address);
    }

    if (readableName && SymFromName(hProcess, readableName, pSymbol)) {
        printf("[Profiler] Found by readable name: %s -> %p\n", readableName, (void*)pSymbol->Address);
        return reinterpret_cast<void*>(pSymbol->Address);
    }

    return nullptr;
}

std::string get_log_path() {
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string exe_dir = ".";
    if (len > 0 && len < MAX_PATH) {
        std::string full(buf, buf + len);
        auto pos = full.find_last_of("\\/");
        if (pos != std::string::npos) exe_dir = full.substr(0, pos);
    }
    std::filesystem::create_directories(exe_dir + "\\logs");
    return exe_dir + "\\logs";
}

//考虑性能，不建议在实时运行时解析符号。可以profile结束后手动解析mimalloc_profiler.log
//static CRITICAL_SECTION g_sym_cs;
static std::mutex g_symbol_mutex;
std::string symbolicate_address(void* addr) {
    std::string out;
    //EnterCriticalSection(&g_sym_cs);
    std::lock_guard<std::mutex> lock(g_symbol_mutex);
    HANDLE process = GetCurrentProcess();
    DWORD64 displacement = 0;
    char symbuf[sizeof(SYMBOL_INFO) + 1024];
    PSYMBOL_INFO pSym = (PSYMBOL_INFO)symbuf;
    pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSym->MaxNameLen = 1024;
    if (SymFromAddr(process, (DWORD64)addr, &displacement, pSym)) {
        IMAGEHLP_LINE64 line;
        DWORD displacementLine = 0;
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        if (SymGetLineFromAddr64(process, (DWORD64)addr, &displacementLine, &line)) {
            char buf[2048];
            sprintf_s(buf, sizeof(buf), "%s (%s:%u) +0x%llx",
                      pSym->Name,
                      line.FileName ? line.FileName : "?",
                      (unsigned)line.LineNumber,
                      (unsigned long long)displacement);
            out = buf;
        } else {
            char buf[1536];
            sprintf_s(buf, sizeof(buf), "%s +0x%llx", pSym->Name, (unsigned long long)displacement);
            out = buf;
        }
    } else {
        char buf[128];
        sprintf_s(buf, sizeof(buf), "0x%p (no-sym)", addr);
        out = buf;
    }
    //LeaveCriticalSection(&g_sym_cs);
    return out;
}