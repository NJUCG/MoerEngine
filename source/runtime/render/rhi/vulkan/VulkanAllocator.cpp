#include "VulkanAllocator.h"
#include "PixelFormat.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"
#include "VulkanResourceTracker.h"
#include "rhi/RHIImpl.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Moer::Render {

namespace {

bool InvokeAllocatorCompletionCallbacksNoexcept(
    Array<std::function<void()>>& _callbacks,
    std::string_view              _owner
) noexcept {
    bool succeeded = true;
    for (auto& callback : _callbacks) {
        try {
            if (callback) {
                callback();
            }
        } catch (const std::exception& error) {
            succeeded = false;
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] {} completion callback threw: {}",
                    _owner,
                    error.what()
                );
            } catch (...) {
            }
        } catch (...) {
            succeeded = false;
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] {} completion callback threw",
                    _owner
                );
            } catch (...) {
            }
        }
    }
    _callbacks.clear();
    return succeeded;
}

} // namespace

// VulkanAllocatorBase
VulkanAllocatorBase::VulkanAllocatorBase(VulkanDevice* _device, EQueueType _type) :
    VulkanDeviceObject(_device),
    queue_type(_type),
    tracker(_type) {
    cmd_allocator.emplace(_device, _type);
    cmd_list.emplace(&cmd_allocator.value(), *_device);
}

VulkanAllocatorBase::~VulkanAllocatorBase() {
    cmd_list.reset();
    cmd_allocator.reset();
    on_complete.clear();
    tracker.Reset();
}

bool VulkanAllocatorBase::ResetCmdList() {
    if (m_device->IsDeviceLost()) {
        m_device->RecordSkippedCommandPoolReset();
        return false;
    }

    VkQueue queue = VK_NULL_HANDLE;
    switch (queue_type) {
        case EQueueType::Graphics:
            queue = m_device->GetGraphicsQueue();
            break;
        case EQueueType::Compute:
            queue = m_device->GetComputeQueue();
            break;
        case EQueueType::Copy:
            queue = m_device->GetTransferQueue();
            break;
        default:
            break;
    }

    const VkResult result = m_device->ResetCommandPool(
        cmd_allocator->GetHandle(),
        VulkanOperationContext{
            .operation  = EVulkanFaultOperation::CommandPoolReset,
            .queue_type = queue_type,
            .queue      = queue,
        }
    );
    if (result == VK_SUCCESS) {
        cmd_list->SetDescriptorPushLease({});
        return true;
    }
    return false;
}

bool VulkanAllocatorBase::CompleteSuccess() noexcept {
    return InvokeAllocatorCompletionCallbacksNoexcept(
        on_complete, "allocator"
    );
}

bool VulkanAllocatorBase::Reset() {
    return ResetCmdList();
}

bool VulkanAllocatorBase::ResetAbandoned() {
    on_complete.clear();
    tracker.Reset();
    return Reset();
}

// VulkanPresentor
VulkanPresentor::VulkanPresentor(VulkanDevice* _device, EQueueType _type) :
    VulkanAllocatorBase(_device, _type) {
    VkSemaphoreCreateInfo semaphore_info{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    const VkResult result = vkCreateSemaphore(
        m_device->GetDevice(),
        &semaphore_info,
        VK_NULL_HANDLE,
        &image_ready_semaphore
    );
    if (result != VK_SUCCESS) {
        image_ready_semaphore = VK_NULL_HANDLE;
        m_device->TryLatchFirstFault(
            VulkanOperationContext{
                .operation  = EVulkanFaultOperation::SwapchainSemaphoreCreate,
                .queue_type = _type,
            },
            result
        );
        throw std::runtime_error(
            "failed to create Vulkan presentor image-ready semaphore"
        );
    }
    try {
        m_device->SetResourceName(
            uint64(image_ready_semaphore),
            VK_OBJECT_TYPE_SEMAPHORE,
            "PresentorImageReadySemaphore_" +
                std::to_string(reinterpret_cast<uintptr_t>(this))
        );
    } catch (...) {
        vkDestroySemaphore(
            m_device->GetDevice(), image_ready_semaphore, VK_NULL_HANDLE
        );
        image_ready_semaphore = VK_NULL_HANDLE;
        throw;
    }
}

VulkanPresentor::~VulkanPresentor() {
    if (image_ready_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(
            m_device->GetDevice(), image_ready_semaphore, VK_NULL_HANDLE
        );
        image_ready_semaphore = VK_NULL_HANDLE;
    }
}

bool VulkanPresentor::CompleteSuccess() noexcept {
    return InvokeAllocatorCompletionCallbacksNoexcept(
        on_complete, "presentor"
    );
}

#pragma region[ Query Pool ]

VkNativeQueryPool::VkNativeQueryPool(
    VulkanDevice& _device,
    VkQueryType    _type,
    uint32         _query_count,
    EQueueType     _queue_type
) :
    device(_device),
    type(_type),
    queue_type(_queue_type) {
    query_pool = CreatePool(_query_count);
    count      = _query_count;
}

VkQueryPool VkNativeQueryPool::CreatePool(uint32 _query_count) {
    if (_query_count == 0) {
        throw std::invalid_argument(
            "Vulkan query-pool creation requires at least one slot"
        );
    }

    VkQueryPoolCreateInfo create_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    create_info.queryType  = type;
    create_info.queryCount = _query_count;
    VkQueryPool new_pool = VK_NULL_HANDLE;
    const VkResult result = vkCreateQueryPool(
        device.GetDevice(), &create_info, nullptr, &new_pool
    );
    if (result != VK_SUCCESS) {
        device.TryLatchFirstFault(
            VulkanOperationContext{
                .operation  = EVulkanFaultOperation::QueryPoolCreate,
                .queue_type = queue_type,
            },
            result
        );
        throw std::runtime_error(
            "Vulkan query-pool creation failed: " +
            std::string(VulkanResultName(result))
        );
    }
    return new_pool;
}

VkNativeQueryPool::~VkNativeQueryPool() {
    if (query_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device.GetDevice(), query_pool, nullptr);
    }
}

