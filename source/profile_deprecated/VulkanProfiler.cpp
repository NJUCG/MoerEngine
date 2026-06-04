#include "VulkanProfiler.h"
#include "Profile.h"
#include "ProfileUtils.h"
#include "MinHook.h"

struct VkTmpBufferAllocator;

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

    // if (!g_in_hook)
    // {
    //     g_in_hook = true;
    auto* ring = g_rings[(int)MemorySource::VulkanTmp];
    if(ring)
    {
        static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::VulkanTmp]);

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
        rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);

        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 1);

        ring->enqueue(token, rec);
    }
        

    //     g_in_hook = false;
    // }

    return handle;
}

uint64_t VkTmpAllocate_Hook_2(
    VkTmpBufferAllocator* _this,
    uint64_t _size,
    EVkInternalBufferUsage _usage)
{
    uint64_t handle = orig_VkTmpAllocate_2(_this, _size, _usage);

    // if (!g_in_hook)
    // {
    //     g_in_hook = true;
    auto* ring = g_rings[(int)MemorySource::VulkanTmp];
    if(ring)
    {
        static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::VulkanTmp]);

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
        rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);
        
        capture_frames_fast(rec.frames, MAX_FRAMES, rec.frame_count, 1);

        ring->enqueue(token, rec);
    }
    //     g_in_hook = false;
    // }

    return handle;
}


void VkTmpDeAllocate_Hook(
    VkTmpBufferAllocator* _this,
    uint64_t _handle)
{
    // if (!g_in_hook)
    // {
    //     g_in_hook = true;
    auto* ring = g_rings[(int)MemorySource::VulkanTmp];
    if(ring)
    {
        static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::VulkanTmp]);

        void* p = reinterpret_cast<void*>(_handle);

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::VulkanTmp;
        rec.action = MemoryAction::Free;
        rec.size = 0;
        rec.ptr = p;
        rec.frame_count = 0;
        rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);

        ring->enqueue(token, rec);
    }  
    //     g_in_hook = false;
    // }
    orig_VkTmpDeAllocate(_this, _handle);
}
//---------------Vk--------------

typedef VkResult (VKAPI_PTR* PFN_vkAllocateMemory)(
    VkDevice,
    const VkMemoryAllocateInfo*,
    const VkAllocationCallbacks*,
    VkDeviceMemory*);

typedef void (VKAPI_PTR* PFN_vkFreeMemory)(
    VkDevice,
    VkDeviceMemory,
    const VkAllocationCallbacks*);

PFN_vkAllocateMemory orig_vkAllocateMemory = nullptr;
PFN_vkFreeMemory     orig_vkFreeMemory = nullptr;

VkResult VKAPI_PTR Detour_vkAllocateMemory(
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

    auto* ring = g_rings[(int)MemorySource::Vulkan];
    if (result == VK_SUCCESS && ring)// && !g_in_hook)
    {
        static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::Vulkan]);

        //g_in_hook = true;

        uint64_t size = pAllocateInfo->allocationSize;
        void* p = reinterpret_cast<void*>(*pMemory);

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Vulkan;
        rec.action = MemoryAction::Alloc;
        rec.size = size;
        rec.ptr = p;
        rec.frame_count = 0;
        rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);

        capture_frames_fast(
            rec.frames,
            MAX_FRAMES,
            rec.frame_count,
            1);

        ring->enqueue(token, rec); 

        //g_in_hook = false;
    }

    return result;
}

