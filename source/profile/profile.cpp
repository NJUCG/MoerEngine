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
#include <string_view>
#include <process.h>
#include <future>
#include <deque>
#include "vulkan/vulkan.h"

#pragma comment(lib,"psapi.lib")
#pragma comment(lib, "dbghelp.lib")

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

std::unique_ptr<perfetto::TracingSession> g_session;
// 导出 trace 文件
void StopSession() {
    if (!g_session) return;

    std::cout << "[Perfetto] Stopping session..." << std::endl;
    auto future = std::async(std::launch::async, []() {
        g_session->Stop();
        return g_session->ReadTraceBlocking();
    });

    // 等 2 秒，等不到就说明后台线程死掉了
    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        auto trace_data = future.get();

        if (!trace_data.empty()) {
            std::string pftracepath = get_log_path() + "\\trace.pftrace";
            std::ofstream output(pftracepath, std::ios::out | std::ios::binary);
            if (output.is_open()) {
                output.write(trace_data.data(), std::streamsize(trace_data.size()));
                output.close();
                std::cout << "[Perfetto] Trace saved: " << pftracepath << std::endl;
            }
        }
    } else {
        std::cerr << "[Perfetto] CRITICAL: StopSession timed out, skipping save to avoid hang." << std::endl;
        g_session.release(); 
    }
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
static std::chrono::steady_clock::time_point g_start_time = std::chrono::steady_clock::now();
static constexpr size_t LOG_BATCH_FLUSH = 0;

struct VkTmpBufferAllocator;

enum class EVkInternalBufferUsage {
    Upload,
    Readback,
    Scratch,
    ShaderBuffer,
    ShaderBuffer_Constant
};

static const char* GetVkUsageName(uint32_t usage) {
    static const char* labels[] = {
        "Upload",
        "Readback",
        "Scratch",
        "ShaderBuffer",
        "ShaderBuffer_Constant"
    };

    if (usage < 5) {
        return labels[usage];
    }
    return "Unknown/ByName";
}

struct RingBuffer {
    std::mutex push_mtx;
    EventRecord* buffer;
    size_t capacity_mask;
    
    std::atomic<uint64_t> head;
    std::atomic<uint64_t> tail;
    std::atomic<uint64_t> dropped;

