#include "profile.h"
#include "../runtime/core/include/misc/MMemory.h"
#include "MinHook.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <vector>
#include <filesystem>
#include <mutex>
#include <map>

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

void InitPerfetto() {
    perfetto::TracingInitArgs args;
    args.backends |= perfetto::kInProcessBackend;
    perfetto::Tracing::Initialize(args);
    perfetto::TrackEvent::Register();
    std::cout << "[Perfetto] Initialized" << std::endl;
}

std::unique_ptr<perfetto::TracingSession> StartSession() {
    perfetto::TraceConfig cfg;
    cfg.add_buffers()->set_size_kb(1024);  // 1 MB

    auto* ds_cfg = cfg.add_data_sources()->mutable_config();
    ds_cfg->set_name("track_event");

    perfetto::protos::gen::TrackEventConfig te_cfg;
    te_cfg.add_enabled_categories("Rendering");  // Enable "rendering" category.
    te_cfg.add_enabled_categories("Memory");     // Enable "rendering" category.
    ds_cfg->set_track_event_config_raw(te_cfg.SerializeAsString());

    std::unique_ptr<perfetto::TracingSession> tracing_session =
        perfetto::Tracing::NewTrace();

    tracing_session->Setup(cfg);
    tracing_session->StartBlocking();

    std::cout << "[Perfetto] Session Started" << std::endl;
    return std::move(tracing_session);
}