void VKAPI_PTR Detour_vkFreeMemory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator)
{
    // if (!g_in_hook)
    // {
    //     g_in_hook = true;
    auto* ring = g_rings[(int)MemorySource::Vulkan];
    if(ring)
    {
        static thread_local moodycamel::ProducerToken token(*g_rings[(size_t)MemorySource::Vulkan]);

        void* p = reinterpret_cast<void*>(memory);
        uint64_t size = 0;

        EventRecord rec;
        rec.ts_us = now_us();
        rec.source = MemorySource::Vulkan;
        rec.action = MemoryAction::Free;
        rec.size = size;
        rec.ptr = p;
        rec.frame_count = 0;
        rec.sequence = g_global_sequence.fetch_add(1, std::memory_order_relaxed);

        capture_frames_fast(
            rec.frames,
            MAX_FRAMES,
            rec.frame_count,
            1);

        ring->enqueue(token, rec);
    }
        //g_in_hook = false;
    //}
    orig_vkFreeMemory(device, memory, pAllocator);
}
//--------------------------------------------------TimeProfile
VkDevice g_vk_device = VK_NULL_HANDLE;
const double g_timestamp_period = 1.0;

std::unordered_map<VkCommandBuffer, CBState> g_cb_states;
std::shared_mutex                            g_cb_states_mtx;

std::unordered_map<VkQueue, QueueState>      g_queue_states;
std::shared_mutex                            g_queue_states_mtx;

std::vector<VkQueryPool>                     g_pool_free_list;
std::mutex                                   g_pool_free_list_mtx;

thread_local std::vector<VkQueryPool>        t_pool_cache;

static std::vector<VkQueryPool> g_all_pools; // 用于最终销毁
static std::mutex g_all_pools_mtx;

thread_local std::unordered_map<VkCommandBuffer, std::vector<RawPassTimer>> t_timer_stacks;

PFN_vkCreateQueryPool    fn_vkCreateQueryPool = nullptr  ;
PFN_vkGetQueryPoolResults fn_vkGetQueryPoolResults = nullptr ;
PFN_vkCmdWriteTimestamp  fn_vkCmdWriteTimestamp = nullptr  ;
PFN_vkCmdResetQueryPool  fn_vkCmdResetQueryPool = nullptr  ;
PFN_vkResetQueryPool fn_vkResetQueryPool = nullptr;
PFN_vkDestroyQueryPool fn_vkDestroyQueryPool = nullptr;
PFN_vkGetSemaphoreCounterValue fn_vkGetSemaphoreCounterValue = nullptr;
PFN_vkCreateSemaphore fn_vkCreateSemaphore = nullptr;
PFN_vkDestroySemaphore fn_vkDestroySemaphore = nullptr;
PFN_vkQueueWaitIdle fn_vkQueueWaitIdle = nullptr;
void InitFunc() {
    HMODULE vklib = GetModuleHandleA("vulkan-1.dll");

    fn_vkCreateQueryPool    = (PFN_vkCreateQueryPool)   GetProcAddress(vklib, "vkCreateQueryPool");
    fn_vkGetQueryPoolResults= (PFN_vkGetQueryPoolResults)GetProcAddress(vklib, "vkGetQueryPoolResults");
    fn_vkCmdWriteTimestamp  = (PFN_vkCmdWriteTimestamp) GetProcAddress(vklib, "vkCmdWriteTimestamp");
    fn_vkCmdResetQueryPool  = (PFN_vkCmdResetQueryPool) GetProcAddress(vklib, "vkCmdResetQueryPool");
    fn_vkResetQueryPool     = (PFN_vkResetQueryPool)    GetProcAddress(vklib, "vkResetQueryPool");
    fn_vkDestroyQueryPool = (PFN_vkDestroyQueryPool)    GetProcAddress(vklib, "vkDestroyQueryPool");
    fn_vkGetSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue) GetProcAddress(vklib, "vkGetSemaphoreCounterValue");
    fn_vkCreateSemaphore = (PFN_vkCreateSemaphore) GetProcAddress(vklib, "vkCreateSemaphore");
    fn_vkDestroySemaphore = (PFN_vkDestroySemaphore) GetProcAddress(vklib, "vkDestroySemaphore");
    fn_vkQueueWaitIdle = (PFN_vkQueueWaitIdle) GetProcAddress(vklib, "vkQueueWaitIdle");