VkResult VkNativeQueryPool::GetResults(
    std::span<uint64>  _data,
    uint32             _first_query,
    uint32             _query_count,
    VkQueryResultFlags _flags
) {
    if (_query_count == 0) {
        return VK_SUCCESS;
    }
    if (_first_query > count || _query_count > count - _first_query) {
        throw std::out_of_range(
            "Vulkan query result range exceeds the native pool"
        );
    }

    const size_t values_per_query =
        (_flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0 ? 2u : 1u;
    if (_query_count > _data.size() / values_per_query) {
        throw std::invalid_argument(
            "Vulkan query result span is too small for the requested flags"
        );
    }
    const VkDeviceSize stride =
        sizeof(uint64) * static_cast<VkDeviceSize>(values_per_query);
    return vkGetQueryPoolResults(
        device.GetDevice(),
        query_pool,
        _first_query,
        _query_count,
        _data.size_bytes(),
        _data.data(),
        stride,
        VK_QUERY_RESULT_64_BIT | _flags
    );
}

void VkNativeQueryPool::EnsureCapacity(uint32 _required_count) {
    if (_required_count <= count) {
        return;
    }

    uint32 grown_count = std::max(count, uint32{1});
    while (grown_count < _required_count) {
        if (grown_count > std::numeric_limits<uint32>::max() / 2) {
            grown_count = _required_count;
            break;
        }
        grown_count *= 2;
    }

    // Allocate first so a failed growth preserves the old idle pool. The
    // owning VulkanAllocator calls this only before beginning native record.
    VkQueryPool replacement = CreatePool(grown_count);
    if (query_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device.GetDevice(), query_pool, nullptr);
    }
    query_pool = replacement;
    count      = grown_count;
}

#pragma endregion

// VulkanAllocator
VulkanAllocator::VulkanAllocator(VulkanDevice* _device, EQueueType _type) :
    VulkanAllocatorBase(_device, _type),
    allocator(_device),
    upload_allocator(&allocator, small_block_size, 1.5),
    readback_allocator(&allocator, small_block_size, 1.5),
    scratch_allocator(_device, &allocator),
    shader_buffer_allocator(_device, &allocator),
    timestamp_valid_bits(_device->GetTimestampValidBits(_type)),
    timestamp_period_ns(_device->GetCoreProperties().core_1_0.limits.timestampPeriod) {}

VulkanAllocator::~VulkanAllocator() {
    upload_allocator.Dispose();
    readback_allocator.Dispose();
    scratch_allocator.Reset();
    shader_buffer_allocator.Reset();
    for (auto& handle : large_buffers) {
        allocator.DeAllocate(reinterpret_cast<uint64>(handle));
    }
    large_buffers.clear();
}

BufferView VulkanAllocator::AllocateUploadBuffer(uint64 _size, uint _alignment) {
    _size = std::max<uint64>(_size, _alignment);
    if (_size < small_block_size) {
        auto          handle = upload_allocator.Allocate(_size, _alignment);
        VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle.handle);
        return {buffer, handle.offset, _size, 1u};
    }
    auto          handle = allocator.Allocate(_size, std::format("VkBackend::LargeUploadBuffer_{}", _size));
    VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle);
    large_buffers.push_back(buffer);
    return {buffer, 0, _size, 1u};
}

BufferView VulkanAllocator::AllocateReadbackBuffer(uint64 _size, uint _alignment) {
    _size = std::max<uint64>(_size, _alignment);
    if (_size < small_block_size) {
        auto          handle = readback_allocator.Allocate(_size, _alignment);
        VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle.handle);
        return {buffer, handle.offset, _size, 1u};
    }
    auto          handle = allocator.Allocate(_size, std::format("VkBackend::LargeReadbackBuffer_{}", _size));
    VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle);
    large_buffers.push_back(buffer);

    return {buffer, 0, _size, 1u};
}

void VulkanAllocator::ResetBufferAlloc() {
    upload_allocator.Reset();
    readback_allocator.Reset();
    for (auto& handle : large_buffers) {
        allocator.DeAllocate(reinterpret_cast<uint64>(handle));
    }
    large_buffers.clear();
    scratch_allocator.Reset();
    shader_buffer_allocator.Reset();
}