static std::string get_log_path() {
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

// 导出 trace 文件
void StopSession(std::unique_ptr<perfetto::TracingSession>& tracing_session) {
    if (!tracing_session)
        return;
    tracing_session->StopBlocking();

    auto trace_data = tracing_session->ReadTraceBlocking();

    std::string pftracepath = get_log_path() + "\\trace.pftrace";
    std::ofstream output;
    output.open(pftracepath, std::ios::out | std::ios::binary);
    output.write(trace_data.data(), std::streamsize(trace_data.size()));
    output.close();
    std::cout << "[Perfetto] Trace exported to " << pftracepath << std::endl;
    tracing_session.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}


size_t getCurrentRSS( )
{
#if defined(_WIN32)
    /* Windows -------------------------------------------------- */
    PROCESS_MEMORY_COUNTERS info;
    GetProcessMemoryInfo( GetCurrentProcess( ), &info, sizeof(info) );
    return (size_t)info.WorkingSetSize;

#elif defined(__APPLE__) && defined(__MACH__)
    /* OSX ------------------------------------------------------ */
    struct mach_task_basic_info info;
    mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
    if ( task_info( mach_task_self( ), MACH_TASK_BASIC_INFO,
        (task_info_t)&info, &infoCount ) != KERN_SUCCESS )
        return (size_t)0L;      /* Can't access? */
    return (size_t)info.resident_size;

#elif defined(__linux__) || defined(__linux) || defined(linux) || defined(__gnu_linux__)
    /* Linux ---------------------------------------------------- */
    long rss = 0L;
    FILE* fp = NULL;
    if ( (fp = fopen( "/proc/self/statm", "r" )) == NULL )
        return (size_t)0L;      /* Can't open? */
    if ( fscanf( fp, "%*s%ld", &rss ) != 1 )
    {
        fclose( fp );
        return (size_t)0L;      /* Can't read? */
    }
    fclose( fp );
    return (size_t)rss * (size_t)sysconf( _SC_PAGESIZE);

#else
    /* AIX, BSD, Solaris, and Unknown OS ------------------------ */
    return (size_t)0L;          /* Unsupported. */
#endif
}

MemoryCounter memorycounter;

// mimalloc_profile
static constexpr int MAX_FRAMES = 32;
static constexpr size_t RING_CAPACITY = 1 << 16;
static constexpr size_t LOG_BATCH_FLUSH = 128;


enum EventType : uint8_t { EVT_ALLOC = 1, EVT_FREE = 2 };

struct EventRecord 
{
    uint64_t ts_us;                          // timestamp
    uint8_t type;                            // EVT_ALLOC/EVT_FREE
    USHORT frame_count;                      // number of frames captured
    size_t size;                             // allocation size
    void* ptr;                               // address returned
    void* frames[MAX_FRAMES];
};

struct RingBuffer {
    std::mutex mtx;
    EventRecord* buffer;
    size_t capacity_mask;
    std::atomic<uint64_t> head;
    std::atomic<uint64_t> tail;
    std::atomic<uint64_t> dropped;
    RingBuffer(size_t capacity) 
    {
        buffer = (EventRecord*)VirtualAlloc(NULL, sizeof(EventRecord) * capacity,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        capacity_mask = capacity - 1;
        head.store(0);
        tail.store(0);
        dropped.store(0);
    }
    ~RingBuffer() {
        if (buffer) VirtualFree(buffer, 0, MEM_RELEASE);
    }

    bool push(const EventRecord& rec) {
        std::lock_guard<std::mutex> lock(mtx);
        uint64_t cur_head = head.load(std::memory_order_acquire);
        uint64_t cur_tail = tail.load(std::memory_order_acquire);

        if (cur_head >= cur_tail + (capacity_mask + 1)) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        size_t idx = cur_head & capacity_mask;
        buffer[idx] = rec;

        head.store(cur_head + 1, std::memory_order_release);
        return true;
    }
    
    bool pop(EventRecord& out) {
        uint64_t cur_tail = tail.load(std::memory_order_acquire);
        uint64_t cur_head = head.load(std::memory_order_acquire);
        if (cur_tail >= cur_head) return false;
        size_t idx = cur_tail & capacity_mask;
        out = buffer[idx];
        tail.store(cur_tail + 1, std::memory_order_release);
        return true;
    }
};

static RingBuffer* g_ring = nullptr;

static CRITICAL_SECTION g_sym_cs;
static std::atomic<bool> g_dbghelp_inited(false);

static std::thread g_worker;
static std::atomic<bool> g_worker_running(false);
static std::ofstream g_log_ofs;

thread_local bool g_in_hook = false;

// get microsecond timestamp
static uint64_t now_us() {
    using namespace std::chrono;
    auto p = system_clock::now().time_since_epoch();
    return (uint64_t)duration_cast<microseconds>(p).count();
}

static void capture_frames_fast(void** out_frames, int max_frames, USHORT& out_count, USHORT skip = 1) {
    USHORT frames = CaptureStackBackTrace(skip, max_frames, out_frames, NULL);
    out_count = frames;
}

static std::string symbolicate_address(void* addr) {
    std::string out;
    EnterCriticalSection(&g_sym_cs);
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
    LeaveCriticalSection(&g_sym_cs);
    return out;
}

static void worker_thread_func() {
    size_t batch_count = 0;
    EventRecord rec;
    uint64_t local_processed = 0;
    while (g_worker_running.load(std::memory_order_acquire)) {
        bool has = g_ring->pop(rec);
        if (!has) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        char header[256];
        SYSTEMTIME st;
        uint64_t us = rec.ts_us;
        time_t secs = (time_t)(us / 1000000ULL);
        uint32_t usec = (uint32_t)(us % 1000000ULL);
        tm tmv;
        gmtime_s(&tmv, &secs);
        sprintf_s(header, sizeof(header), "[%04d-%02d-%02d %02d:%02d:%02d.%06u] %s ptr=0x%p size=%zu\n",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                  usec, (rec.type == EVT_ALLOC ? "ALLOC" : "FREE"), rec.ptr, rec.size);
        g_log_ofs << header;
        // symbolicate captured frames (fast mode)
        for (uint8_t i = 0; i < rec.frame_count; ++i) {
            void* a = rec.frames[i];
            std::string s = symbolicate_address(a);
            g_log_ofs << "    #" << (int)i << " " << s << "\n";
        }
        g_log_ofs << "----------------------------------------\n";
        ++batch_count;
        ++local_processed;
        if (batch_count >= LOG_BATCH_FLUSH) {
            g_log_ofs.flush();
            batch_count = 0;
        }
    }
    g_log_ofs.flush();
}

// UI
std::vector<HotspotSnapshot> GetHotspots(size_t top_n) {
    std::vector<HotspotSnapshot> out;
    std::lock_guard<std::mutex> lock(g_hotspots_mtx);

    for (auto& [key, info] : g_hotspots) {
        HotspotSnapshot snap;
        snap.total_size = info.total_size;
        snap.alloc_count = info.alloc_count;

        for (auto f : key.frames) {
            snap.stack_str += symbolicate_address(f) + "\n";
        }
        out.push_back(std::move(snap));
    }

    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.total_size > b.total_size; });
    if (out.size() > top_n) out.resize(top_n);
    return out;
}