#define CHECK_VK_FUNC(funcName) \
    if (!fn_##funcName) { \
        std::cerr << "[VulkanProfiler] ERROR: Failed to get address of " << #funcName << std::endl; \
    }

    CHECK_VK_FUNC(vkCreateQueryPool);
    CHECK_VK_FUNC(vkGetQueryPoolResults);
    CHECK_VK_FUNC(vkCmdWriteTimestamp);
    CHECK_VK_FUNC(vkCmdResetQueryPool);
    CHECK_VK_FUNC(vkResetQueryPool);
    CHECK_VK_FUNC(vkDestroyQueryPool);
    CHECK_VK_FUNC(vkGetSemaphoreCounterValue);
    CHECK_VK_FUNC(vkCreateSemaphore);
    CHECK_VK_FUNC(vkDestroySemaphore);
    CHECK_VK_FUNC(vkQueueWaitIdle);

#undef LOAD_VK_FUNC

}

static VkQueryPool AcquireQueryPool() {
    if (!t_pool_cache.empty()) {
        VkQueryPool pool = t_pool_cache.back();
        t_pool_cache.pop_back();
        return pool;
    }

    {
        std::lock_guard<std::mutex> lock(g_pool_free_list_mtx);
        if (!g_pool_free_list.empty()) {
            VkQueryPool pool = g_pool_free_list.back();
            g_pool_free_list.pop_back();
            return pool;
        }
    }

    VkQueryPoolCreateInfo ci{};
    ci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = MAX_GPU_PASSES * 2;

    VkQueryPool pool = VK_NULL_HANDLE;
    if (fn_vkCreateQueryPool(g_vk_device, &ci, nullptr, &pool) == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_all_pools_mtx);
        g_all_pools.push_back(pool);
    }
    return pool;
}

static void ReturnQueryPool(VkQueryPool pool) {
    if (!pool) return;

    if (fn_vkResetQueryPool)
        fn_vkResetQueryPool(g_vk_device, pool, 0, MAX_GPU_PASSES * 2);

    std::lock_guard<std::mutex> lock(g_pool_free_list_mtx);
    g_pool_free_list.push_back(pool);
}

static QueueState& GetOrCreateQueueState(VkQueue queue) {
    {
        std::shared_lock lock(g_queue_states_mtx);
        auto it = g_queue_states.find(queue);
        if (it != g_queue_states.end()) return it->second;
    }


    std::unique_lock lock(g_queue_states_mtx);
    auto& qs = g_queue_states[queue];
    if (qs.timeline_sem != VK_NULL_HANDLE) return qs;

    VkSemaphoreTypeCreateInfo type_info{};
    type_info.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue  = 0;

    VkSemaphoreCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    ci.pNext = &type_info;

    fn_vkCreateSemaphore(g_vk_device, &ci, nullptr, &qs.timeline_sem);
    qs.timeline_value = 0;
    return qs;
}

typedef VkResult(VKAPI_PTR* PFN_vkQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
PFN_vkQueueSubmit orig_vkQueueSubmit = nullptr;

VkResult VKAPI_PTR Detour_vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence)
{
    VkResult result = orig_vkQueueSubmit(queue, submitCount, pSubmits, fence);

    return result;
}

typedef VkResult (VKAPI_PTR *PFN_vkQueueSubmit2)(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence);
PFN_vkQueueSubmit2 orig_vkQueueSubmit2 = nullptr;