VulkanAllocator::TimestampQueryRecord*
VulkanAllocator::FindTimestampQueryRecord(uint64 _token_id) noexcept {
    const auto iter = std::find_if(
        timestamp_query_records.begin(),
        timestamp_query_records.end(),
        [_token_id](const TimestampQueryRecord& _record) {
            return _record.token.Id() == _token_id;
        }
    );
    return iter == timestamp_query_records.end() ? nullptr : &*iter;
}

VulkanAllocator::OcclusionQueryRecord*
VulkanAllocator::FindOcclusionQueryRecord(uint64 _token_id) noexcept {
    const auto iter = std::find_if(
        occlusion_query_records.begin(),
        occlusion_query_records.end(),
        [_token_id](const OcclusionQueryRecord& _record) {
            return _record.token.Id() == _token_id;
        }
    );
    return iter == occlusion_query_records.end() ? nullptr : &*iter;
}

void VulkanAllocator::EnsureQueryCapacity(
    size_t _timestamp_slot_count,
    size_t _occlusion_pair_count
) {
    EnsureTimestampQueryCapacity(_timestamp_slot_count);
    EnsureOcclusionQueryCapacity(_occlusion_pair_count);
}

void VulkanAllocator::EnsureTimestampQueryCapacity(size_t _required_count) {
    if (_required_count == 0) {
        return;
    }
    if (next_timestamp_query != 0 || !timestamp_query_records.empty()) {
        throw std::logic_error(
            "Vulkan timestamp query pool can grow only before recording"
        );
    }
    if (timestamp_valid_bits == 0) {
        throw std::runtime_error(
            "Vulkan queue family does not support timestamp queries"
        );
    }
    if (_required_count > std::numeric_limits<uint32>::max()) {
        throw std::length_error(
            "Vulkan timestamp query slot count exceeds uint32 capacity"
        );
    }

    const uint32 required_count = static_cast<uint32>(_required_count);
    // Reserve every CPU-side record before native recording begins. A Query
    // begin writes commands immediately; allowing emplace_back to allocate
    // afterwards could turn an otherwise legal packet into a partial native
    // recording that cannot be replayed safely.
    timestamp_query_records.reserve(_required_count);
    if (!timestamp_pool) {
        timestamp_pool.emplace(
            *m_device,
            VK_QUERY_TYPE_TIMESTAMP,
            required_count,
            queue_type
        );
        return;
    }
    timestamp_pool->EnsureCapacity(required_count);
}

void VulkanAllocator::EnsureOcclusionQueryCapacity(size_t _required_count) {
    if (_required_count == 0) {
        return;
    }
    if (queue_type != EQueueType::Graphics) {
        throw std::runtime_error(
            "Vulkan occlusion queries require a Graphics allocator"
        );
    }
    if (next_occlusion_query != 0 || !occlusion_query_records.empty()) {
        throw std::logic_error(
            "Vulkan occlusion query pool can grow only before recording"
        );
    }
    if (_required_count > std::numeric_limits<uint32>::max()) {
        throw std::length_error(
            "Vulkan occlusion query pair count exceeds uint32 capacity"
        );
    }

    const uint32 required_count = static_cast<uint32>(_required_count);
    occlusion_query_records.reserve(_required_count);
    if (!occlusion_pool) {
        occlusion_pool.emplace(
            *m_device,
            VK_QUERY_TYPE_OCCLUSION,
            required_count,
            queue_type
        );
        return;
    }
    occlusion_pool->EnsureCapacity(required_count);
}

uint32 VulkanAllocator::AllocateTimestampQuerySlot() {
    if (!timestamp_pool ||
        next_timestamp_query >= timestamp_pool->GetCount()) {
        throw std::runtime_error(
            "Vulkan timestamp query pool exhausted for one recorded command buffer"
        );
    }
    return next_timestamp_query++;
}

uint32 VulkanAllocator::AllocateOcclusionQuerySlot() {
    if (!occlusion_pool ||
        next_occlusion_query >= occlusion_pool->GetCount()) {
        throw std::runtime_error(
            "Vulkan occlusion query pool exhausted for one recorded command buffer"
        );
    }
    return next_occlusion_query++;
}

void VulkanAllocator::RecordQuery(
    VulkanCmdList& _cmd_list,
    const QueryCmd& _cmd
) {
    const QueryToken& token = _cmd.Token();
    if (!token.Valid()) {
        throw std::runtime_error("invalid Vulkan query token");
    }

    switch (token.Kind()) {
        case QueryKind::Timestamp:
            if (!_cmd.IsTimestamp()) {
                throw std::runtime_error(
                    "Vulkan timestamp query token has an occlusion operation"
                );
            }
            RecordTimestampQuery(_cmd_list, _cmd);
            return;
        case QueryKind::Occlusion:
            if (!_cmd.IsOcclusion()) {
                throw std::runtime_error(
                    "Vulkan occlusion query token has a timestamp operation"
                );
            }
            RecordOcclusionQuery(_cmd_list, _cmd);
            return;
        default:
            throw std::runtime_error("unsupported Vulkan query kind");
    }
}