    RingBuffer(size_t capacity) {
        buffer = (EventRecord*)VirtualAlloc(NULL, sizeof(EventRecord) * capacity,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        capacity_mask = capacity - 1;
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
        dropped.store(0, std::memory_order_relaxed);
    }

    ~RingBuffer() {
        if (buffer) VirtualFree(buffer, 0, MEM_RELEASE);
    }

    bool push(const EventRecord& rec) {
        std::lock_guard<std::mutex> lock(push_mtx);

        uint64_t cur_head = head.load(std::memory_order_relaxed);
        uint64_t cur_tail = tail.load(std::memory_order_acquire);

        if (cur_head - cur_tail >= (capacity_mask + 1)) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        size_t idx = cur_head & capacity_mask;
        buffer[idx] = rec;

        head.store(cur_head + 1, std::memory_order_release);
        return true;
    }

    bool pop(EventRecord& out) {
        uint64_t cur_tail = tail.load(std::memory_order_relaxed);
        uint64_t cur_head = head.load(std::memory_order_acquire);

        if (cur_tail >= cur_head) {
            return false;
        }

        size_t idx = cur_tail & capacity_mask;
        out = buffer[idx];

        tail.store(cur_tail + 1, std::memory_order_release);
        return true;
    }

    uint64_t size() const {
        return head.load() - tail.load();
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

//考虑性能，不在实时运行时解析符号。可以profile结束后手动解析mimalloc_profiler.log
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

PROFILE_API SourceUIConfig g_UIConfigs[3] = {
    { MemorySource::Editor,    "System Editor", ImVec4(0.4f, 0.8f, 0.4f, 1.0f) },
    { MemorySource::Vulkan,    "Vulkan Static", ImVec4(0.2f, 0.6f, 1.0f, 1.0f) },
    { MemorySource::VulkanTmp, "Vulkan Temp",   ImVec4(1.0f, 0.7f, 0.2f, 1.0f) }
};

std::unordered_map<void*, LiveAllocInfo> g_live_allocs; 
std::unordered_map<StackKey, HotspotInfo, StackKeyHash> g_hotspots; 
std::mutex g_hotspots_mtx;

PROFILE_API std::deque<TimePoint> g_history_data;
PROFILE_API std::mutex g_history_mtx;

LiveMetrics g_metrics;

const char* GetSourceStr(MemorySource s) {
    static const char* sources[] = { "Editor", "Vulkan", "VulkanTmp" };
    return sources[(int)s];
}

const char* GetActionStr(MemoryAction a) {
    return (a == MemoryAction::Alloc) ? "ALLOC" : "FREE";
}

PROFILE_API size_t Profile_GetPeakBytesBySource(MemorySource source) {
    return g_metrics.peaks[(size_t)source].load();
}

static void worker_thread_func() {
    g_in_hook = true;
    
    EventRecord rec;
    size_t batch_count = 0;

    while (true) {
        if (!g_ring->pop(rec)) {
            if (!g_worker_running.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const uint8_t s_idx = static_cast<uint8_t>(rec.source);
        if (s_idx >= (uint8_t)MemorySource::MAX_SOURCES) continue;
        size_t live_size = 0; //rec.size for Free didn't record accuately
        // (Metrics & Hotspots)
        if (rec.action == MemoryAction::Alloc) {
            LiveAllocInfo& live = g_live_allocs[rec.ptr];
            live.size = rec.size;
            live_size = rec.size;
            live.source = rec.source;
            live.ptr = rec.ptr;
            live.key.frame_count = rec.frame_count;
            memcpy(live.key.frames, rec.frames, rec.frame_count * sizeof(void*));
            
            StackKeyHash hasher;
            live.key.cached_hash = hasher(live.key);

            size_t current_val = g_metrics.bytes[s_idx].fetch_add(rec.size, std::memory_order_relaxed) + rec.size;
            size_t old_peak = g_metrics.peaks[s_idx].load(std::memory_order_relaxed);
            while (current_val > old_peak && 
                   !g_metrics.peaks[s_idx].compare_exchange_weak(old_peak, current_val, std::memory_order_relaxed));

            {
                std::lock_guard<std::mutex> lock(g_hotspots_mtx);
                auto& h_info = g_hotspots[live.key];
                h_info.total_size += rec.size;
                h_info.alloc_count++;
                h_info.source = rec.source;
            }
            // if(rec.source == MemorySource::Vulkan)
            // {
            //     printf("[vkAllocateMemory_Hook]ALLOC:%zd\n",live.size);
            // }
        } 
        else if (rec.action == MemoryAction::Free) {
            auto it = g_live_allocs.find(rec.ptr);
            if (it != g_live_allocs.end()) {
                const auto& live = it->second;

            // if(rec.source == MemorySource::Vulkan)
            // {
            //     printf("[vkAllocateMemory_Hook]FREE:%zd\n",live.size);
            // }    
            live_size = live.size;
                if (g_metrics.bytes[s_idx] >= live.size) {
                    g_metrics.bytes[s_idx].fetch_sub(live.size, std::memory_order_relaxed);
                }

                {
                    std::lock_guard<std::mutex> lock(g_hotspots_mtx);
                    auto h_it = g_hotspots.find(live.key);
                    if (h_it != g_hotspots.end()) {
                        if (h_it->second.total_size >= live.size) {
                            h_it->second.total_size -= live.size;
                        }
                    }
                }
                g_live_allocs.erase(it);
            }
        }
        //(File Logging)
        char header[512];
        uint64_t us = rec.ts_us;
        time_t secs = (time_t)(us / 1000000ULL);
        uint32_t usec = (uint32_t)(us % 1000000ULL);
        tm tmv;
        gmtime_s(&tmv, &secs);

        int offset = sprintf_s(header, sizeof(header), 
            "[%04d-%02d-%02d %02d:%02d:%02d.%06u] [%s:%s] ptr=0x%p size=%zu",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
            usec, GetSourceStr(rec.source), GetActionStr(rec.action), rec.ptr, live_size);

        if (rec.source == MemorySource::VulkanTmp && rec.action == MemoryAction::Alloc) {
            if (rec.usage != 0xFFFFFFFF) {
                sprintf_s(header + offset, sizeof(header) - offset, " usage=%s\n", GetVkUsageName(rec.usage));
            } else {
                sprintf_s(header + offset, sizeof(header) - offset, " name=%s\n", rec.name);
            }
        } else {
            sprintf_s(header + offset, sizeof(header) - offset, "\n");
        }

        g_log_ofs << header;
        for (uint16_t i = 0; i < rec.frame_count; ++i) {
            g_log_ofs << "    #" << i << " " << rec.frames[i] << "\n";
        }
        g_log_ofs << "----------------------------------------\n";
        if (LOG_BATCH_FLUSH > 0 && ++batch_count >= LOG_BATCH_FLUSH) {
            g_log_ofs.flush();
            batch_count = 0;
        }
    }
    
    g_in_hook = false;
}

// UI--------------------------------------------
void Profile_TickSample() {
    static auto last_sample = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sample).count() < 100) {
        return;
    }
    last_sample = now;

    TimePoint tp;
    tp.time = std::chrono::duration<float>(now - g_start_time).count();
    
    bool has_any_value = false;
    for (int i = 0; i < SOURCE_COUNT; ++i) {
        tp.values[i] = static_cast<float>(g_metrics.bytes[i].load(std::memory_order_relaxed));
        if (tp.values[i] > 0) has_any_value = true;
    }


    std::lock_guard<std::mutex> lock(g_history_mtx);
    g_history_data.push_back(tp);

    if (g_history_data.size() > 2000) { 
        g_history_data.pop_front();
    }
}

size_t Profile_GetBytesBySource(MemorySource source) {
    return g_metrics.bytes[(size_t)source].load();
}

std::vector<HotspotSnapshot> GetHotspots(size_t top_n, MemorySource filterSource) {
    std::vector<HotspotSnapshot> out;
    std::lock_guard<std::mutex> lock(g_hotspots_mtx);

    for (auto& pair : g_hotspots) {
        const StackKey& key = pair.first;
        const HotspotInfo& info = pair.second;

        if (info.source != filterSource || info.total_size == 0) continue;

        HotspotSnapshot snap;
        snap.total_size = info.total_size;
        snap.alloc_count = info.alloc_count;
        snap.source = info.source;

        for (int i = 0; i < key.frame_count; ++i) {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%p\n", key.frames[i]);
            snap.stack_str += buf;
        }
        out.push_back(std::move(snap));
    }

    std::sort(out.begin(), out.end(), [](const HotspotSnapshot& a, const HotspotSnapshot& b) {
        return a.total_size > b.total_size;
    });

    if (out.size() > top_n) out.resize(top_n);
    return out;
}


//---------------
static std::mutex g_alloc_mtx;

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
        return orig_Malloc(size);
    }
    g_in_hook = true;

    void* p = orig_Malloc(size);
    if (g_ring && p) { 
        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Editor;
        rec.action = MemoryAction::Alloc;
        rec.size = size;
        rec.ptr = p;
        
        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

        g_ring->push(rec); 
    }

    g_in_hook = false;
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
    if (g_ring) { 
        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Editor;
        rec.action = MemoryAction::Free;
        rec.size = 0;
        rec.ptr = p;
        
        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

        g_ring->push(rec); 
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

    if (g_ring) { 
        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Editor;
        rec.action = MemoryAction::Free;
        rec.size = size;
        rec.ptr = p;
        
        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

        g_ring->push(rec); 
    }

    orig_Free2(p, size);

    g_in_hook = false;
}

// ----------------- VkTmp pointer -----------------
typedef uint64_t(*PFN_VkTmpAllocate_1)(
    VkTmpBufferAllocator* _this,
    uint64_t _size, 
    std::string_view _name
);

typedef uint64_t(*PFN_VkTmpAllocate_2)(
    VkTmpBufferAllocator* _this,
    uint64_t _size,
    EVkInternalBufferUsage _usage
);

typedef void(*PFN_VkTmpDeAllocate)(
    VkTmpBufferAllocator* _this,
    uint64_t _handle
);

PFN_VkTmpAllocate_1 orig_VkTmpAllocate_1 = nullptr;
PFN_VkTmpAllocate_2 orig_VkTmpAllocate_2 = nullptr;
PFN_VkTmpDeAllocate orig_VkTmpDeAllocate = nullptr;


uint64_t VkTmpAllocate_Hook_1(
    VkTmpBufferAllocator* _this,
    uint64_t _size, 
    std::string_view _name
)
{
    uint64_t handle = orig_VkTmpAllocate_1(_this, _size, _name);

    if (!g_in_hook)
    {
        g_in_hook = true;

        void* p = reinterpret_cast<void*>(handle);// not a address pointer
        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::VulkanTmp;
        rec.action = MemoryAction::Alloc;
        rec.size = _size;
        rec.ptr = p;
        rec.frame_count = 0;
        rec.usage = 0xFFFFFFFF;
        
        size_t copy_len = (_name.size() < 31) ? _name.size() : 31;
        memcpy(rec.name, _name.data(), copy_len);
        rec.name[copy_len] = '\0';
        

        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 1);

        if (g_ring) g_ring->push(rec);

        g_in_hook = false;
    }

    return handle;
}

uint64_t VkTmpAllocate_Hook_2(
    VkTmpBufferAllocator* _this,
    uint64_t _size,
    EVkInternalBufferUsage _usage)
{
    uint64_t handle = orig_VkTmpAllocate_2(_this, _size, _usage);

    if (!g_in_hook)
    {
        g_in_hook = true;
        void* p = reinterpret_cast<void*>(handle);// not a address pointer
        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::VulkanTmp;
        rec.action = MemoryAction::Alloc;
        rec.size = _size;
        rec.ptr = p; // not a address pointer
        rec.frame_count = 0;
        rec.usage = (uint32_t)_usage;
        rec.name[0] = '\0';
        
        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 1);

        if (g_ring) g_ring->push(rec);

        g_in_hook = false;
    }