//vulkan规定vkQueueSubmit2必须手动互斥，参见VulkanQueue.h:43.说明在同一Queue中hook不需要加锁
VkResult VKAPI_PTR Detour_vkQueueSubmit2(
    VkQueue              queue,
    uint32_t             submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence              fence)
{
    QueueState& qs = GetOrCreateQueueState(queue);

    uint64_t completed = 0;
    if (fn_vkGetSemaphoreCounterValue)
    {
        fn_vkGetSemaphoreCounterValue(g_vk_device, qs.timeline_sem, &completed);
    }
        
    // 从 pending 头部开始
    while (!qs.pending.empty()) {
        SubmitRecord& rec = qs.pending.front();
        if (rec.signal_value > completed)
        {
            break;
        }
        current_frame = (std::max)(current_frame, rec.signal_value);

        for (auto& snap : rec.cb_snapshots) {
            if (!snap.pool_valid || snap.pass_count == 0) continue;

            uint64_t timestamps[MAX_GPU_PASSES * 2] = {};
            VkResult qr = fn_vkGetQueryPoolResults(
                g_vk_device,
                snap.pool,
                0,
                snap.pass_count * 2,
                sizeof(uint64_t) * snap.pass_count * 2,
                timestamps,
                sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT
            );

            //读取失败也无所谓
            if (qr == VK_SUCCESS) {
                for (int i = 0; i < snap.pass_count; i++) {
                    uint64_t begin = timestamps[i * 2];
                    uint64_t end   = timestamps[i * 2 + 1];
                    if (end > begin) {
                        snap.passes[i].gpu_render_ms =
                            (float)((end - begin) * g_timestamp_period * 1e-6f);
                        snap.passes[i].gpu_valid = true;
                    }
                }

                UpdatePassHistory(snap);
            }

            ReturnQueryPool(snap.pool);
            snap.pool       = VK_NULL_HANDLE;
            snap.pool_valid = false;
        }

        qs.pending.pop_front();
    }

    SubmitRecord record;
    record.signal_value = 0;

    for (uint32_t s = 0; s < submitCount; s++) {
        const VkSubmitInfo2& si = pSubmits[s];
        for (uint32_t c = 0; c < si.commandBufferInfoCount; c++) {
            VkCommandBuffer cb_handle = si.pCommandBufferInfos[c].commandBuffer;

            std::unique_lock lock(g_cb_states_mtx);
            auto it = g_cb_states.find(cb_handle);
            if (it != g_cb_states.end()) {
                record.cb_snapshots.push_back(std::move(it->second));
                g_cb_states.erase(it);
            }

            t_timer_stacks.erase(cb_handle);
        }
    }
    //正常情况下g_cb_states和t_timer_stacks每一次submit后应该清空
    //printf("[profile]g_cb_states size : %d\n", g_cb_states.size());
    //printf("[profile]t_timer_stacks size : %d\n", t_timer_stacks.size());

    // 对最后一个cb手动注入 Timeline Semaphore signal
/*
    uint64_t this_value = ++qs.timeline_value;
    record.signal_value = this_value;

    VkResult result = VK_SUCCESS;

    if (submitCount > 0) {
        VkSemaphoreSubmitInfo inject{};
        inject.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        inject.semaphore = qs.timeline_sem;
        inject.value     = this_value;
        inject.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        const VkSubmitInfo2& last = pSubmits[submitCount - 1];
        std::vector<VkSemaphoreSubmitInfo> signals(
            last.pSignalSemaphoreInfos,
            last.pSignalSemaphoreInfos + last.signalSemaphoreInfoCount
        );
        signals.push_back(inject);

        VkSubmitInfo2 patched            = last;
        patched.pSignalSemaphoreInfos    = signals.data();
        patched.signalSemaphoreInfoCount = (uint32_t)signals.size();

        // 前 N-1 个 submit 原样提交
        if (submitCount > 1) {
            result = orig_vkQueueSubmit2(queue, submitCount - 1, pSubmits, VK_NULL_HANDLE);
            if (result != VK_SUCCESS) return result;
        }
        result = orig_vkQueueSubmit2(queue, 1, &patched, fence);

    } else {
        // submitCount == 0，单独补一个空 submit 来 signal semaphore
        result = orig_vkQueueSubmit2(queue, 0, nullptr, fence);

        VkSemaphoreSubmitInfo inject{};
        inject.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        inject.semaphore = qs.timeline_sem;
        inject.value     = this_value;
        inject.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkSubmitInfo2 empty{};
        empty.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        empty.pSignalSemaphoreInfos    = &inject;
        empty.signalSemaphoreInfoCount = 1;
        orig_vkQueueSubmit2(queue, 1, &empty, VK_NULL_HANDLE);
    }
*/
    //直接创建空submit只做signal
    uint64_t this_value = ++qs.timeline_value;
    record.signal_value = this_value;

    VkResult result = VK_SUCCESS;
    result = orig_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
    if (result != VK_SUCCESS) return result;

    VkSemaphoreSubmitInfo inject{};
    inject.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    inject.semaphore = qs.timeline_sem;
    inject.value     = this_value;
    inject.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 empty{};
    empty.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    empty.pSignalSemaphoreInfos    = &inject;
    empty.signalSemaphoreInfoCount = 1;

    orig_vkQueueSubmit2(queue, 1, &empty, VK_NULL_HANDLE);

    //计入历史submit
    qs.pending.push_back(std::move(record));

    return result;
}

