#pragma once

#ifdef MOER_PROFILE_EXPORTS
    #define PROFILE_API __declspec(dllexport)
#else
    #define PROFILE_API __declspec(dllimport)
#endif

#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
    #include <imgui.h>
    #include <dbghelp.h>
#elif defined(__linux__)
    #include <unistd.h>
    #include <fstream>
#endif
#include "concurrentqueue.h"
#include "misc/Types.h"
#include "vulkan/vulkan.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static constexpr int    MAX_FRAMES           = 64;
static constexpr size_t RING_CAPACITY        = 1 << 20;
static constexpr int    MAX_GPU_PASSES       = 128;
static constexpr int    MAX_FRAMES_IN_FLIGHT = 3;       //考虑到性能，稳妥起见和swapchain的帧数一致，其实用不上
static constexpr int    MAX_LABEL_DEPTH      = 32;

struct FlameEvent {
    const char* name;
    long long start_us;
    long long duration_us;
    Moer::uint32 thread_id;
};

class FlameProfiler {
public:
    static FlameProfiler& Get() {
        static FlameProfiler instance;
        return instance;
    }

    void Begin(const char* name);

    void End();

    void Save(const std::string& path);

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_events.clear();
    }

private:
    static std::vector<std::pair<const char*, long long>>& GetStack() {
        thread_local std::vector<std::pair<const char*, long long>> stack;
        return stack;
    }

    static long long NowUs() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }

    std::mutex m_mutex;
    std::vector<FlameEvent> m_events;
};

struct FlameScope {
    FlameScope(const char* name) { FlameProfiler::Get().Begin(name); }
    ~FlameScope() { FlameProfiler::Get().End(); }
};

#define FLAME_SCOPE() FlameScope _flame_##__LINE__(__FUNCTION__);
#define FLAME_FUNC(name) FlameScope _flame_##__LINE__(name);

//------------------------------------
struct StackKey {
    void* frames[MAX_FRAMES];
    Moer::uint16   frame_count;
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

//------------------------------------
enum class MemorySource : Moer::uint8 {
    Editor = 0,
    Vulkan,
    VulkanTmp,
    MAX_SOURCES
};
static constexpr int SOURCE_COUNT = (int)MemorySource::MAX_SOURCES;

const char* GetSourceStr(MemorySource s);

enum class MemoryAction : Moer::uint8 {
    Alloc = 0,
    Free
};
const char* GetActionStr(MemoryAction a);

enum class EVkInternalBufferUsage {
    Upload = 0,
    Readback,
    Scratch,
    ShaderBuffer,
    ShaderBuffer_Constant,
    Count
};

const char* GetVkUsageName(Moer::uint32 usage);

//-----------------------------------UI
struct SourceUIConfig {
    MemorySource source;
    const char* label;
    ImVec4 color;
};

struct LiveMetrics {
    std::atomic<size_t> bytes[(size_t)MemorySource::MAX_SOURCES];
    std::atomic<size_t> peaks[(size_t)MemorySource::MAX_SOURCES];
    LiveMetrics() {
        for(int i=0; i<(int)MemorySource::MAX_SOURCES; ++i) {
            bytes[i] = 0; peaks[i] = 0;
        }
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
    void* frames[MAX_FRAMES];
    USHORT frame_count;
    size_t total_size;
    size_t alloc_count;
    MemorySource source;
};

struct TimePoint {
    float time;
    float values[SOURCE_COUNT];
};

struct SymbolResult {
    std::string name;
    uintptr_t address;
};

//-----------------------------------Hook
struct EventRecord 
{
    Moer::uint64 ts_us; // timestamp
    MemorySource source;
    MemoryAction action;
    USHORT frame_count;                      // number of frames captured
    size_t size;                             // allocation size
    void* ptr;                               // address returned
    void* frames[MAX_FRAMES];
    Moer::uint64 sequence;
    //VkTmp
    Moer::uint32 usage;
    char name[32];
};

struct RawPassTimer {
    Moer::uint64 start = 0;
    Moer::uint64 delta = 0;

    void Start() {
#if defined(_WIN32)
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        start = (Moer::uint64)li.QuadPart;
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        start = (Moer::uint64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
    }

    void Stop() {
#if defined(_WIN32)
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        delta = (Moer::uint64)li.QuadPart - start;
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        Moer::uint64 now = (Moer::uint64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        delta = now - start;
#endif
    }

    Moer::uint64 GetDelta() const {
        return delta;
    }
};


struct PassProfile {
    char     name[64]        = {};
    char     parent_name[64] = {};
    float    gpu_render_ms   = 0.0f;
    Moer::uint64 cpu_record_clock = 0;
    int      depth           = 0;
    int      parent_idx      = -1;
    int      query_idx       = 0;
    bool     gpu_valid       = false;
};

struct CBState {
    PassProfile  passes[MAX_GPU_PASSES] = {};
    int          pass_count             = 0;

    int          label_stack[MAX_LABEL_DEPTH] = {};
    int          stack_top                    = 0;

    VkQueryPool  pool       = VK_NULL_HANDLE;
    bool         pool_valid = false;
};

struct SubmitRecord {
    Moer::uint64           signal_value = 0;
    std::vector<CBState>   cb_snapshots; // 从 g_cb_states move 出来的数据
};

struct QueueState {
    VkSemaphore              timeline_sem   = VK_NULL_HANDLE;
    Moer::uint64             timeline_value = 0; // 单调递增，仅QueueSubmit2使用，Editor侧已经加锁，参见VulkanQueue.h:43
    std::deque<SubmitRecord> pending;            // 已提交未读取的历史submit2
};

//-----------------------------------------UI
struct PassHistory {
    char  name[64];
    char  parent_name[64];
    int   depth;           // 嵌套深度，用于缩进显示
    float samples[60] = {};     // 最近60帧GPU耗时
    int   write_idx;       // 环形写入位置
    float avg_ms = 0;
    float max_ms = 0;
    bool         active;
    Moer::uint64 last_seen_frame;
};
