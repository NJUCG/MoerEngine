#ifdef WITH_PROFILE
#include "perfetto.h"
#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
#elif defined(__linux__)
    #include <unistd.h>
    #include <fstream>
#endif
#include <chrono>

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

struct StackKey {
    std::vector<void*> frames; // 栈帧
    bool operator==(const StackKey& other) const {
        return frames == other.frames;
    }
};

struct StackKeyHash {
    std::size_t operator()(const StackKey& key) const {
        std::size_t h = 0;
        for (auto f : key.frames) {
            h ^= std::hash<void*>()(f) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};

// 热点统计信息
struct HotspotInfo {
    size_t total_size = 0;
    size_t alloc_count = 0;
};
static std::unordered_map<StackKey, HotspotInfo, StackKeyHash> g_hotspots;
static std::mutex g_hotspots_mtx;

struct AllocationInfo
{
    size_t size;
    std::chrono::steady_clock::time_point time;
};
struct HotspotSnapshot {
    std::string stack_str;
    size_t total_size;
    size_t alloc_count;
};

extern MemoryCounter memorycounter;
void initialize_mimalloc_profiler();
void shutdown_mimalloc_profiler();
size_t Profile_GetLiveBytes();
size_t Profile_GetPeakBytes();
size_t Profile_GetLiveAllocCount();
size_t Profile_GetAllocationCount();
std::vector<HotspotSnapshot> GetHotspots(size_t top_n = 20);

#define TRACE_FUNC(TAG) TRACE_EVENT(TAG, __FUNCTION__); memorycounter.Update()
#else
#define TRACE_FUNC(TAG) 
#endif