void VulkanAllocator::RecordTimestampQuery(
    VulkanCmdList& _cmd_list,
    const QueryCmd& _cmd
) {
    const QueryToken& token = _cmd.Token();
    if (!token.Valid() || token.Kind() != QueryKind::Timestamp) {
        throw std::runtime_error("invalid Vulkan timestamp query token");
    }
    if (timestamp_valid_bits == 0) {
        throw std::runtime_error(
            "Vulkan queue family does not support timestamp queries"
        );
    }

    constexpr uint32 invalid_slot = std::numeric_limits<uint32>::max();
    if (_cmd.IsBegin()) {
        if (FindTimestampQueryRecord(token.Id()) != nullptr) {
            throw std::runtime_error("duplicate Vulkan timestamp query begin");
        }
        const uint32 slot = AllocateTimestampQuerySlot();
        _cmd_list.ResetQueryPool(*timestamp_pool, slot, 1);
        _cmd_list.WriteTimeStamp(
            *timestamp_pool, slot, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
        );
        timestamp_query_records.emplace_back(TimestampQueryRecord{
            .token      = token,
            .begin_slot = slot,
            .end_slot   = invalid_slot,
        });
        return;
    }

    TimestampQueryRecord* record = FindTimestampQueryRecord(token.Id());
    if (record == nullptr || record->begin_slot == invalid_slot) {
        throw std::runtime_error(
            "Vulkan timestamp query end has no matching begin"
        );
    }
    if (record->end_slot != invalid_slot) {
        throw std::runtime_error("duplicate Vulkan timestamp query end");
    }
    const uint32 slot = AllocateTimestampQuerySlot();
    _cmd_list.ResetQueryPool(*timestamp_pool, slot, 1);
    _cmd_list.WriteTimeStamp(
        *timestamp_pool, slot, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT
    );
    record->end_slot = slot;
}

void VulkanAllocator::RecordOcclusionQuery(
    VulkanCmdList& _cmd_list,
    const QueryCmd& _cmd
) {
    const QueryToken& token = _cmd.Token();
    if (!token.Valid() || token.Kind() != QueryKind::Occlusion) {
        throw std::runtime_error("invalid Vulkan occlusion query token");
    }
    if (queue_type != EQueueType::Graphics ||
        _cmd.GetQueueType() != EQueueType::Graphics) {
        throw std::runtime_error(
            "Vulkan occlusion queries require the Graphics queue"
        );
    }

    constexpr uint32 invalid_slot = std::numeric_limits<uint32>::max();
    if (_cmd.IsBegin()) {
        if (FindOcclusionQueryRecord(token.Id()) != nullptr) {
            throw std::runtime_error("duplicate Vulkan occlusion query begin");
        }
        const uint32 slot = AllocateOcclusionQuerySlot();
        // VulkanDevice enables the complete supported core feature set at
        // device creation. Preserve visibility-only support on devices
        // without occlusionQueryPrecise, but do not expose their
        // implementation-defined nonzero value as an exact sample count.
        const bool precise =
            m_device->GetCoreFeatures().core_1_0.occlusionQueryPrecise ==
            VK_TRUE;
        const VkQueryControlFlags flags =
            precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
        _cmd_list.ResetQueryPool(*occlusion_pool, slot, 1);
        _cmd_list.BeginQuery(*occlusion_pool, slot, flags);
        occlusion_query_records.emplace_back(OcclusionQueryRecord{
            .token   = token,
            .slot    = slot,
            .precise = precise,
        });
        return;
    }

    OcclusionQueryRecord* record =
        FindOcclusionQueryRecord(token.Id());
    if (record == nullptr || record->slot == invalid_slot) {
        throw std::runtime_error(
            "Vulkan occlusion query end has no matching begin"
        );
    }
    if (record->ended) {
        throw std::runtime_error("duplicate Vulkan occlusion query end");
    }
    _cmd_list.EndQuery(*occlusion_pool, record->slot);
    record->ended = true;
}

