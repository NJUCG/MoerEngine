#include "Profile.h"
#include "ProfileUtils.h"
#include "VulkanProfiler.h"
#include "MemoryProfiler.h"
#include "MinHook.h"

#pragma comment(lib,"psapi.lib")
#pragma comment(lib, "dbghelp.lib")

// mimalloc_profile
static std::chrono::steady_clock::time_point g_start_time = std::chrono::steady_clock::now();
static constexpr size_t LOG_BATCH_FLUSH = 0;

moodycamel::ConcurrentQueue<EventRecord>* g_rings[(int)MemorySource::MAX_SOURCES] = { nullptr };
std::atomic<uint64_t> g_global_sequence{0};

std::priority_queue<EventRecord, std::vector<EventRecord>, RecordComparator> g_reorder_buffer;
uint64_t g_expected_sequence = 0;
const size_t MAX_REORDER_WINDOW = 4096;

static std::atomic<bool> g_dbghelp_inited(false);

static std::thread g_worker;
static std::atomic<bool> g_worker_running(false);

std::string log_path = get_log_path() + "\\profiler.log";
static std::ofstream g_log_ofs;
std::mutex g_log_mtx;

//hook的原函数实现可能有递归嵌套，既然没有hook mimalloc，先停用
//thread_local bool g_in_hook = false;

PROFILE_API SourceUIConfig g_UIConfigs[3] = {
    { MemorySource::Editor,    "System Editor", ImVec4(0.4f, 0.8f, 0.4f, 1.0f) },
    { MemorySource::Vulkan,    "Vulkan Static", ImVec4(0.2f, 0.6f, 1.0f, 1.0f) },
    { MemorySource::VulkanTmp, "Vulkan Temp",   ImVec4(1.0f, 0.7f, 0.2f, 1.0f) }
};

std::unordered_map<void*,    LiveAllocInfo> g_live_allocs_editor;
std::unordered_map<void*,    LiveAllocInfo> g_live_allocs_vulkan;
//VkTmp返回的不是指针，不统计hotspot
std::unordered_map<uint64_t, size_t>        g_live_allocs_vktmp;

std::unordered_map<StackKey, HotspotInfo, StackKeyHash> g_hotspots[(int)MemorySource::MAX_SOURCES];
std::mutex g_hotspots_mtx[(int)MemorySource::MAX_SOURCES];

PROFILE_API std::deque<TimePoint> g_history_data;
PROFILE_API std::mutex g_history_mtx;
uint64_t current_frame = 0;

LiveMetrics g_metrics;

PROFILE_API PassHistory g_pass_history[MAX_GPU_PASSES];
PROFILE_API int         g_pass_history_count = 0;
PROFILE_API std::mutex  g_pass_history_mtx;

PROFILE_API size_t Profile_GetPeakBytesBySource(MemorySource source) {
    return g_metrics.peaks[(size_t)source].load();
}

PROFILE_API size_t Profile_GetBytesBySource(MemorySource source) {
    return g_metrics.bytes[(size_t)source].load();
}