//--------------
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vkGetDeviceProcAddr)(VkDevice device, const char* pName);
PFN_vkGetDeviceProcAddr orig_vkGetDeviceProcAddr = nullptr;
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
PFN_vkGetInstanceProcAddr orig_vkGetInstanceProcAddr = nullptr;

typedef void (VKAPI_PTR *PFN_vkCmdBeginDebugUtilsLabelEXT)(VkCommandBuffer commandBuffer, const VkDebugUtilsLabelEXT* pLabelInfo);
typedef void (VKAPI_PTR *PFN_vkCmdEndDebugUtilsLabelEXT)(VkCommandBuffer commandBuffer);
PFN_vkCmdBeginDebugUtilsLabelEXT orig_vkCmdBeginDebugUtilsLabelEXT = nullptr;
PFN_vkCmdEndDebugUtilsLabelEXT   orig_vkCmdEndDebugUtilsLabelEXT   = nullptr;

void VKAPI_PTR Detour_vkCmdBeginDebugUtilsLabelEXT(
    VkCommandBuffer             commandBuffer,
    const VkDebugUtilsLabelEXT* pLabelInfo)
{
    orig_vkCmdBeginDebugUtilsLabelEXT(commandBuffer, pLabelInfo);
    if (!pLabelInfo || !pLabelInfo->pLabelName) return;

    CBState* cb = nullptr;
    {
        std::shared_lock lock(g_cb_states_mtx);
        auto it = g_cb_states.find(commandBuffer);
        if (it != g_cb_states.end()) {
            cb = &it->second;
        }
    }

    if (!cb) {
        VkQueryPool pool = AcquireQueryPool();

        //新建的pool需要reset
        if (fn_vkResetQueryPool)
            fn_vkResetQueryPool(g_vk_device, pool, 0, MAX_GPU_PASSES * 2);

        std::unique_lock lock(g_cb_states_mtx);
        auto& state       = g_cb_states[commandBuffer];
        state.pool        = pool;
        state.pool_valid  = (pool != VK_NULL_HANDLE);
        state.pass_count  = 0;
        state.stack_top   = 0;
        cb = &state;
    }

    int idx = cb->pass_count;
    if (idx >= MAX_GPU_PASSES) return;
    cb->pass_count++;

    auto& p = cb->passes[idx];
    strncpy(p.name, pLabelInfo->pLabelName, 63);
    p.name[63]      = '\0';
    p.gpu_render_ms = 0.0f;
    p.gpu_valid     = false;
    p.query_idx     = idx;

    //嵌套label栈
    p.depth      = cb->stack_top;
    p.parent_idx = (cb->stack_top > 0) ? cb->label_stack[cb->stack_top - 1] : -1;
    if (cb->stack_top > 0) {
        int parent = cb->label_stack[cb->stack_top - 1];
        strncpy(p.parent_name, cb->passes[parent].name, 63);
        p.parent_name[63] = '\0';
    } else {
        p.parent_name[0] = '\0';
    }
    cb->label_stack[cb->stack_top++] = idx;

    if (cb->pool_valid && fn_vkCmdWriteTimestamp) {
        fn_vkCmdWriteTimestamp(
            commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            cb->pool,
            idx * 2  // begin slot
        );
    }

    //启动CPU timer（thread_local无锁），暂时用不上
    auto& timer_stack = t_timer_stacks[commandBuffer];
    RawPassTimer timer;
    timer.Start();
    timer_stack.push_back(timer);
}