static std::map<void*, AllocationInfo> g_allocations;
static std::mutex g_alloc_mtx;

static std::atomic<size_t> g_live_bytes{0};
static std::atomic<size_t> g_peak_bytes{0};
static std::atomic<size_t> g_live_alloc_count{0};
size_t Profile_GetLiveBytes() { return g_live_bytes.load(); }
size_t Profile_GetPeakBytes() { return g_peak_bytes.load(); }
size_t Profile_GetLiveAllocCount() { return g_live_alloc_count.load(); }

size_t Profile_GetAllocationCount()
{
    std::lock_guard<std::mutex> lock(g_alloc_mtx);
    return g_allocations.size();
}
//---------------

//typedef void* (*malloc_t)(size_t);
//typedef void  (*free_t)(void*);
typedef void* (*Malloc_t)(size_t);
typedef void (*Free1_t)(void* p);
typedef void (*Free2_t)(void* p, size_t size);

//malloc_t orig_malloc = nullptr;
//free_t   orig_free   = nullptr;
Malloc_t orig_Malloc = nullptr;
Free1_t orig_Free1 = nullptr;
Free2_t orig_Free2 = nullptr;

void* malloc_hook(size_t size) {
    if (g_in_hook) {
        // already in hook, do not record
        return orig_Malloc(size);
    }
    g_in_hook = true;

    void* p = orig_Malloc(size);

    // prepare event (avoid any allocation/free)
    EventRecord rec;
    rec.ts_us = now_us();
    rec.type = EVT_ALLOC;
    rec.size = size;
    rec.ptr = p;
    rec.frame_count = 0;

    {
        std::lock_guard<std::mutex> lock(g_alloc_mtx);

        g_allocations[p] = {
            size,
            std::chrono::steady_clock::now()
        };
    }
    
    g_live_bytes.fetch_add(size, std::memory_order_relaxed);
    g_live_alloc_count.fetch_add(1, std::memory_order_relaxed);
    size_t current_live = g_live_bytes.load();
    size_t peak        = g_peak_bytes.load();
    while (current_live > peak && !g_peak_bytes.compare_exchange_weak(peak, current_live)) {
    }

    capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

    // --- 更新热点统计 ---
    StackKey key;
    key.frames.assign(rec.frames, rec.frames + rec.frame_count);

    {
        std::lock_guard<std::mutex> lock(g_hotspots_mtx);
        auto& info = g_hotspots[key];
        info.total_size += size;
        info.alloc_count += 1;
    }
//----------
    if (g_ring) g_ring->push(rec);

    g_in_hook = false;
    //std::cout << "[HOOK] malloc " << size << " -> " << p << "\n";
    return p;
}
void free_hook_1(void* p)
{
    if (g_in_hook)
    {
        orig_Free1(p);
        return;
    }

    g_in_hook = true;

    size_t freed_size = 0;

    {
        std::lock_guard<std::mutex> lock(g_alloc_mtx);
        auto it = g_allocations.find(p);
        if (it != g_allocations.end()) {
            freed_size = it->second.size;
            g_allocations.erase(it);
        }
    }

    if (freed_size > 0) {
        g_live_bytes.fetch_sub(freed_size, std::memory_order_relaxed);
        g_live_alloc_count.fetch_sub(1, std::memory_order_relaxed);
        EventRecord rec;
        rec.ts_us = now_us();
        rec.type = EVT_FREE;
        rec.size = freed_size;
        rec.ptr = p;
        rec.frame_count = 0;

        if (g_ring) g_ring->push(rec);
    }

    orig_Free1(p);

    g_in_hook = false;
}
void free_hook_2(void* p, size_t size)
{
    if (g_in_hook)
    {
        orig_Free2(p, size);
        return;
    }

    g_in_hook = true;

    {
        std::lock_guard<std::mutex> lock(g_alloc_mtx);
        g_allocations.erase(p);
    }

    if (size > 0) {
        g_live_bytes.fetch_sub(size, std::memory_order_relaxed);
        g_live_alloc_count.fetch_sub(1, std::memory_order_relaxed);
        EventRecord rec;
        rec.ts_us = now_us();
        rec.type = EVT_FREE;
        rec.size = size;
        rec.ptr = p;
        rec.frame_count = 0;

        if (g_ring) g_ring->push(rec);
    }

    orig_Free2(p, size);
    //std::cout << "[HOOK] Free2 " << " -> " << p << "\n";
    g_in_hook = false;
}
/*
void free_hook(void* p) {
    if (g_in_hook) {
        orig_free(p);
        return;
    }
    g_in_hook = true;

    // record event
    EventRecord rec;
    rec.ts_us = now_us();
    rec.type = EVT_FREE;
    rec.size = 0;
    rec.ptr = p;
    rec.frame_count = 0;

    capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

    if (g_ring) g_ring->push(rec);

    orig_free(p);

    g_in_hook = false;
    //std::cout << "[HOOK] free " << p << "\n";
}
*/