    return handle;
}


void VkTmpDeAllocate_Hook(
    VkTmpBufferAllocator* _this,
    uint64_t _handle)
{
    if (!g_in_hook)
    {
        g_in_hook = true;

        void* p = reinterpret_cast<void*>(_handle);

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::VulkanTmp;
        rec.action = MemoryAction::Free;
        rec.size = 0;
        rec.ptr = p;
        rec.frame_count = 0;

        if (g_ring)
        {
            g_ring->push(rec);
        }
        orig_VkTmpDeAllocate(_this, _handle);
        g_in_hook = false;
    }
}
//---------------Vk--------------

typedef VkResult (VKAPI_PTR* PFN_vkAllocateMemory_t)(
    VkDevice,
    const VkMemoryAllocateInfo*,
    const VkAllocationCallbacks*,
    VkDeviceMemory*);

typedef void (VKAPI_PTR* PFN_vkFreeMemory_t)(
    VkDevice,
    VkDeviceMemory,
    const VkAllocationCallbacks*);

PFN_vkAllocateMemory_t orig_vkAllocateMemory = nullptr;
PFN_vkFreeMemory_t     orig_vkFreeMemory = nullptr;

VkResult VKAPI_PTR vkAllocateMemory_Hook(
    VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory* pMemory)
{
    VkResult result = orig_vkAllocateMemory(
        device,
        pAllocateInfo,
        pAllocator,
        pMemory);

    if (result == VK_SUCCESS && !g_in_hook)
    {
        g_in_hook = true;

        uint64_t size = pAllocateInfo->allocationSize;
        void* p = reinterpret_cast<void*>(*pMemory);

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Vulkan;
        rec.action = MemoryAction::Alloc;
        rec.size = size;
        rec.ptr = p;
        rec.frame_count = 0;

        capture_frames_fast(
            rec.frames,
            MAX_FRAMES,
            rec.frame_count,
            1);

        if (g_ring)
        {
            g_ring->push(rec);
        }  

        g_in_hook = false;
    }

    return result;
}