void VKAPI_PTR Detour_vkCmdEndDebugUtilsLabelEXT(VkCommandBuffer commandBuffer)
{
    CBState* cb = nullptr;
    {
        std::shared_lock lock(g_cb_states_mtx);
        auto it = g_cb_states.find(commandBuffer);
        if (it != g_cb_states.end())
            cb = &it->second;
    }

    if (cb && cb->stack_top > 0) {
        int idx = cb->label_stack[--cb->stack_top];

        auto& timer_stack = t_timer_stacks[commandBuffer];
        if (!timer_stack.empty()) {
            timer_stack.back().Stop();
            cb->passes[idx].cpu_record_clock = timer_stack.back().GetDelta();
            timer_stack.pop_back();
        }

        if (cb->pool_valid && fn_vkCmdWriteTimestamp) {
            fn_vkCmdWriteTimestamp(
                commandBuffer,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                cb->pool,
                idx * 2 + 1  // end slot
            );
        }
    }

    orig_vkCmdEndDebugUtilsLabelEXT(commandBuffer);
}

typedef void(VKAPI_PTR* PFN_vkDestroyDevice)(VkDevice, const VkAllocationCallbacks*);
PFN_vkDestroyDevice orig_vkDestroyDevice = nullptr;

void VKAPI_PTR Detour_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    if (device != g_vk_device)
    {
        orig_vkDestroyDevice(device, pAllocator);
        return;
    }

    {
        std::shared_lock lock(g_queue_states_mtx);
        for (auto& [queue, qs] : g_queue_states) {
            fn_vkQueueWaitIdle(queue);
        }
    }

    {
        std::unique_lock lock(g_queue_states_mtx);
        for (auto& [queue, qs] : g_queue_states) {
            if (qs.timeline_sem != VK_NULL_HANDLE)
                fn_vkDestroySemaphore(device, qs.timeline_sem, nullptr);
        }
        g_queue_states.clear();
    }

    {
        // std::lock_guard<std::mutex> lock(g_pool_free_list_mtx);
        // for (VkQueryPool pool : g_pool_free_list)
        //     fn_vkDestroyQueryPool(device, pool, nullptr);
        g_pool_free_list.clear();
    }

    {
        std::lock_guard<std::mutex> lock(g_all_pools_mtx);
        for (auto pool : g_all_pools) {
            if (pool != VK_NULL_HANDLE) {
                fn_vkDestroyQueryPool(device, pool, pAllocator);
            }
        }
        g_all_pools.clear();
    }
    printf("[Profiler] QueryPools destroyed\n");

    orig_vkDestroyDevice(device, pAllocator);
}

