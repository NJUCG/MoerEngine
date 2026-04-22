//目前Editor里只使用了Malloc，MallocN，MallocAligned

#include "Profile.h"
#include "MemoryProfiler.h"
#include "ProfileUtils.h"
#include "MinHook.h"

static std::mutex g_alloc_mtx;

//typedef void* (*malloc_t)(size_t);
//typedef void  (*free_t)(void*);
typedef void* (*Malloc)(size_t);
typedef void* (*Calloc)(size_t count, size_t size);
typedef void* (*ReAlloc)(void* p, size_t newsize);
typedef void (*Free1)(void* p);
typedef void (*Free2)(void* p, size_t size);
typedef void* (*MallocAligned)(size_t size, size_t alignment);
typedef void* (*MallocN)(size_t count, size_t size);
typedef void* (*CallocAligned)(size_t count, size_t size, size_t alignment);

//malloc_t orig_malloc = nullptr;
//free_t   orig_free   = nullptr;
static Malloc orig_Malloc = nullptr;
static Calloc   orig_Calloc   = nullptr;
static ReAlloc  orig_ReAlloc  = nullptr;
static Free1 orig_Free1 = nullptr;
static Free2 orig_Free2 = nullptr;
static MallocAligned orig_MallocAligned = nullptr;
static MallocN  orig_MallocN  = nullptr;
static CallocAligned orig_CallocAligned = nullptr;

void* Detour_Malloc(size_t size) {
    // if (g_in_hook) {
    //     return orig_Malloc(size);
    // }
    // g_in_hook = true;

    void* p = orig_Malloc(size);
    auto* ring = g_rings[(int)MemorySource::Editor];
    if (ring && p) { 
        static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::Editor]);

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Editor;
        rec.action = MemoryAction::Alloc;
        rec.size = size;
        rec.ptr = p;
        rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);
        
        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

        ring->enqueue(token, rec);
    }

    //g_in_hook = false;
    return p;
}

void* Detour_Calloc(size_t count, size_t size) {
    void* p = orig_Calloc(count, size);
    auto* ring = g_rings[(int)MemorySource::Editor];
    if (ring && p) {
        static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::Editor]);

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Editor;
        rec.action = MemoryAction::Alloc;
        rec.size = count * size;
        rec.ptr = p;
        rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);

        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

        ring->enqueue(token, rec);
    }
    return p;
}

void* Detour_ReAlloc(void* p, size_t newsize) {
    if (!p)
    {
        return orig_ReAlloc(p, newsize);
    }
    if (p) {
        auto* ring = g_rings[(int)MemorySource::Editor];
        if (ring) {
            static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::Editor]);

            EventRecord rec;
            rec.ts_us = now_us();
            rec.action = MemoryAction::Free;
            rec.ptr = p;
            rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);

            ring->enqueue(token, rec);
        }
    }

    // 2. 调用原 realloc
    void* new_p = orig_ReAlloc(p, newsize);

    // 3. 记录新分配的地址
    if (new_p && newsize > 0) {
        auto* ring = g_rings[(int)MemorySource::Editor];
        if (ring) {
            static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::Editor]);

            EventRecord rec;
            rec.ts_us = now_us();
            rec.action = MemoryAction::Alloc;
            rec.size = newsize;
            rec.ptr = new_p;
            rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);

            capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

            ring->enqueue(token, rec);
        }
    }
    return new_p;
}

void Detour_Free1(void* p)
{
    // if (g_in_hook)
    // {
    //     orig_Free1(p);
    //     return;
    // }

    // g_in_hook = true;
    if (!p) {
        orig_Free1(p);
        return;
    }
    auto* ring = g_rings[(int)MemorySource::Editor];
    if (ring) { 
        static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::Editor]);

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Editor;
        rec.action = MemoryAction::Free;
        rec.size = 0;
        rec.ptr = p;
        rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);
        
        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

        ring->enqueue(token, rec);
    }

    orig_Free1(p);

    //g_in_hook = false;
}
void Detour_Free2(void* p, size_t size)
{
    // if (g_in_hook)
    // {
    //     orig_Free2(p, size);
    //     return;
    // }

    // g_in_hook = true;
    if (!p) {
        orig_Free2(p, size);
        return;
    }
    auto* ring = g_rings[(int)MemorySource::Editor];
    if (ring) { 
        static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::Editor]);

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Editor;
        rec.action = MemoryAction::Free;
        rec.size = size;
        rec.ptr = p;
        rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);
        
        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

        ring->enqueue(token, rec);
    }

    orig_Free2(p, size);

    //g_in_hook = false;
}

void* Detour_MallocAligned(size_t size, size_t alignment) {
    // if (g_in_hook) {
    //     return orig_MallocAligned(size);
    // }
    // g_in_hook = true;

    void* p = orig_MallocAligned(size, alignment);
    auto* ring = g_rings[(int)MemorySource::Editor];
    if (ring && p) { 
        static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::Editor]);

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Editor;
        rec.action = MemoryAction::Alloc;
        rec.size = size;
        rec.ptr = p;
        rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);
        
        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

        ring->enqueue(token, rec);
    }

    //g_in_hook = false;
    return p;
}
void* Detour_MallocN(size_t count, size_t size) {
    void* p = orig_MallocN(count, size);

    auto* ring = g_rings[(int)MemorySource::Editor];
    if (ring && p) { 
        static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::Editor]);

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Editor;
        rec.action = MemoryAction::Alloc;
        
        rec.size = count * size; 
        rec.ptr = p;
        rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);
        
        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

        ring->enqueue(token, rec); 
    }

    return p;
}