void VKAPI_PTR vkFreeMemory_Hook(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator)
{
    if (!g_in_hook)
    {
        g_in_hook = true;

        void* p = reinterpret_cast<void*>(memory);
        uint64_t size = 0;

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Vulkan;
        rec.action = MemoryAction::Free;
        rec.size = size;
        rec.ptr = p;
        rec.frame_count = 0;

        capture_frames_fast(
            rec.frames,
            MAX_FRAMES,
            rec.frame_count,
            1);

        if (g_ring)
        {
            g_ring->push(rec);
        }
                
        orig_vkFreeMemory(device, memory, pAllocator);
        g_in_hook = false;
    }
}
//--------------
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vkGetDeviceProcAddr)(VkDevice device, const char* pName);
PFN_vkGetDeviceProcAddr orig_vkGetDeviceProcAddr = nullptr;

//hook vkGetDeviceProcAddr, replace vkAllocateMemory to vkAllocateMemory_Hook
PFN_vkVoidFunction VKAPI_PTR Detour_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    PFN_vkVoidFunction realAddr = orig_vkGetDeviceProcAddr(device, pName);

    if (pName && strcmp(pName, "vkAllocateMemory") == 0) {
        printf("[profile] Detour_vkGetDeviceProcAddr1\n");
        orig_vkAllocateMemory = (PFN_vkAllocateMemory_t)realAddr;
        return (PFN_vkVoidFunction)vkAllocateMemory_Hook; //hook vkAllocateMemory 第二层，通过vkGetDeviceProcAddr直接返回hook函数
    }
    
    if (pName && strcmp(pName, "vkFreeMemory") == 0) {
        orig_vkFreeMemory = (PFN_vkFreeMemory_t)realAddr;
        return (PFN_vkVoidFunction)vkFreeMemory_Hook;
    }

    return realAddr;
}