VkResult VulkanAllocator::PrepareTimestampQueriesAfterGpuCompletion(
    const VulkanOperationContext& _context
) noexcept {
    constexpr uint32 invalid_slot = std::numeric_limits<uint32>::max();
    const uint32 valid_bits = std::min(timestamp_valid_bits, uint32{64});
    const uint64 valid_mask =
        valid_bits == 64 ? std::numeric_limits<uint64>::max() :
        valid_bits == 0  ? 0 :
                           (uint64{1} << valid_bits) - 1;

    struct NativeTimestampResult {
        uint64 value{0};
        uint64 available{0};
    };

    auto resolve_slot = [&](uint32 _slot, uint64& _value) noexcept {
        NativeTimestampResult native_result{};
        if (!timestamp_pool) {
            return VK_ERROR_UNKNOWN;
        }
        const VkResult result = vkGetQueryPoolResults(
            m_device->GetDevice(),
            timestamp_pool->GetHandle(),
            _slot,
            1,
            sizeof(native_result),
            &native_result,
            sizeof(native_result),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
        );
        if (result == VK_SUCCESS) {
            if (native_result.available == 0) {
                return VK_NOT_READY;
            }
            _value = native_result.value & valid_mask;
            return VK_SUCCESS;
        }
        if (result != VK_NOT_READY) {
            VulkanOperationContext query_context = _context;
            query_context.operation = EVulkanFaultOperation::QueryPoolResults;
            if (query_context.queue_type == EQueueType::Ignore) {
                query_context.queue_type = queue_type;
            }
            m_device->TryLatchFirstFault(query_context, result);
        }
        return result;
    };

    for (TimestampQueryRecord& record : timestamp_query_records) {
        record.prepared_state =
            TimestampQueryRecord::EPreparedState::Unprepared;
        record.prepared_result = {};

        if (valid_bits == 0 || record.begin_slot == invalid_slot ||
            record.end_slot == invalid_slot) {
            record.prepared_state =
                valid_bits == 0 ?
                    TimestampQueryRecord::EPreparedState::InvalidValidBits :
                    TimestampQueryRecord::EPreparedState::Incomplete;
            continue;
        }

        uint64 begin_tick = 0;
        uint64 end_tick   = 0;
        const VkResult begin_result =
            resolve_slot(record.begin_slot, begin_tick);
        if (begin_result != VK_SUCCESS) {
            record.prepared_state =
                begin_result == VK_NOT_READY ?
                    TimestampQueryRecord::EPreparedState::Unavailable :
                    TimestampQueryRecord::EPreparedState::NativeFault;
            if (begin_result != VK_NOT_READY) {
                return begin_result;
            }
            continue;
        }

        const VkResult end_result =
            resolve_slot(record.end_slot, end_tick);
        if (end_result != VK_SUCCESS) {
            record.prepared_state =
                end_result == VK_NOT_READY ?
                    TimestampQueryRecord::EPreparedState::Unavailable :
                    TimestampQueryRecord::EPreparedState::NativeFault;
            if (end_result != VK_NOT_READY) {
                return end_result;
            }
            continue;
        }

        const uint64 delta_tick = (end_tick - begin_tick) & valid_mask;
        record.prepared_result = TimestampQueryResult{
            .begin_tick     = begin_tick,
            .end_tick       = end_tick,
            .valid_bits     = valid_bits,
            .tick_period_ns = timestamp_period_ns,
            .duration_ns    = static_cast<double>(delta_tick) *
                           timestamp_period_ns,
        };
        record.prepared_state =
            TimestampQueryRecord::EPreparedState::Ready;
    }
    return VK_SUCCESS;
}

VkResult VulkanAllocator::PrepareOcclusionQueriesAfterGpuCompletion(
    const VulkanOperationContext& _context
) noexcept {
    constexpr uint32 invalid_slot = std::numeric_limits<uint32>::max();

    struct NativeOcclusionResult {
        uint64 sample_count{0};
        uint64 available{0};
    };

    for (OcclusionQueryRecord& record : occlusion_query_records) {
        record.prepared_state =
            OcclusionQueryRecord::EPreparedState::Unprepared;
        record.prepared_result = {};

        if (record.slot == invalid_slot || !record.ended) {
            record.prepared_state =
                OcclusionQueryRecord::EPreparedState::Incomplete;
            continue;
        }

        NativeOcclusionResult native_result{};
        if (!occlusion_pool) {
            record.prepared_state =
                OcclusionQueryRecord::EPreparedState::NativeFault;
            return VK_ERROR_UNKNOWN;
        }
        const VkResult result = vkGetQueryPoolResults(
            m_device->GetDevice(),
            occlusion_pool->GetHandle(),
            record.slot,
            1,
            sizeof(native_result),
            &native_result,
            sizeof(native_result),
            VK_QUERY_RESULT_64_BIT |
                VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
        );
        if (result == VK_NOT_READY ||
            (result == VK_SUCCESS && native_result.available == 0)) {
            record.prepared_state =
                OcclusionQueryRecord::EPreparedState::Unavailable;
            continue;
        }
        if (result != VK_SUCCESS) {
            record.prepared_state =
                OcclusionQueryRecord::EPreparedState::NativeFault;
            VulkanOperationContext query_context = _context;
            query_context.operation = EVulkanFaultOperation::QueryPoolResults;
            if (query_context.queue_type == EQueueType::Ignore) {
                query_context.queue_type = queue_type;
            }
            m_device->TryLatchFirstFault(query_context, result);
            return result;
        }

        record.prepared_result = OcclusionQueryResult{
            .sample_count = record.precise ?
                                std::optional<uint64>{
                                    native_result.sample_count
                                } :
                                std::nullopt,
            .visible      = native_result.sample_count != 0,
        };
        record.prepared_state =
            OcclusionQueryRecord::EPreparedState::Ready;
    }
    return VK_SUCCESS;
}

VkResult VulkanAllocator::PrepareQueriesAfterGpuCompletion(
    const VulkanOperationContext& _context
) noexcept {
    const VkResult timestamp_result =
        PrepareTimestampQueriesAfterGpuCompletion(_context);
    if (timestamp_result != VK_SUCCESS) {
        return timestamp_result;
    }
    return PrepareOcclusionQueriesAfterGpuCompletion(_context);
}

