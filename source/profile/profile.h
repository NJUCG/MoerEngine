#pragma once

#ifdef WITH_PROFILE

#ifdef MOER_PROFILE_EXPORTS
    #define PROFILE_API __declspec(dllexport)
#else
    #define PROFILE_API __declspec(dllimport)
#endif

#include "perfetto.h"
#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
    #include <imgui.h>
#elif defined(__linux__)
    #include <unistd.h>
    #include <fstream>
#endif
#include <chrono>
#include <deque>
#include <vector>

PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("Rendering")
        .SetDescription("Timestamp for editor"),
    perfetto::Category("Memory")
        .SetDescription("CPU memory"));

void InitPerfetto();

std::unique_ptr<perfetto::TracingSession> StartSession();

void StopSession(std::unique_ptr<perfetto::TracingSession>& tracing_session);

size_t getCurrentRSS();

class MemoryCounter {
public:
    MemoryCounter(){

    }

    void Update() {
        size_t current = getCurrentRSS();
        TRACE_COUNTER("Memory", "Physical memory", current);
    }
};


static constexpr int MAX_FRAMES = 32;
static constexpr size_t RING_CAPACITY = 1 << 16;

enum class MemorySource : uint8_t {
    Editor = 0,
    Vulkan,
    VulkanTmp,
    MAX_SOURCES
};
static constexpr int SOURCE_COUNT = (int)MemorySource::MAX_SOURCES;
// 2. 定义动作类型
enum class MemoryAction : uint8_t {
    Alloc = 0,
    Free
};

struct StackKey {
    void* frames[MAX_FRAMES];
    uint16_t frame_count;
    mutable size_t cached_hash = 0;

    bool operator==(const StackKey& o) const {
        if (cached_hash != 0 && o.cached_hash != 0 && cached_hash != o.cached_hash) return false;
        return frame_count == o.frame_count && memcmp(frames, o.frames, frame_count * sizeof(void*)) == 0;
    }
};

struct StackKeyHash {
    size_t operator()(const StackKey& k) const {
        if (k.cached_hash == 0) {
            size_t h = 0;
            for (int i = 0; i < k.frame_count; ++i) 
                h ^= (size_t)k.frames[i] + 0x9e3779b9 + (h << 6) + (h >> 2);
            k.cached_hash = h;
        }
        return k.cached_hash;
    }
};

struct LiveAllocInfo {
    void *ptr;
    StackKey key;
    size_t size;
    MemorySource source;
};

struct HotspotInfo {
    size_t total_size = 0;   
    size_t alloc_count = 0;  
    MemorySource source;     
    size_t peak_size = 0; 
};

struct HotspotSnapshot {
    std::string stack_str;
    size_t total_size;
    size_t alloc_count;
    MemorySource source;
};

struct TimePoint {
    float time;
    float values[SOURCE_COUNT];
};

struct EventRecord 
{
    uint64_t ts_us;                          // timestamp
    MemorySource source;
    MemoryAction action;
    USHORT frame_count;                      // number of frames captured
    size_t size;                             // allocation size
    void* ptr;                               // address returned
    void* frames[MAX_FRAMES];

    //VkTmp
    uint32_t usage; 
    char name[32];
};
struct SourceUIConfig {
    MemorySource source;
    const char* label;
    ImVec4 color;
};

extern PROFILE_API SourceUIConfig g_UIConfigs[3];
extern size_t g_UIConfigCount;

struct LiveMetrics {
    std::atomic<size_t> bytes[(size_t)MemorySource::MAX_SOURCES];
    std::atomic<size_t> peaks[(size_t)MemorySource::MAX_SOURCES];
    LiveMetrics() {
        for(int i=0; i<(int)MemorySource::MAX_SOURCES; ++i) {
            bytes[i] = 0; peaks[i] = 0;
        }
    }
};

void Profile_TickSample();

size_t Profile_GetBytesBySource(MemorySource source);
PROFILE_API size_t Profile_GetPeakBytesBySource(MemorySource source);
std::vector<HotspotSnapshot> GetHotspots(size_t top_n, MemorySource filterSource);

extern PROFILE_API std::deque<TimePoint> g_history_data;
extern PROFILE_API std::mutex g_history_mtx;

void initialize_mimalloc_profiler();
void shutdown_mimalloc_profiler();

struct SymbolResult {
    std::string name;
    uintptr_t address;
};
void ProfileInitStatic();
void ProfileInitFunc();
#define TRACE_FUNC(TAG) TRACE_EVENT(TAG, __FUNCTION__); memorycounter.Update()
#else
#define TRACE_FUNC(TAG) 
#endif