typedef FARPROC (WINAPI* PFN_GetProcAddress)(HMODULE, LPCSTR);
PFN_GetProcAddress orig_GetProcAddress = nullptr;
FARPROC WINAPI Detour_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    FARPROC addr = orig_GetProcAddress(hModule, lpProcName);

    if (lpProcName && !HIWORD(lpProcName)) return addr;

    if (lpProcName && strcmp(lpProcName, "vkGetDeviceProcAddr") == 0) {
        orig_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)addr;
        //printf("ASDFasdfasdf\n");
        return (FARPROC)Detour_vkGetDeviceProcAddr;
    }

    return addr;
}


void SetupVulkanHooks(HMODULE vklib) {
    if (orig_vkAllocateMemory) return;
    void* gdpa = (void*)GetProcAddress(vklib, "vkGetDeviceProcAddr");
    if (gdpa) {
        MH_STATUS status = MH_CreateHook(gdpa, &Detour_vkGetDeviceProcAddr, (LPVOID*)&orig_vkGetDeviceProcAddr);
        status = MH_EnableHook(gdpa);
        printf("[Profiler] EnableHook vkGetDeviceProcAddr: %s\n", MH_StatusToString(status));
    }

    if (vklib) {
        void* addr = (void*)GetProcAddress(vklib, "vkAllocateMemory");
        MH_STATUS status = MH_CreateHook(addr, &vkAllocateMemory_Hook, (LPVOID*)&orig_vkAllocateMemory);
        status = MH_EnableHook(addr);
        printf("[Profiler] EnableHook vkAllocateMemory: %s\n", MH_StatusToString(status));

        addr = (void*)GetProcAddress(vklib, "vkFreeMemory");
        MH_CreateHook(
            addr,
            vkFreeMemory_Hook,
            reinterpret_cast<void**>(&orig_vkFreeMemory));

        status = MH_EnableHook(addr);
        printf("[Profiler] EnableHook vkFreeMemory: %s\n", MH_StatusToString(status));
    }
    else
    {
        printf("[Profiler] No vklib\n");
    }
}
//vulkan-1.dll是动态加载的，先hook住loader
//反复测试没啥卵用，太慢了
//LoaderHook--------------------
// typedef HMODULE(WINAPI* PFN_LoadLibraryW)(LPCWSTR);
// PFN_LoadLibraryW orig_LoadLibraryW = nullptr;
// HMODULE WINAPI Detour_LoadLibraryW(LPCWSTR lpLibFileName) {
//     HMODULE hModule = orig_LoadLibraryW(lpLibFileName);
    
//     if (hModule && lpLibFileName) {
//         std::wstring name = lpLibFileName;
//         if (name.find(L"vulkan-1.dll") != std::wstring::npos) {
//             printf("[Profiler] Target ACQUIRED: vulkan-1.dll loaded via W\n");
//             SetupVulkanHooks(hModule); //hook vkAllocateMemory 第一层，尝试在load时挂载
//         }
//     }
//     return hModule;
// }

// typedef HMODULE (WINAPI* PFN_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD);
// PFN_LoadLibraryExW orig_LoadLibraryExW = nullptr;

// HMODULE WINAPI Detour_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
//     HMODULE hModule = orig_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    
//     if (hModule && lpLibFileName) {
//         if (wcsstr(lpLibFileName, L"vulkan-1.dll")) {
//             printf("[Profiler] Target ACQUIRED: vulkan-1.dll loaded via ExW\n");
//             SetupVulkanHooks(hModule);
//         }
//     }
//     return hModule;
// }