void VulkanAllocator::PublishTimestampQueriesAfterGpuCompletion(
    QueryPublishBatch _batch,
    bool              _gpu_success,
    std::string_view  _failure_reason
) noexcept {
    for (const TimestampQueryRecord& record : timestamp_query_records) {
        if (!_gpu_success) {
            QueryBackendAccess::PublishErrorIfPending(
                record.token,
                _failure_reason.empty() ?
                    "Vulkan submission did not reach successful GPU completion" :
                    _failure_reason,
                _batch
            );
            continue;
        }

        switch (record.prepared_state) {
            case TimestampQueryRecord::EPreparedState::Ready:
                QueryBackendAccess::PublishTimestamp(
                    record.token, record.prepared_result, _batch
                );
                break;
            case TimestampQueryRecord::EPreparedState::InvalidValidBits:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token,
                    "Vulkan queue family has timestampValidBits == 0",
                    _batch
                );
                break;
            case TimestampQueryRecord::EPreparedState::Incomplete:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token,
                    "Vulkan timestamp query pair is incomplete",
                    _batch
                );
                break;
            case TimestampQueryRecord::EPreparedState::Unavailable:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token,
                    "Vulkan timestamp query result is unavailable",
                    _batch
                );
                break;
            case TimestampQueryRecord::EPreparedState::NativeFault:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token,
                    "Vulkan timestamp query result readback faulted",
                    _batch
                );
                break;
            case TimestampQueryRecord::EPreparedState::Unprepared:
            default:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token,
                    "Vulkan timestamp query was not prepared for completion",
                    _batch
                );
                break;
        }
    }
}

void VulkanAllocator::PublishOcclusionQueriesAfterGpuCompletion(
    QueryPublishBatch _batch,
    bool              _gpu_success,
    std::string_view  _failure_reason
) noexcept {
    for (const OcclusionQueryRecord& record : occlusion_query_records) {
        if (!_gpu_success) {
            QueryBackendAccess::PublishErrorIfPending(
                record.token,
                _failure_reason.empty() ?
                    "Vulkan submission did not reach successful GPU completion" :
                    _failure_reason,
                _batch
            );
            continue;
        }

        switch (record.prepared_state) {
            case OcclusionQueryRecord::EPreparedState::Ready:
                QueryBackendAccess::PublishOcclusion(
                    record.token, record.prepared_result, _batch
                );
                break;
            case OcclusionQueryRecord::EPreparedState::Incomplete:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token,
                    "Vulkan occlusion query pair is incomplete",
                    _batch
                );
                break;
            case OcclusionQueryRecord::EPreparedState::Unavailable:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token,
                    "Vulkan occlusion query result is unavailable",
                    _batch
                );
                break;
            case OcclusionQueryRecord::EPreparedState::NativeFault:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token,
                    "Vulkan occlusion query result readback faulted",
                    _batch
                );
                break;
            case OcclusionQueryRecord::EPreparedState::Unprepared:
            default:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token,
                    "Vulkan occlusion query was not prepared for completion",
                    _batch
                );
                break;
        }
    }
}

void VulkanAllocator::PublishQueriesAfterGpuCompletion(
    QueryPublishBatch _batch,
    bool              _gpu_success,
    std::string_view  _failure_reason
) noexcept {
    // Publish every token in the allocator before releasing any callback.
    // A callback may synchronously inspect another query from this packet.
    PublishTimestampQueriesAfterGpuCompletion(
        _batch, _gpu_success, _failure_reason
    );
    PublishOcclusionQueriesAfterGpuCompletion(
        _batch, _gpu_success, _failure_reason
    );
}

void VulkanAllocator::NotifyTimestampQueriesAfterGpuCompletion(
    QueryPublishBatch _batch
) noexcept {
    for (const TimestampQueryRecord& record : timestamp_query_records) {
        QueryBackendAccess::NotifyTerminal(record.token, _batch);
    }
}

void VulkanAllocator::NotifyOcclusionQueriesAfterGpuCompletion(
    QueryPublishBatch _batch
) noexcept {
    for (const OcclusionQueryRecord& record : occlusion_query_records) {
        QueryBackendAccess::NotifyTerminal(record.token, _batch);
    }
}

void VulkanAllocator::NotifyQueriesAfterGpuCompletion(
    QueryPublishBatch _batch
) noexcept {
    NotifyTimestampQueriesAfterGpuCompletion(_batch);
    NotifyOcclusionQueriesAfterGpuCompletion(_batch);
}

bool VulkanAllocator::CompleteSuccessCallbacks() noexcept {
    return InvokeAllocatorCompletionCallbacksNoexcept(
        on_complete, "allocator"
    );
}

void VulkanAllocator::ClearQueries() noexcept {
    timestamp_query_records.clear();
    occlusion_query_records.clear();
    next_timestamp_query = 0;
    next_occlusion_query = 0;
}

bool VulkanAllocator::CompleteSuccess() noexcept {
    VulkanOperationContext context{
        .operation  = EVulkanFaultOperation::QueryPoolResults,
        .queue_type = queue_type,
    };
    const VkResult prepare_result =
        PrepareQueriesAfterGpuCompletion(context);
    const bool callbacks_succeeded =
        prepare_result == VK_SUCCESS && CompleteSuccessCallbacks();
    const QueryPublishBatch query_batch =
        QueryBackendAccess::BeginPublishBatch();
    PublishQueriesAfterGpuCompletion(
        query_batch,
        prepare_result == VK_SUCCESS,
        "Vulkan query result preparation failed"
    );
    NotifyQueriesAfterGpuCompletion(query_batch);
    return prepare_result == VK_SUCCESS && callbacks_succeeded;
}