//hook vkGetDeviceProcAddr, replace vkAllocateMemory to Detour_vkAllocateMemory
PFN_vkVoidFunction VKAPI_PTR Detour_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    PFN_vkVoidFunction addr = orig_vkGetDeviceProcAddr(device, pName);

    if (g_vk_device == VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        g_vk_device = device;
        InitFunc();
    }

    if (pName && strcmp(pName, "vkAllocateMemory") == 0) {
        printf("[profiler] Detour_vkGetDeviceProcAddr1\n");
        orig_vkAllocateMemory = (PFN_vkAllocateMemory)addr;
        return (PFN_vkVoidFunction)Detour_vkAllocateMemory; //hook vkAllocateMemory 第二层，通过vkGetDeviceProcAddr直接返回hook函数
    }
    
    if (pName && strcmp(pName, "vkFreeMemory") == 0) {
        orig_vkFreeMemory = (PFN_vkFreeMemory)addr;
        return (PFN_vkVoidFunction)Detour_vkFreeMemory;
    }
    if (strcmp(pName, "vkCmdBeginDebugUtilsLabelEXT") == 0) {
        orig_vkCmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT)addr;
        return (PFN_vkVoidFunction)Detour_vkCmdBeginDebugUtilsLabelEXT;
    }
    if (strcmp(pName, "vkCmdEndDebugUtilsLabelEXT") == 0) {
        orig_vkCmdEndDebugUtilsLabelEXT = (PFN_vkCmdEndDebugUtilsLabelEXT)addr;
        return (PFN_vkVoidFunction)Detour_vkCmdEndDebugUtilsLabelEXT;
    }

    if (pName && strcmp(pName, "vkQueueSubmit") == 0) {
        orig_vkQueueSubmit = (PFN_vkQueueSubmit)addr;
        return (PFN_vkVoidFunction)Detour_vkQueueSubmit;
    }
    if (pName && strcmp(pName, "vkQueueSubmit2") == 0) {
        orig_vkQueueSubmit2 = (PFN_vkQueueSubmit2)addr;
        return (PFN_vkVoidFunction)Detour_vkQueueSubmit2;
    }
    if (pName && strcmp(pName, "vkResetQueryPool") == 0) {
        fn_vkResetQueryPool = (PFN_vkResetQueryPool)addr;
        return (PFN_vkVoidFunction)fn_vkResetQueryPool;
    }
    if (pName && strcmp(pName, "vkDestroyDevice") == 0) {
        orig_vkDestroyDevice = (PFN_vkDestroyDevice)addr;
        return (PFN_vkVoidFunction)Detour_vkDestroyDevice;
    }
    return addr;
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

PFN_vkVoidFunction VKAPI_PTR Detour_vkGetInstanceProcAddr(
    VkInstance instance,
    const char* pName)
{
    
    PFN_vkVoidFunction addr = orig_vkGetInstanceProcAddr(instance, pName);

    // if (pName) {
    //     printf("[Profiler] GIPA Query: %s -> Address: %p\n", pName, (void*)addr);
    // }
    if (!pName) return addr;

    if (pName && strcmp(pName, "vkAllocateMemory") == 0) {
        printf("[profiler] Detour_vkGetDeviceProcAddr1\n");
        orig_vkAllocateMemory = (PFN_vkAllocateMemory)addr;
        return (PFN_vkVoidFunction)Detour_vkAllocateMemory; //hook vkAllocateMemory 第二层，通过vkGetDeviceProcAddr直接返回hook函数
    }
    
    if (pName && strcmp(pName, "vkFreeMemory") == 0) {
        orig_vkFreeMemory = (PFN_vkFreeMemory)addr;
        return (PFN_vkVoidFunction)Detour_vkFreeMemory;
    }

    if (strcmp(pName, "vkCmdBeginDebugUtilsLabelEXT") == 0) {
        orig_vkCmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT)addr;
        return (PFN_vkVoidFunction)Detour_vkCmdBeginDebugUtilsLabelEXT;
    }

    if (strcmp(pName, "vkCmdEndDebugUtilsLabelEXT") == 0) {
        orig_vkCmdEndDebugUtilsLabelEXT = (PFN_vkCmdEndDebugUtilsLabelEXT)addr;
        return (PFN_vkVoidFunction)Detour_vkCmdEndDebugUtilsLabelEXT;
    }
    
    if (pName && strcmp(pName, "vkQueueSubmit") == 0) {
        orig_vkQueueSubmit = (PFN_vkQueueSubmit)addr;
        return (PFN_vkVoidFunction)Detour_vkQueueSubmit;
    }
    if (pName && strcmp(pName, "vkQueueSubmit2") == 0) {
        orig_vkQueueSubmit2 = (PFN_vkQueueSubmit2)addr;
        return (PFN_vkVoidFunction)Detour_vkQueueSubmit2;
    }
    if (pName && strcmp(pName, "vkDestroyDevice") == 0) {
        orig_vkDestroyDevice = (PFN_vkDestroyDevice)addr;
        return (PFN_vkVoidFunction)Detour_vkDestroyDevice;
    }
    return addr;
}