//类似UDP，新增缓冲区
static void worker_thread_func() {
    const size_t BATCH_SIZE = 512;
    EventRecord batch[BATCH_SIZE];
    
    while (true) {
        bool got_any = false;

        for (int i = 0; i < (int)MemorySource::MAX_SOURCES; i++) {
            if (!g_rings[i]) continue;
            size_t count = g_rings[i]->try_dequeue_bulk(batch, BATCH_SIZE);
            if (count == 0) continue;
            
            got_any = true;
            for (size_t j = 0; j < count; j++) {
                g_reorder_buffer.push(batch[j]);
            }
        }

        while (!g_reorder_buffer.empty()) {
            const EventRecord& top = g_reorder_buffer.top();

            // 检查序号是否连续
            bool is_next = (top.sequence == g_expected_sequence);
            bool is_timeout = (g_reorder_buffer.size() > MAX_REORDER_WINDOW); 

            if (is_next || is_timeout) {
                if (is_timeout && !is_next) {
                    // printf("[Profiler] Sequence gap detected! Expected %llu, got %llu\n", 
                    //        g_expected_sequence, top.sequence);
                    g_expected_sequence = top.sequence;
                }

                EventRecord rec = top;
                g_reorder_buffer.pop();
                
                process_record_logical(rec); 

                g_expected_sequence++;
            } else {
                break;
            }
        }

        if (!got_any) {
            if (!g_worker_running.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}
void process_record_logical(const EventRecord& rec) {
    const uint8_t s_idx = static_cast<uint8_t>(rec.source);
    if (s_idx >= (uint8_t)MemorySource::MAX_SOURCES) return;

    switch (rec.source) {

        case MemorySource::Editor:
        case MemorySource::Vulkan: {
            auto& live_map = (rec.source == MemorySource::Editor)
                ? g_live_allocs_editor
                : g_live_allocs_vulkan;

            if (rec.ptr == nullptr) break;

            if (rec.action == MemoryAction::Alloc) {
                LiveAllocInfo& live  = live_map[rec.ptr];
                live.size            = rec.size;
                live.source          = rec.source;
                live.ptr             = rec.ptr;
                live.key.frame_count = rec.frame_count;
                memcpy(live.key.frames, rec.frames, rec.frame_count * sizeof(void*));
                StackKeyHash hasher;
                live.key.cached_hash = hasher(live.key);

                // 更新metrics
                size_t cur = g_metrics.bytes[s_idx].fetch_add(rec.size, std::memory_order_relaxed) + rec.size;
                size_t old = g_metrics.peaks[s_idx].load(std::memory_order_relaxed);
                while (cur > old &&
                    !g_metrics.peaks[s_idx].compare_exchange_weak(old, cur, std::memory_order_relaxed));
                // 更新hotspot
                {
                    std::lock_guard<std::mutex> lock(g_hotspots_mtx[s_idx]);
                    auto& h = g_hotspots[s_idx][live.key];
                    h.total_size += rec.size;
                    h.alloc_count++;
                    h.source = rec.source;
                }
            } else { // Free
                auto it = live_map.find(rec.ptr);
                if (it != live_map.end()) {
                    LiveAllocInfo& live  = it->second;
                    if (g_metrics.bytes[s_idx] >= live.size)
                        g_metrics.bytes[s_idx].fetch_sub(live.size, std::memory_order_relaxed);
                    
                    // 更新hotspot
                    {
                        std::lock_guard<std::mutex> lock(g_hotspots_mtx[s_idx]);
                        auto h_it = g_hotspots[s_idx].find(live.key);
                        if (h_it != g_hotspots[s_idx].end() && h_it->second.total_size >= live.size)
                        {
                            h_it->second.total_size -= live.size;
                            h_it->second.alloc_count--;
                            if(h_it->second.alloc_count <= 0)
                            {
                                g_hotspots[s_idx].erase(h_it);
                            }
                        }
                    }

                    live_map.erase(it);
                }
                // else {
                //     // 新加：统计找不到的次数
                //     printf("----------------------------------------------------\n");
                //     printf("[Profiler] Free without Alloc: ptr=%p source=%s\n",
                //         rec.ptr, GetSourceStr(rec.source));
                //         for (int i = 0; i < rec.frame_count; i++) {
                //             auto sym = symbolicate_address(rec.frames[i]);
                //             printf("    #%d %s\n", i, sym.c_str());
                //         }
                //     //printf("[Profiler] RingBuffer dropped: %llu\n", g_rings[(int)rec.source]->dropped.load());
                //     printf("----------------------------------------------------\n");
                // }
            }
            break;
        }

        case MemorySource::VulkanTmp: {
            // handle不是真实指针，用uint64_t作key，独立map
            uint64_t handle = (uint64_t)rec.ptr;

            if (rec.action == MemoryAction::Alloc) {
                g_live_allocs_vktmp[handle] = rec.size;

                size_t cur = g_metrics.bytes[s_idx].fetch_add(rec.size, std::memory_order_relaxed) + rec.size;
                size_t old = g_metrics.peaks[s_idx].load(std::memory_order_relaxed);
                while (cur > old &&
                    !g_metrics.peaks[s_idx].compare_exchange_weak(old, cur, std::memory_order_relaxed));
            } else { // Free
                auto it = g_live_allocs_vktmp.find(handle);
                if (it != g_live_allocs_vktmp.end()) {
                    size_t sz = it->second;
                    if (g_metrics.bytes[s_idx] >= sz)
                        g_metrics.bytes[s_idx].fetch_sub(sz, std::memory_order_relaxed);
                    g_live_allocs_vktmp.erase(it);
                }
            }
            break;
        }

        default: break;
    }

    // 暂时跳过写文件
    return;
    if (rec.source == MemorySource::Editor) return;

    char header[512];
    uint64_t us     = rec.ts_us;
    time_t   secs   = (time_t)(us / 1000000ULL);
    uint32_t usec   = (uint32_t)(us % 1000000ULL);
    tm tmv;
    gmtime_s(&tmv, &secs);

    size_t log_size = 0;
    if (rec.source == MemorySource::Vulkan) {
        // Vulkan用真实map查size
        auto it = g_live_allocs_vulkan.find(rec.ptr);
        log_size = (it != g_live_allocs_vulkan.end()) ? it->second.size : 0;
    } else if (rec.source == MemorySource::VulkanTmp) {
        auto it = g_live_allocs_vktmp.find((uint64_t)rec.ptr);
        log_size = (it != g_live_allocs_vktmp.end()) ? it->second : rec.size;
    }

    int offset = sprintf_s(header, sizeof(header),
        "[%04d-%02d-%02d %02d:%02d:%02d.%06u] [%s:%s] ptr=0x%p size=%zu",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec, usec,
        GetSourceStr(rec.source), GetActionStr(rec.action),
        rec.ptr, log_size);

    if (rec.source == MemorySource::VulkanTmp && rec.action == MemoryAction::Alloc) {
        if (rec.usage != 0xFFFFFFFF)
            sprintf_s(header + offset, sizeof(header) - offset, " usage=%s\n", GetVkUsageName(rec.usage));
        else
            sprintf_s(header + offset, sizeof(header) - offset, " name=%s\n", rec.name);
    } else {
        sprintf_s(header + offset, sizeof(header) - offset, "\n");
    }

    // g_log_ofs << header;
    // for (uint16_t j = 0; j < rec.frame_count; ++j)
    //     g_log_ofs << "    #" << j << " " << rec.frames[j] << "\n";
    // g_log_ofs << "----------------------------------------\n";

    
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

void WriteHotspots(bool ifDump) {
    std::lock_guard<std::mutex> lock(g_log_mtx);

    g_log_ofs.close();
    g_log_ofs.open(log_path, std::ios::out | std::ios::trunc);
    if (!g_log_ofs.is_open()) {
        std::fprintf(stderr, "Failed to open log file: %s\n", log_path.c_str());
    } else {
        g_log_ofs << "=== profiler log ===\n";
    }

    LogModuleInfo();

    for (int s = 0; s < (int)MemorySource::MAX_SOURCES; ++s) 
    {
        if(s == (int)MemorySource::Editor)
        {
            g_log_ofs << "=== Editor Memory ===\n";
        }
        else if(s == (int)MemorySource::Vulkan)
        {
            g_log_ofs << "=== Vulkan Memory ===\n";
        }
        else
        {
            break;
        }
        std::vector<HotspotSnapshot> all_snapshots;
        const size_t top_n = 50; // 设定前 N 个热点

        std::lock_guard<std::mutex> hs_lock(g_hotspots_mtx[s]);
        
        for (auto& pair : g_hotspots[s]) {
            const StackKey& key = pair.first;
            const HotspotInfo& info = pair.second;

            if (info.total_size == 0) continue;

            HotspotSnapshot snap;
            snap.total_size = info.total_size;
            snap.alloc_count = info.alloc_count;
            snap.source = info.source;
            snap.frame_count = key.frame_count;
            memcpy(snap.frames, key.frames, key.frame_count * sizeof(void*));

            all_snapshots.push_back(std::move(snap));
        }

        std::sort(all_snapshots.begin(), all_snapshots.end(), 
            [](const HotspotSnapshot& a, const HotspotSnapshot& b) {
                return a.total_size > b.total_size;
        });

        size_t write_count = (std::min)(all_snapshots.size(), top_n);
        for (size_t i = 0; i < write_count; ++i) {
            const auto& snap = all_snapshots[i];
            g_log_ofs << "-------------------------------------------\n";
            g_log_ofs << "Rank: " << i + 1 << "\n";
            g_log_ofs << "Source: " << GetSourceStr(snap.source) << "\n";
            g_log_ofs << "Total Size: " << snap.total_size << " bytes\n";
            g_log_ofs << "Alloc Count: " << snap.alloc_count << "\n";
            
            for (int f = 0; f < snap.frame_count; ++f) {
                g_log_ofs << "    #" << f << " ";
                
                if (ifDump) {
                    auto sym = symbolicate_address(snap.frames[f]);
                    g_log_ofs << sym << " ";
                }
                
                g_log_ofs << "[0x" << std::hex << std::uppercase << (uint64_t)snap.frames[f] << std::dec << "]\n";
            }
            g_log_ofs << "-------------------------------------------\n";
        }
        all_snapshots.clear();
    }

    g_log_ofs << "=== End of Hotspots ===\n";
    g_log_ofs.flush();
}

#define DEAD_THRESHOLD 30
void UpdatePassHistory(const CBState& snap) {
    for (int i = 0; i < snap.pass_count; i++) {
        const PassProfile& p = snap.passes[i];
        if (!p.gpu_valid) continue;
        PassHistory* slot = nullptr;
        for (int j = 0; j < g_pass_history_count; j++) {
            if (strcmp(g_pass_history[j].name, p.name) == 0) {
                slot = &g_pass_history[j];
                break;
            }
        }
        if (!slot && g_pass_history_count < MAX_GPU_PASSES) {
            slot = &g_pass_history[g_pass_history_count++];
            strncpy(slot->name, p.name, 63);
            strncpy(slot->parent_name, p.parent_name, 63);
            slot->depth     = p.depth;
            slot->write_idx = 0;
            slot->avg_ms    = 0;
            slot->max_ms    = 0;
            slot->active         = true;
            slot->last_seen_frame = current_frame;
            memset(slot->samples, 0, sizeof(slot->samples));
        }

        if (!slot) continue;

        slot->samples[slot->write_idx] = p.gpu_render_ms;
        slot->write_idx = (slot->write_idx + 1) % 60;
        slot->last_seen_frame = current_frame;

        float sum = 0, mx = 0;
        int cnt = 0;
        for (int j = 0; j < 60; j++) 
        {
            if(slot->samples[j] > 0)
            {
                sum += slot->samples[j];
                cnt++;
            }
            
            if (slot->samples[j] > mx) mx = slot->samples[j];
        }
        if(cnt != 0)
        {
            slot->avg_ms = sum / cnt;
        }
        slot->max_ms = mx;
    }
    for (int i = 0; i < g_pass_history_count; i++) {
        //printf("[profile]%lld %lld\n", current_frame, g_pass_history[i].last_seen_frame);
        g_pass_history[i].active = 
            (current_frame < g_pass_history[i].last_seen_frame || current_frame - g_pass_history[i].last_seen_frame <= DEAD_THRESHOLD); //30帧pass没有更新，设置为不可见
    }
}

//LoaderHook--------------------
static HMODULE(WINAPI* orig_LoadLibraryA)(LPCSTR) = nullptr;
static HMODULE(WINAPI* orig_LoadLibraryW)(LPCWSTR) = nullptr;
static HMODULE(WINAPI* orig_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD) = nullptr;

HMODULE WINAPI Detour_LoadLibraryA(LPCSTR lpLibFileName) {
    HMODULE hMod = orig_LoadLibraryA(lpLibFileName);
    if (hMod && lpLibFileName) {
        if (strstr(lpLibFileName, "vulkan-1")) {
            printf("[Profiler] vulkan-1.dll loaded via LoadLibraryA\n");
            SetupVulkanHooks(hMod);
        }
    }
    return hMod;
}

HMODULE WINAPI Detour_LoadLibraryW(LPCWSTR lpLibFileName) {
    HMODULE hMod = orig_LoadLibraryW(lpLibFileName);
    if (hMod && lpLibFileName) {
        if (wcsstr(lpLibFileName, L"vulkan-1")) {
            printf("[Profiler] vulkan-1.dll loaded via LoadLibraryW\n");
            SetupVulkanHooks(hMod);
        }
    }
    return hMod;
}

HMODULE WINAPI Detour_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    HMODULE hMod = orig_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (hMod && lpLibFileName) {
        if (wcsstr(lpLibFileName, L"vulkan-1")) {
            printf("[Profiler] vulkan-1.dll loaded via LoadLibraryExW\n");
            SetupVulkanHooks(hMod);
        }
    }
    return hMod;
}

void remove_hooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

//ExitHook-----------------------
void ProfileExitFunc() {
    
    static std::atomic<bool> s_saved = false;
    if (s_saved.exchange(true)) return;

    remove_hooks();

    // stop worker
    //std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    
    g_worker_running.store(false, std::memory_order_release);
    if (g_worker.joinable()) g_worker.join();

    // cleanup
    if (g_log_ofs.is_open()) {
        g_log_ofs.flush();
        g_log_ofs.close();
    }

    if (g_dbghelp_inited.load()) {
        SymCleanup(GetCurrentProcess());
        g_dbghelp_inited.store(false);
    }

    for (int i = 0; i < (int)MemorySource::MAX_SOURCES; i++) {
        if (g_rings[i]) {
            delete g_rings[i];
            g_rings[i] = nullptr;
        }
    }
    
    FlameProfiler::Get().Save(get_log_path() + "/frame_trace.json");
    FlameProfiler::Get().Clear();
    
    //DumpLeaks(); //不分析泄漏，没卵用
    
    std::cout<<"[profiler] Shutdown"<<std::endl;
}
// 写文件可以挂载exit时清理，要写文件再hook exit纯作死，改成hook editor的shutdown

// typedef void (WINAPI* PFN_ExitProcess)(UINT);
// PFN_ExitProcess orig_ExitProcess = nullptr;

// void WINAPI Detour_ExitProcess(UINT uExitCode) {
//     static std::atomic<bool> already_run{ false };
//     if (already_run.exchange(true))
//     {
//         orig_ExitProcess(uExitCode); //第一次退出
//     }

//     ProfileExitFunc();
//     orig_ExitProcess(uExitCode); //第二次退出
// }

typedef void (*PFN_EditorShutDown)(void*);
PFN_EditorShutDown orig_EditorShutDown = nullptr;

void __fastcall Detour_EditorShutDown(void* thisPtr) {
    orig_EditorShutDown(thisPtr);
    ProfileExitFunc();
    fflush(stdout);
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


// 获取模块地址

// void GetModuleRange(HMODULE module, BYTE*& base, size_t& size)
// {
//     MODULEINFO info{};
//     GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info));

//     base = (BYTE*)info.lpBaseOfDll;
//     size = info.SizeOfImage;
// }

void ProfileInitDynamic()
{
    // start worker
    g_worker_running.store(true);
    g_worker = std::thread(worker_thread_func);
}

void install_hooks_static() {
    MH_STATUS status = MH_Initialize();
    void* pLoadLibA = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    MH_CreateHook(pLoadLibA, &Detour_LoadLibraryA, (LPVOID*)&orig_LoadLibraryA);
    MH_EnableHook(pLoadLibA);

    void* pLoadLibW = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW");
    MH_CreateHook(pLoadLibW, &Detour_LoadLibraryW, (LPVOID*)&orig_LoadLibraryW);
    MH_EnableHook(pLoadLibW);

    void* pLoadLibExW = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryExW");
    MH_CreateHook(pLoadLibExW, &Detour_LoadLibraryExW, (LPVOID*)&orig_LoadLibraryExW);
    MH_EnableHook(pLoadLibExW);

    SetupMemoryHooks();

    void* addr = GetProcAddressFromPdb("?ShutDown@Editor@Moer@@QEAAXXZ");
    if (addr) {
        status = MH_CreateHook(addr, &Detour_EditorShutDown, (LPVOID*)&orig_EditorShutDown);
        status = MH_EnableHook(addr);
        printf("[Profiler] EnableHook Editor::ShutDown %s\n", MH_StatusToString(status));
    }

    // VkTmpAllocate没有导出符号，现在有pdb直接从里面搜索符号。release版可能需要特征码匹配或者导出符号
    //uintptr_t base = (uintptr_t)GetModuleHandleA("moer_renderd.dll");
    // LPVOID allocAddr_1 = (LPVOID)(base + 0x360E20);
    // LPVOID allocAddr_2 = (LPVOID)(base + 0x3612D0);
    // LPVOID freeAddr = (LPVOID)(base + 0x361870);

    //hCore = GetModuleHandleA("moer_renderd.dll"); 
    
    //MH_EnableHook(MH_ALL_HOOKS);
        // void* pExitProc = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "ExitProcess");
    // if (pExitProc) {
    //     MH_CreateHook(pExitProc, &Detour_ExitProcess, (LPVOID*)&orig_ExitProcess);
    //     MH_EnableHook(pExitProc);
    //     printf("[Profiler] ExitProcess hook installed.\n");
    // }
    //std::atexit(ProfileExitFunc); //退出时机改为hook ExitProcess，此时记录数据
}
void ProfileInitFunc()
{
    static std::atomic<bool> s_init = false;
    if (s_init.exchange(true)) return;
    g_rings[(int)MemorySource::Editor]    = new moodycamel::ConcurrentQueue<EventRecord>(1 << 20); // Editor内存操作最频繁
    g_rings[(int)MemorySource::Vulkan]    = new moodycamel::ConcurrentQueue<EventRecord>(1 << 16); // Vulkan分配相对少
    g_rings[(int)MemorySource::VulkanTmp] = new moodycamel::ConcurrentQueue<EventRecord>(1 << 16);

    if (!InitSymbolEngine()) {
        printf("[Profiler] Failed to initialize Symbol Engine, PDB hooks will fail.\n");
    }

    install_hooks_static();

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

        //std::thread(ProfileInitFunc).detach(); //异步玄学挂的上
        ProfileInitFunc(); //同步玄学挂的上
        //std::thread(ProfileInitDynamic).detach();//异步可能挂不上

    }
    return TRUE;
}

// void DumpLeaks() {
//     std::cout << "=== Editor Leaks ===\n";
//     for (auto& [ptr, info] : g_live_allocs_editor) {
//         std::cout << "ptr=" << ptr << " size=" << info.size << "\n";
//     }
//     std::cout << "=== Vulkan Leaks ===\n";
//     for (auto& [ptr, info] : g_live_allocs_vulkan) {
//         std::cout << "ptr=" << ptr << " size=" << info.size << "\n";
//     }
//     std::cout << "=== VulkanTmp Live ===\n";
//     for (auto& [handle, size] : g_live_allocs_vktmp) {
//         std::cout << "handle=" << handle << " size=" << size << "\n";
//     }
// }
