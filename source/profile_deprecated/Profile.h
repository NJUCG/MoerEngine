#pragma once

#if WITH_PROFILE
#include "ProfileTypes.h"

extern PROFILE_API SourceUIConfig g_UIConfigs[3];
extern size_t g_UIConfigCount;

extern PROFILE_API std::deque<TimePoint> g_history_data;
extern PROFILE_API std::mutex g_history_mtx;

//--------------------------

extern PROFILE_API PassHistory g_pass_history[];
extern PROFILE_API int         g_pass_history_count;
extern PROFILE_API std::mutex  g_pass_history_mtx;
extern uint64_t current_frame;

extern moodycamel::ConcurrentQueue<EventRecord>* g_rings[];
extern std::atomic<uint64_t> g_global_sequence;

extern std::unordered_map<void*,    LiveAllocInfo> g_live_allocs_editor;
extern std::unordered_map<void*,    LiveAllocInfo> g_live_allocs_vulkan;
extern std::unordered_map<uint64_t, size_t>        g_live_allocs_vktmp;
// extern std::unordered_map<StackKey, HotspotInfo, StackKeyHash> g_hotspots;
// extern std::mutex g_hotspots_mtx;

PROFILE_API size_t Profile_GetPeakBytesBySource(MemorySource source);
PROFILE_API size_t Profile_GetBytesBySource(MemorySource source);
void Profile_TickSample();
//std::vector<HotspotSnapshot> GetHotspots(size_t top_n, MemorySource filterSource);
void WriteHotspots(bool ifDump);
void UpdatePassHistory(const CBState& snap);

void ProfileInitFunc();
void remove_hooks();
HMODULE WINAPI Detour_LoadLibraryA(LPCSTR lpLibFileName);
HMODULE WINAPI Detour_LoadLibraryW(LPCWSTR lpLibFileName);
HMODULE WINAPI Detour_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
void ProfileExitFunc();
void __fastcall Detour_EditorShutDown(void* thisPtr);
bool MatchPattern(BYTE* addr, const BYTE* pattern, const char* mask);
void* PatternScan(BYTE* base, size_t size, const BYTE* pattern, const char* mask);
void install_hooks_static();
void LogModuleInfo();
void ProfileInitDynamic();
void ProfileInitFunc();
void process_record_logical(const EventRecord& rec);

struct RecordComparator {
    bool operator()(const EventRecord& a, const EventRecord& b) {
        return a.sequence > b.sequence;
    }
};

extern std::priority_queue<EventRecord, std::vector<EventRecord>, RecordComparator> g_reorder_buffer;
extern uint64_t g_expected_sequence;
extern const size_t MAX_REORDER_WINDOW;
//void DumpLeaks();
#endif