//ExitHook-----------------------
void __cdecl ProfileExitFunc() {
    static bool handled = false;
    if (handled) return;
    handled = true;

    shutdown_mimalloc_profiler();
    StopSession();
    std::cout << "Moer Engine Perfetto Exited" << std::endl;
}

typedef void (WINAPI* PFN_Exit)(int);
PFN_Exit orig_exit = nullptr;

typedef BOOL (WINAPI* PFN_TerminateProcess)(HANDLE, UINT);
PFN_TerminateProcess orig_TerminateProcess = nullptr;

void WINAPI Detour_exit(int code) {
    static std::atomic<bool> already_exited{false};
    if (!already_exited.exchange(true)) {
        printf("[Profiler] Intercepted exit(%d). Cleaning up...\n", code);
        ProfileExitFunc(); 
    }
    orig_exit(code);
}

BOOL WINAPI Detour_TerminateProcess(HANDLE hProcess, UINT uExitCode) {
    if (hProcess == GetCurrentProcess() || GetProcessId(hProcess) == GetCurrentProcessId()) {
        static std::atomic<bool> already_terminated{false};
        if (!already_terminated.exchange(true)) {
            printf("[Profiler] Intercepted self-termination. Emergency dump...\n");
            ProfileExitFunc();
        }
    }
    return orig_TerminateProcess(hProcess, uExitCode);
}

typedef void (WINAPI* PFN_ExitProcess)(UINT);
PFN_ExitProcess orig_ExitProcess = nullptr;

void WINAPI Detour_ExitProcess(UINT uExitCode) {
    static std::atomic<bool> already_run{ false };
    if (!already_run.exchange(true)) {
        printf("\n[Profiler] Intercepted ExitProcess(%u). Dumping leaks...\n", uExitCode);
        ProfileExitFunc();
    }
    orig_ExitProcess(uExitCode);
}

// Pattern Scan
bool MatchPattern(BYTE* addr, const BYTE* pattern, const char* mask)
{
    for (; *mask; ++mask, ++addr, ++pattern)
    {
        if (*mask == 'x' && *addr != *pattern)
            return false;
    }
    return true;
}

void* PatternScan(BYTE* base, size_t size, const BYTE* pattern, const char* mask)
{
    size_t patternLen = strlen(mask);

    for (size_t i = 0; i < size - patternLen; i++)
    {
        if (MatchPattern(base + i, pattern, mask))
            return base + i;
    }

    return nullptr;
}

//------------InitSymbol
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