bool VulkanAllocator::Reset() {
    if (!ResetCmdList()) {
        return false;
    }
    ResetBufferAlloc();
    ClearQueries();
    return true;
}

VkTmpBufferAllocator::VkTmpBufferAllocator(VulkanDevice* _device) : VulkanDeviceObject(_device) {}
uint64 VkTmpBufferAllocator::Allocate(uint64 _size, std::string_view _name) {
    VkBufferCreateInfo buffer_info = {
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .size                  = _size,
        .usage                 = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr
    };

#if WITH_CUDA
    buffer_info.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

    VmaAllocationCreateInfo alloc_info{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, .usage = VMA_MEMORY_USAGE_AUTO
    };
    VulkanBuffer::BufferAlloc buffer_alloc;
    BufferInfo                info(_size, 1, EBufferUsageFlags::CPU_VISIBLE);

    VK_CHECK_RESULT(vmaCreateBuffer(
        m_device->GetVmaAllocator(),
        &buffer_info,
        &alloc_info,
        &buffer_alloc.buffer,
        &buffer_alloc.alloc,
        nullptr
    ));
    VulkanBuffer* vk_buffer =
        MoerNew(VulkanBuffer)(_name, info, *m_device, buffer_alloc.buffer, buffer_alloc.alloc, false);

    return reinterpret_cast<uint64>(vk_buffer);
}

uint64 VkTmpBufferAllocator::Allocate(uint64 _size, EVkInternalBufferUsage _usage) {

    VkBufferUsageFlags       usage{};
    VmaAllocationCreateFlags flags{};
    VmaMemoryUsage           mem_usage     = VMA_MEMORY_USAGE_AUTO;
    bool                     devce_address = false;
    switch (_usage) {

        case EVkInternalBufferUsage::Upload:
        case EVkInternalBufferUsage::Readback: {
            usage     = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            flags     = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            mem_usage = VMA_MEMORY_USAGE_AUTO;
            break;
        }
        case EVkInternalBufferUsage::Scratch: {
            usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            flags         = VMA_ALLOCATION_CREATE_STRATEGY_BEST_FIT_BIT;
            mem_usage     = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            devce_address = true;
            break;
        }
        case EVkInternalBufferUsage::ShaderBuffer: {
            usage         = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            flags         = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            mem_usage     = VMA_MEMORY_USAGE_AUTO;
            devce_address = true;
            break;
        }
        case EVkInternalBufferUsage::ShaderBuffer_Constant: {
            usage         = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            flags         = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            mem_usage     = VMA_MEMORY_USAGE_AUTO;
            devce_address = true;
            break;
        }
    }
    VkBufferCreateInfo buffer_info = {
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .size                  = _size,
        .usage                 = usage,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr
    };

#if WITH_CUDA
    buffer_info.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif

    VmaAllocationCreateInfo   alloc_info{.flags = flags, .usage = mem_usage};
    VulkanBuffer::BufferAlloc buffer_alloc;
    BufferInfo                info(_size, 1, EBufferUsageFlags::NONE);

    VK_CHECK_RESULT(vmaCreateBuffer(
        m_device->GetVmaAllocator(),
        &buffer_info,
        &alloc_info,
        &buffer_alloc.buffer,
        &buffer_alloc.alloc,
        nullptr
    ));

    static constexpr std::string_view usage_str[] = {
        "VkBackend::Upload",
        "VkBackend::Readback",
        "VkBackend::Scratch",
        "VkBackend::ShaderBuffer",
        "VkBackend::ShaderBuffer_Constant"
    };
    VulkanBuffer* vk_buffer = MoerNew(VulkanBuffer)(
        usage_str[uint(_usage)],
        info,
        *m_device,
        buffer_alloc.buffer,
        buffer_alloc.alloc,
        false,
        devce_address
    );
    return reinterpret_cast<uint64>(vk_buffer);
}

void VkTmpBufferAllocator::DeAllocate(uint64 _buffer) {
    auto* buffer = reinterpret_cast<VulkanBuffer*>(_buffer);
    MoerDelete(buffer);
}

VulkanAllocator::ScratchAllocator::ScratchAllocator(VulkanDevice* _device, VkTmpBufferAllocator* _allocator) :
    VulkanDeviceObject(_device),
    allocator(_allocator) {
    alignment = _device->GetOptionalProperties()
                    .acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;
}

uint64 VulkanAllocator::ScratchAllocator::Allocate(uint64 _size) {
    uint64 handle = allocator->Allocate(_size, EVkInternalBufferUsage::Scratch);
    allocated_buffers.push_back(handle);
    return handle;
}

BufferView VulkanAllocator::AllocateScratch(uint64 _size) {
    auto          handle = scratch_allocator.Allocate(_size);
    VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle);
    return {buffer, 0, buffer->GetNumElement(), 1u};
}

void VulkanAllocator::ScratchAllocator::Deallocate(uint64 _handle) {
    allocator->DeAllocate(_handle);
}

void VulkanAllocator::ScratchAllocator::Reset() {
    for (uint64 handle : allocated_buffers) {
        Deallocate(handle);
    }
    allocated_buffers.clear();
}