//目前没有启用
typedef void(__thiscall* PFN_OnFinishPresent)(void* _this, uint64_t _image_idx);
PFN_OnFinishPresent orig_OnFinishPresent = nullptr;

void SetupVulkanHooks(HMODULE vklib) {
    MH_STATUS status;
    //Editor---------------------------------
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
        status = MH_CreateHook(
            freeAddr,
            &VkTmpDeAllocate_Hook,
            (LPVOID*)&orig_VkTmpDeAllocate
        );
        status = MH_EnableHook(freeAddr);
        printf("[Profiler] EnableHook DeAllocate(uint64 _handle) %s\n", MH_StatusToString(status));
    }

    // void* pOnFinishPresent = GetProcAddressFromPdb(
    // "?OnFinishPresent@VkSwapchain@Render@Moer@@AEAAX_K@Z"
    // );
    // if (pOnFinishPresent) {
    //     status = MH_CreateHook(pOnFinishPresent, &Detour_OnFinishPresent, (LPVOID*)&orig_OnFinishPresent);
    //     status = MH_EnableHook(pOnFinishPresent);
    //     printf("[Profiler] EnableHook OnFinishPresent %s\n", MH_StatusToString(status));
    // }

    //Vulkan---------------------------------
    if (orig_vkAllocateMemory) return;
    void* gdpa = (void*)GetProcAddress(vklib, "vkGetDeviceProcAddr");
    if (gdpa) {
        MH_STATUS status = MH_CreateHook(gdpa, &Detour_vkGetDeviceProcAddr, (LPVOID*)&orig_vkGetDeviceProcAddr);
        status = MH_EnableHook(gdpa);
        printf("[Profiler] EnableHook vkGetDeviceProcAddr: %s\n", MH_StatusToString(status));
    }

    void* gipa = (void*)GetProcAddress(vklib, "vkGetInstanceProcAddr");
    if (gipa) {
        MH_STATUS status = MH_CreateHook(gipa, &Detour_vkGetInstanceProcAddr, (LPVOID*)&orig_vkGetInstanceProcAddr);
        status = MH_EnableHook(gipa);
        printf("[Profiler] EnableHook vkGetInstanceProcAddr: %s\n", MH_StatusToString(status));
    }

    if (vklib) {
        void* addr = (void*)GetProcAddress(vklib, "vkAllocateMemory");
        MH_STATUS status = MH_CreateHook(addr, &Detour_vkAllocateMemory, (LPVOID*)&orig_vkAllocateMemory);
        status = MH_EnableHook(addr);
        printf("[Profiler] EnableHook vkAllocateMemory: %s\n", MH_StatusToString(status));

        addr = (void*)GetProcAddress(vklib, "vkFreeMemory");
        MH_CreateHook(
            addr,
            Detour_vkFreeMemory,
            reinterpret_cast<void**>(&orig_vkFreeMemory));

        status = MH_EnableHook(addr);
        printf("[Profiler] EnableHook vkFreeMemory: %s\n", MH_StatusToString(status));

        //vkCmdBeginDebugUtilsLabelEXT不属于vulkan-1.dll的静态导出函数

        addr = (void*)GetProcAddress(vklib, "vkQueueSubmit");
        if (addr) {
            status = MH_CreateHook(addr, &Detour_vkQueueSubmit, (LPVOID*)&orig_vkQueueSubmit);
            MH_EnableHook(addr);
            printf("[Profiler] EnableHook vkQueueSubmit: %s\n", MH_StatusToString(status));
        }
        addr = (void*)GetProcAddress(vklib, "vkQueueSubmit2");
        if (addr) {
            status = MH_CreateHook(addr, &Detour_vkQueueSubmit2, (LPVOID*)&orig_vkQueueSubmit2);
            MH_EnableHook(addr);
            printf("[Profiler] EnableHook vkQueueSubmit2: %s\n", MH_StatusToString(status));
        }
    }
    else
    {
        printf("[Profiler] No vklib\n");
    }
}