void* GetProcAddressFromPdb(const char* mangledName, const char* readableName = nullptr) {
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

// 获取模块地址

// void GetModuleRange(HMODULE module, BYTE*& base, size_t& size)
// {
//     MODULEINFO info{};
//     GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info));

//     base = (BYTE*)info.lpBaseOfDll;
//     size = info.SizeOfImage;
// }



void install_hooks_static() {
    //MH_STATUS status = MH_Initialize();
    //printf("[Profiler] MH_Initialize: %s\n", MH_StatusToString(status));
    MH_STATUS status;
    
    HMODULE hCore = GetModuleHandleA("moer_cored.dll"); 

    if (hCore) {
        void* pMalloc = (void*)GetProcAddress(hCore, "?Malloc@Memory@@SAPEAX_K@Z");
            if (pMalloc) {
            MH_CreateHook(pMalloc, &malloc_hook, (LPVOID*)&orig_Malloc);
            status = MH_EnableHook(pMalloc);
            printf("[Profiler] EnableHook Memory:Malloc: %s\n", MH_StatusToString(status));
        }
        void* pFree1 = (void*)GetProcAddress(hCore, "?Free@Memory@@SAXPEAX@Z");
        if (pFree1) {
            MH_CreateHook(pFree1, &free_hook_1, (LPVOID*)&orig_Free1);
            status = MH_EnableHook(pFree1);
            printf("[Profiler] EnableHook Memory::Free(void*) at %p\n", pFree1);
        }
        void* pFree2 = (void*)GetProcAddress(hCore, "?Free@Memory@@SAXPEAX_K@Z");
        if (pFree2) {
            MH_CreateHook(pFree2, &free_hook_2, (LPVOID*)&orig_Free2);
            status = MH_EnableHook(pFree2);
            printf("[Profiler] EnableHook Memory::Free(void*) at %p\n", pFree2);
        }
    }

    // VkTmpAllocate没有导出符号，现在有pdb直接从里面搜索符号。release版可能需要特征码匹配或者导出符号
    //uintptr_t base = (uintptr_t)GetModuleHandleA("moer_renderd.dll");
    // LPVOID allocAddr_1 = (LPVOID)(base + 0x360E20);
    // LPVOID allocAddr_2 = (LPVOID)(base + 0x3612D0);
    // LPVOID freeAddr = (LPVOID)(base + 0x361870);

    //hCore = GetModuleHandleA("moer_renderd.dll"); 
    void* allocAddr_1 = GetProcAddressFromPdb("?Allocate@VkTmpBufferAllocator@Render@Moer@@QEAA_K_KV?$basic_string_view@DU?$char_traits@D@std@@@std@@@Z");
    //void* allocAddr_1 = (void*)GetProcAddress(hCore, "?Allocate@VkTmpBufferAllocator@Render@Moer@@QEAA_K_KV?$basic_string_view@DU?$char_traits@D@std@@@std@@@Z");
    if (allocAddr_1) {
        status = MH_CreateHook(
            allocAddr_1,
            &VkTmpAllocate_Hook_1,
            (LPVOID*)&orig_VkTmpAllocate_1
        );
        status = MH_EnableHook(allocAddr_1);
        printf("[Profiler] EnableHook VkTmpBufferAllocator::Allocate(uint64 _size, std::string_view _name) %s\n", MH_StatusToString(status));
    }
    void* allocAddr_2 = GetProcAddressFromPdb("?Allocate@VkTmpBufferAllocator@Render@Moer@@QEAA_K_KW4EVkInternalBufferUsage@23@@Z", "VkTmpBufferAllocator::Allocate");
    //void* allocAddr_2 =(void*)GetProcAddress(hCore, "?Allocate@VkTmpBufferAllocator@Render@Moer@@QEAA_K_KW4EVkInternalBufferUsage@23@@Z");
    if (allocAddr_2) {
        status = MH_CreateHook(
            allocAddr_2,
            &VkTmpAllocate_Hook_2,
            (LPVOID*)&orig_VkTmpAllocate_2
        );
        status = MH_EnableHook(allocAddr_2);
        printf("[Profiler] EnableHook VkTmpBufferAllocator::Allocate(uint64 _size, EVkInternalBufferUsage _usage) %s\n", MH_StatusToString(status));
    }
    void* freeAddr = GetProcAddressFromPdb("?DeAllocate@VkTmpBufferAllocator@Render@Moer@@QEAAX_K@Z","VkTmpBufferAllocator::DeAllocate");
   // void* freeAddr =(void*)GetProcAddress(hCore, "?DeAllocate@VkTmpBufferAllocator@Render@Moer@@QEAAX_K@Z");
    if(freeAddr)
    {
        MH_CreateHook(
            freeAddr,
            &VkTmpDeAllocate_Hook,
            (LPVOID*)&orig_VkTmpDeAllocate
        );
        status = MH_EnableHook(freeAddr);
        printf("[Profiler] EnableHook DeAllocate(uint64 _handle) %s\n", MH_StatusToString(status));
    }

    //MH_EnableHook(MH_ALL_HOOKS);
}

void LogModuleInfo() {
    if (!g_log_ofs.is_open()) return;

    g_log_ofs << "=== Module Map ===\n";
    HMODULE hMods[1024];
    HANDLE hProcess = GetCurrentProcess();
    DWORD cbNeeded;

    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        g_log_ofs << "MODULE_NAME | BaseOfDll | SizeOfImage \n";
        for (int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
            TCHAR szModName[MAX_PATH];
            MODULEINFO mi;
            if (GetModuleFileNameEx(hProcess, hMods[i], szModName, sizeof(szModName) / sizeof(TCHAR))) {
                GetModuleInformation(hProcess, hMods[i], &mi, sizeof(mi));
                g_log_ofs << "MODULE:  " << szModName << " | " << std::hex << std::showbase << mi.lpBaseOfDll << " | " << mi.SizeOfImage << std::dec << "\n";
            }
        }
    }
    g_log_ofs << "=== End Module Map ===\n";
    g_log_ofs.flush();
}