void install_hooks() {
    MH_Initialize();
    //[todo?]
    //MH_CreateHook(&mi_malloc, &malloc_hook, reinterpret_cast<LPVOID*>(&orig_malloc));
    //MH_CreateHook(&mi_free,   &free_hook,   reinterpret_cast<LPVOID*>(&orig_free));
    MH_CreateHook(&Memory::Malloc, &malloc_hook, reinterpret_cast<LPVOID*>(&orig_Malloc));
    MH_CreateHook(
    reinterpret_cast<LPVOID>(
        static_cast<Free1_t>(&Memory::Free)
    ),
    &free_hook_1,
    reinterpret_cast<LPVOID*>(&orig_Free1)
);

MH_CreateHook(
    reinterpret_cast<LPVOID>(
        static_cast<Free2_t>(&Memory::Free)
    ),
    &free_hook_2,
    reinterpret_cast<LPVOID*>(&orig_Free2)
);
    MH_EnableHook(MH_ALL_HOOKS);
}

void remove_hooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

void initialize_mimalloc_profiler() {
    // Prevent double init
    static bool inited = false;
    if (inited) return;
    inited = true;

    g_ring = new RingBuffer(RING_CAPACITY);

    InitializeCriticalSection(&g_sym_cs);
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    if (!SymInitialize(GetCurrentProcess(), NULL, TRUE)) {
        DWORD err = GetLastError();
        std::fprintf(stderr, "SymInitialize failed: %u\n", (unsigned)err);
    } else {
        g_dbghelp_inited.store(true);
    }

    std::string log_path = get_log_path() + "\\mimalloc_profiler.log";
    g_log_ofs.open(log_path, std::ios::out | std::ios::trunc);
    if (!g_log_ofs.is_open()) {
        std::fprintf(stderr, "Failed to open log file: %s\n", log_path.c_str());
    } else {
        g_log_ofs << "=== mimalloc profiler log ===\n";
    }

    install_hooks();

    // start worker
    g_worker_running.store(true);
    g_worker = std::thread(worker_thread_func);

    std::cout<<"[MINHOOK]Install"<<std::endl;
}

void DumpLeaks()
{
    std::lock_guard<std::mutex> lock(g_alloc_mtx);

    if (g_allocations.empty())
    {
        std::cout << "No leaks\n";
        return;
    }

    size_t total = 0;

    for (auto& [ptr, info] : g_allocations)
    {
        std::cout << "Leak: " << ptr
                  << " size=" << info.size << "\n";
        total += info.size;
    }

    std::cout << "Total leaked: " << total << " bytes\n";
}

void shutdown_mimalloc_profiler() {
    remove_hooks();

    // stop worker
    g_worker_running.store(false);
    if (g_worker.joinable()) g_worker.join();

    // cleanup
    if (g_log_ofs.is_open()) {
        g_log_ofs << "=== profiler shutdown ===\n";
        g_log_ofs.flush();
        g_log_ofs.close();
    }
    if (g_dbghelp_inited.load()) {
        SymCleanup(GetCurrentProcess());
        g_dbghelp_inited.store(false);
    }
    DeleteCriticalSection(&g_sym_cs);

    if (g_ring) { delete g_ring; g_ring = nullptr; }
    DumpLeaks();
    std::cout<<"[MINHOOK]Shutdown"<<std::endl;
}