VulkanAllocator::StackAllocator::StackAllocator(
    VkTmpBufferAllocator* _alloc,
    uint64                _init_cap,
    double                _grouth_factor
) :
    allocator(_alloc),
    init_capacity(_init_cap),
    growth_factor(_grouth_factor),
    stack_memory_id(0) {
    capacity = std::max<uint64>(init_capacity, 1);
    allocated_buffers.push_back({allocator->Allocate(capacity, GetStackBufferName()), capacity, 0});
}

VulkanAllocator::StackAllocator::Chunk VulkanAllocator::StackAllocator::Allocate(uint64 _size, uint _align) {

    uint64 align_size = std::max(_align, 16u);
    for (auto& alloc_buf : allocated_buffers) {
        auto offset = Moer::AlignUp(alloc_buf.offset, align_size);
        if (alloc_buf.size - offset >= _size) {
            alloc_buf.offset = offset + _size;
            return {alloc_buf.handle, offset};
        }
    }

    align_size = std::max(align_size, _size);

    if (capacity < align_size) {
        capacity = std::max<uint64>(capacity * growth_factor, align_size);
    }

    auto buffer = allocator->Allocate(capacity, GetStackBufferName());
    allocated_buffers.push_back({buffer, capacity, align_size});
    return {buffer, 0};
}

VulkanAllocator::StackAllocator::Chunk VulkanAllocator::StackAllocator::Allocate(uint64 _size) {
    for (auto& alloc_buf : allocated_buffers) {
        if (alloc_buf.size - alloc_buf.offset >= _size) {
            auto offset = alloc_buf.offset;
            alloc_buf.offset += _size;
            return {alloc_buf.handle, offset};
        }
    }

    if (capacity < _size) {
        capacity = std::max<uint64>(capacity * growth_factor, _size);
    }

    auto buffer = allocator->Allocate(capacity, GetStackBufferName());
    allocated_buffers.push_back({buffer, capacity, _size});
    return {buffer, 0};
}

void VulkanAllocator::StackAllocator::Reset() {
    if (allocated_buffers.size() == 1) {
        allocated_buffers.back().offset = 0;
    }
    if (allocated_buffers.size() > 1) {
        //pack all staging buffer to one
        uint64 sum_size = 0;
        for (auto& alloc_buf : allocated_buffers) {
            sum_size += alloc_buf.size;
            allocator->DeAllocate(alloc_buf.handle);
        }
        allocated_buffers.clear();

        stack_memory_id = 0;

        allocated_buffers.push_back({allocator->Allocate(sum_size, GetStackBufferName()), sum_size, 0});
    }
}

void VulkanAllocator::StackAllocator::Dispose() {
    for (auto& alloc_buf : allocated_buffers) {
        allocator->DeAllocate(alloc_buf.handle);
    }
    allocated_buffers.clear();
}

std::string VulkanAllocator::StackAllocator::GetStackBufferName() {
    return std::format("VkBackend::StackAllocBuffer_{}", stack_memory_id++);
}

VulkanAllocator::ShaderBufferAllocator::ShaderBufferAllocator(
    VulkanDevice*         _device,
    VkTmpBufferAllocator* _allocator
) :
    VulkanDeviceObject(_device),
    allocator(_allocator) {}

uint64 VulkanAllocator::ShaderBufferAllocator::Allocate(uint64 _size) {
    uint64 handle = allocator->Allocate(_size, EVkInternalBufferUsage::ShaderBuffer);
    allocated_buffers.push_back(handle);
    return handle;
}

void VulkanAllocator::ShaderBufferAllocator::Deallocate(uint64 _handle) {
    allocator->DeAllocate(_handle);
}

void VulkanAllocator::ShaderBufferAllocator::Reset() {
    for (uint64 handle : allocated_buffers) {
        Deallocate(handle);
    }
    allocated_buffers.clear();
}

BufferView VulkanAllocator::AllocateShaderBuffer(uint64 _size) {
    auto          handle = shader_buffer_allocator.Allocate(_size);
    VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(handle);
    return {buffer, 0, buffer->GetNumElement(), 1u};
}

VulkanCmdAllocator::VulkanCmdAllocator(VulkanDevice* _device, EQueueType _queue_type) :
    VulkanDeviceObject(_device),
    queue_type(VulkanEnumTranslator::METoVKQueueFlagBits(_queue_type)) {
    // 必须使用 GetQueueFamilyIndex(EQueueType) 而非 GetQueueFamilyIndex(VkQueueFlags)。
    // 后者根据硬件能力搜索，可能返回不同的 family（如 AMD 上返回 compute family），
    // 而前者使用引擎已确定的、可能经过 override 的 family index。
    uint32_t family_index = m_device->GetQueueFamilyIndex(_queue_type);

    VkCommandPoolCreateInfo pool_info = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .queueFamilyIndex = family_index
    };

    VK_CHECK_RESULT(vkCreateCommandPool(_device->GetDevice(), &pool_info, nullptr, &command_pool));
}

VulkanCmdAllocator::~VulkanCmdAllocator() {
    if (command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device->GetDevice(), command_pool, nullptr);
        command_pool = VK_NULL_HANDLE;
    }
}
} // namespace Moer::Render