void ProfileInitDynamic()
{
    std::string log_path = get_log_path() + "\\mimalloc_profiler.log";
    g_log_ofs.open(log_path, std::ios::out | std::ios::trunc);
    if (!g_log_ofs.is_open()) {
        std::fprintf(stderr, "Failed to open log file: %s\n", log_path.c_str());
    } else {
        g_log_ofs << "=== mimalloc profiler log ===\n";
    }
    LogModuleInfo();
    // start worker
    g_worker_running.store(true);
    g_worker = std::thread(worker_thread_func);
    
    InitPerfetto();
    g_session = StartSession();
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

    if (!InitSymbolEngine()) {
        printf("[Profiler] Failed to initialize Symbol Engine, PDB hooks will fail.\n");
    }
}

// 出入口
void ProfileInitStatic()
{
    initialize_mimalloc_profiler();
    install_hooks_static();
    //std::atexit(ProfileExitFunc); //退出时机改为hook ExitProcess，此时记录数据
}

void __cdecl ProfileInitFunc()
{
    ProfileInitStatic();

    // HMODULE hUcrt = GetModuleHandleA("ucrtbase.dll");
    // if (!hUcrt) hUcrt = GetModuleHandleA("msvcrt.dll");

    // if (hUcrt) {
    //     void* pExit = (void*)GetProcAddress(hUcrt, "exit");
    //     if (pExit) {
    //         MH_CreateHook(pExit, &Detour_exit, (LPVOID*)&orig_exit);
    //         MH_EnableHook(pExit);
    //     }
    //     printf("[Profiler] System exit() hooked. ...\n");
    // }

    // void* pTerminate = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "TerminateProcess");
    // if (pTerminate) {
    //     MH_CreateHook(pTerminate, &Detour_TerminateProcess, (LPVOID*)&orig_TerminateProcess);
    //     MH_EnableHook(pTerminate);
    // }
    
    void* pExitProc = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "ExitProcess");
    if (pExitProc) {
        MH_CreateHook(pExitProc, &Detour_ExitProcess, (LPVOID*)&orig_ExitProcess);
        MH_EnableHook(pExitProc);
        printf("[Profiler] ExitProcess hook installed.\n");
    }

    // void* pLoadLib = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW");
    // if (pLoadLib) {
    //     MH_CreateHook(pLoadLib, &Detour_LoadLibraryW, (LPVOID*)&orig_LoadLibraryW);
    //     MH_EnableHook(pLoadLib);
    //     printf("[Profiler] System LoadLibraryW hooked. Waiting for Vulkan...\n");
    // }

    // void* pLoadLibExW = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryExW");
    // if (pLoadLibExW) {
    //     MH_CreateHook(pLoadLibExW, &Detour_LoadLibraryExW, (LPVOID*)&orig_LoadLibraryExW);
    //     MH_EnableHook(pLoadLibExW);
    //     printf("[Profiler] System LoadLibraryExW hooked. Waiting for Vulkan...\n");
    // }

    //目前挂载vk唯一入口
    HMODULE hVulkan = GetModuleHandleA("vulkan-1.dll");
    if (hVulkan) {
        printf("[Profiler] vulkan-1.dll Loaded before ProfileInitFunc\n");
        SetupVulkanHooks(hVulkan);
    }
    else
    {
        printf("[Profiler] No vulkan-1.dll Loaded before ProfileInitFunc\n");
    }
    ProfileInitDynamic();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        MH_Initialize();
        // MH_CreateHookApi(L"kernel32.dll", "GetProcAddress", &Detour_GetProcAddress, (LPVOID*)&orig_GetProcAddress);
        // MH_EnableHook(MH_ALL_HOOKS);
        std::thread(ProfileInitFunc).detach(); //异步玄学挂的上
        //ProfileInitFunc();
        //std::thread(ProfileInitDynamic).detach();//异步可能挂不上

    }
    return TRUE;
}

void DumpLeaks()
{
    std::lock_guard<std::mutex> lock(g_alloc_mtx);

    if (g_live_allocs.empty())
    {
        std::cout << "No leaks\n";
        return;
    }

    size_t total = 0;

    for (auto& [key, info] : g_live_allocs)
    {
        std::cout << "Leak at: " << info.ptr
                  << " size=" << info.size 
                  << " type=" << GetSourceStr(info.source)
                  << "\n";
        total += info.size;
    }

    std::cout << "Total leaked: " << total << " bytes\n";
}

void shutdown_mimalloc_profiler() {
    remove_hooks();

    // stop worker
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    
    g_worker_running.store(false, std::memory_order_release);
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