void* Detour_CallocAligned(size_t count, size_t size, size_t alignment)
{
    void* p = orig_CallocAligned(count, size, alignment);
    auto* ring = g_rings[(int)MemorySource::Editor];
    if (ring && p) {
        static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::Editor]);

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Editor;
        rec.action = MemoryAction::Alloc;
        rec.size = count * size;
        rec.ptr = p;
        rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);
        
        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 2);

        ring->enqueue(token, rec);
    }
    return p;
}

void SetupMemoryHooks() {
    HMODULE hCore = GetModuleHandleA("moer_cored.dll"); 
    MH_STATUS status;
    if (hCore) {
        void* pMalloc = (void*)GetProcAddress(hCore, "?Malloc@Memory@@SAPEAX_K@Z");
            if (pMalloc) {
            status = MH_CreateHook(pMalloc, &Detour_Malloc, (LPVOID*)&orig_Malloc);
            status = MH_EnableHook(pMalloc);
            printf("[Profiler] EnableHook Memory::Malloc: %s\n", MH_StatusToString(status));
        }
        void* pCalloc = (void*)GetProcAddress(hCore, "?Calloc@Memory@@SAPEAX_K0@Z");
            if (pCalloc) {
            status = MH_CreateHook(pCalloc, &Detour_Calloc, (LPVOID*)&orig_Calloc);
            status = MH_EnableHook(pCalloc);
            printf("[Profiler] EnableHook Memory::Calloc: %s\n", MH_StatusToString(status));
        }
        void* pReAlloc = (void*)GetProcAddress(hCore, "?ReAlloc@Memory@@SAPEAXPEAX_K@Z");
            if (pReAlloc) {
            status = MH_CreateHook(pReAlloc, &Detour_ReAlloc, (LPVOID*)&orig_ReAlloc);
            status = MH_EnableHook(pReAlloc);
            printf("[Profiler] EnableHook Memory::Free: %s\n", MH_StatusToString(status));
        }
        void* pFree1 = (void*)GetProcAddress(hCore, "?Free@Memory@@SAXPEAX@Z");
        if (pFree1) {
            status = MH_CreateHook(pFree1, &Detour_Free1, (LPVOID*)&orig_Free1);
            status = MH_EnableHook(pFree1);
            printf("[Profiler] EnableHook Memory::Free: %s\n", MH_StatusToString(status));
        }
        void* pFree2 = (void*)GetProcAddress(hCore, "?Free@Memory@@SAXPEAX_K@Z");
        if (pFree2) {
            status = MH_CreateHook(pFree2, &Detour_Free2, (LPVOID*)&orig_Free2);
            status = MH_EnableHook(pFree2);
            printf("[Profiler] EnableHook Memory::ReAlloc: %s\n", MH_StatusToString(status));
        }
        void* pMallocN = (void*)GetProcAddress(hCore, "?MallocN@Memory@@SAPEAX_K0@Z");
        if (pMallocN) {
            status = MH_CreateHook(pMallocN, &Detour_MallocN, (LPVOID*)&orig_MallocN);
            if (status == MH_OK) {
                status = MH_EnableHook(pMallocN);
                printf("[Profiler] EnableHook Memory:MallocN: %s\n", MH_StatusToString(status));
            }
        }
        void* pMallocAligned = (void*)GetProcAddress(hCore, "?MallocAligned@Memory@@SAPEAX_K0@Z");
            if (pMallocAligned) {
            status = MH_CreateHook(pMallocAligned, &Detour_MallocAligned, (LPVOID*)&orig_MallocAligned);
            status = MH_EnableHook(pMallocAligned);
            printf("[Profiler] EnableHook Memory:Detour_MallocAligned: %s\n", MH_StatusToString(status));
        }
        void* pCallocAligned = (void*)GetProcAddress(hCore, "?CallocAligned@Memory@@SAPEAX_K00@Z");
            if (pCallocAligned) {
            status = MH_CreateHook(pCallocAligned, &Detour_MallocAligned, (LPVOID*)&orig_CallocAligned);
            status = MH_EnableHook(pCallocAligned);
            printf("[Profiler] EnableHook Memory:Detour_CallocAligned: %s\n", MH_StatusToString(status));
        }
    }
    // HMODULE hCore = GetModuleHandleA("moer_cored.dll"); 

    // if (hCore) {
    //     void* pMalloc = (void*)GetProcAddress(hCore, "?Malloc@Memory@@SAPEAX_K@Z");
    //         if (pMalloc) {
    //         status = MH_CreateHook(pMalloc, &malloc_hook, (LPVOID*)&orig_Malloc);
    //         status = MH_EnableHook(pMalloc);
    //         printf("[Profiler] EnableHook Memory:Malloc: %s\n", MH_StatusToString(status));
    //     }
    //     void* pFree1 = (void*)GetProcAddress(hCore, "?Free@Memory@@SAXPEAX@Z");
    //     if (pFree1) {
    //         status = MH_CreateHook(pFree1, &free_hook_1, (LPVOID*)&orig_Free1);
    //         status = MH_EnableHook(pFree1);
    //         printf("[Profiler] EnableHook Memory::Free(void*) at %p\n", pFree1);
    //     }
    //     void* pFree2 = (void*)GetProcAddress(hCore, "?Free@Memory@@SAXPEAX_K@Z");
    //     if (pFree2) {
    //         status = MH_CreateHook(pFree2, &free_hook_2, (LPVOID*)&orig_Free2);
    //         status = MH_EnableHook(pFree2);
    //         printf("[Profiler] EnableHook Memory::Free(void*) at %p\n", pFree2);
    //     }
        
    // }
    //HMODULE hEditor = GetModuleHandleA("MoerEditor.exe");
}