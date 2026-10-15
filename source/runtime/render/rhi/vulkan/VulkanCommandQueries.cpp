#include "VulkanCommandQueries.h"

#include "VulkanCommand.h"
#include "VulkanDevice.h"
#include "rhi/RHIImpl.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Moer::Render {

VkNativeQueryPool::VkNativeQueryPool(
    VulkanDevice& _device,
    VkQueryType   _type,
    uint32        _query_count,
    EQueueType    _queue_type
) :
    device(_device),
    type(_type),
    queue_type(_queue_type) {
    query_pool = CreatePool(_query_count);
    count      = _query_count;
}

VkQueryPool VkNativeQueryPool::CreatePool(uint32 _query_count) {
    if (_query_count == 0) {
        throw std::invalid_argument("Vulkan query-pool creation requires at least one slot");
    }

    VkQueryPoolCreateInfo create_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    create_info.queryType   = type;
    create_info.queryCount  = _query_count;
    VkQueryPool    new_pool = VK_NULL_HANDLE;
    const VkResult result   = vkCreateQueryPool(device.GetDevice(), &create_info, nullptr, &new_pool);
    if (result != VK_SUCCESS) {
        device.TryLatchFirstFault(
            VulkanOperationContext{
                .operation  = EVulkanFaultOperation::QueryPoolCreate,
                .queue_type = queue_type,
            },
            result
        );
        throw std::runtime_error(
            "Vulkan query-pool creation failed: " + std::string(VulkanResultName(result))
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
        throw std::out_of_range("Vulkan query result range exceeds the native pool");
    }

    const size_t values_per_query = (_flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0 ? 2u : 1u;
    if (_query_count > _data.size() / values_per_query) {
        throw std::invalid_argument("Vulkan query result span is too small for the requested flags");
    }
    const VkDeviceSize stride = sizeof(uint64) * static_cast<VkDeviceSize>(values_per_query);
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

void VkNativeQueryPool::ResetHost(uint32 _first_query, uint32 _query_count) {
    if (_query_count == 0) {
        return;
    }
    if (_first_query > count || _query_count > count - _first_query) {
        throw std::out_of_range("Vulkan host query reset range exceeds the native pool");
    }
    vkResetQueryPool(device.GetDevice(), query_pool, _first_query, _query_count);
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
    // owning VulkanCommandQueries calls this only before beginning native record.
    VkQueryPool replacement = CreatePool(grown_count);
    if (query_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device.GetDevice(), query_pool, nullptr);
    }
    query_pool = replacement;
    count      = grown_count;
}

VulkanCommandQueries::VulkanCommandQueries(VulkanDevice& _device, EQueueType _queue_type) :
    device(_device),
    queue_type(_queue_type),
    timestamp_valid_bits(_device.GetTimestampValidBits(_queue_type)),
    timestamp_reset_mode(_device.GetTimestampQueryResetMode(_queue_type)),
    timestamp_period_ns(_device.GetCoreProperties().core_1_0.limits.timestampPeriod) {}

void VulkanCommandQueries::EnsureQueryCapacity(size_t _timestamp_slot_count, size_t _occlusion_pair_count) {
    EnsureTimestampQueryCapacity(_timestamp_slot_count);
    EnsureOcclusionQueryCapacity(_occlusion_pair_count);
}

void VulkanCommandQueries::ConfigureTimestampCapabilityForRecording(
    uint32                         _valid_bits,
    EVulkanTimestampQueryResetMode _reset_mode
) {
    if (next_timestamp_query != 0 || !timestamp_query_records.empty()) {
        throw std::logic_error("Vulkan timestamp capability can change only before recording");
    }
    timestamp_valid_bits = std::min(_valid_bits, uint32{64});
    const EVulkanTimestampQueryResetMode expected_mode =
        device.GetTimestampQueryResetMode(queue_type, timestamp_valid_bits);
    if (_reset_mode != expected_mode) {
        throw std::invalid_argument("Vulkan timestamp reset mode does not match queue capability");
    }
    timestamp_reset_mode = _reset_mode;
}

void VulkanCommandQueries::RecordQuery(VulkanCmdList& _cmd_list, const QueryCmd& _cmd) {
    const QueryToken& token = _cmd.Token();
    if (!token.Valid()) {
        throw std::runtime_error("invalid Vulkan query token");
    }

    switch (token.Kind()) {
        case QueryKind::Timestamp:
            if (!_cmd.IsTimestamp()) {
                throw std::runtime_error("Vulkan timestamp query token has an occlusion operation");
            }
            RecordTimestampQuery(_cmd_list, _cmd);
            return;
        case QueryKind::Occlusion:
            if (!_cmd.IsOcclusion()) {
                throw std::runtime_error("Vulkan occlusion query token has a timestamp operation");
            }
            RecordOcclusionQuery(_cmd_list, _cmd);
            return;
        default:
            throw std::runtime_error("unsupported Vulkan query kind");
    }
}

VkResult
VulkanCommandQueries::PrepareQueriesAfterGpuCompletion(const VulkanOperationContext& _context) noexcept {
    const VkResult timestamp_result = PrepareTimestampQueriesAfterGpuCompletion(_context);
    if (timestamp_result != VK_SUCCESS) {
        return timestamp_result;
    }
    return PrepareOcclusionQueriesAfterGpuCompletion(_context);
}

void VulkanCommandQueries::PublishQueriesAfterGpuCompletion(
    QueryPublishBatch _batch,
    bool              _gpu_success,
    std::string_view  _failure_reason
) noexcept {
    // Publish every token in the allocator before releasing any callback.
    // A callback may synchronously inspect another query from this packet.
    PublishTimestampQueriesAfterGpuCompletion(_batch, _gpu_success, _failure_reason);
    PublishOcclusionQueriesAfterGpuCompletion(_batch, _gpu_success, _failure_reason);
}

void VulkanCommandQueries::NotifyQueriesAfterGpuCompletion(QueryPublishBatch _batch) noexcept {
    NotifyTimestampQueriesAfterGpuCompletion(_batch);
    NotifyOcclusionQueriesAfterGpuCompletion(_batch);
}

void VulkanCommandQueries::ClearRecords() noexcept {
    timestamp_query_records.clear();
    occlusion_query_records.clear();
    next_timestamp_query = 0;
    next_occlusion_query = 0;
}

VulkanCommandQueries::TimestampQueryRecord*
VulkanCommandQueries::FindTimestampQueryRecord(uint64 _token_id) noexcept {
    const auto iter = std::find_if(
        timestamp_query_records.begin(),
        timestamp_query_records.end(),
        [_token_id](const TimestampQueryRecord& _record) {
            return _record.token.Id() == _token_id;
        }
    );
    return iter == timestamp_query_records.end() ? nullptr : &*iter;
}

void VulkanCommandQueries::EnsureTimestampQueryCapacity(size_t _required_count) {
    if (_required_count == 0) {
        return;
    }
    if (next_timestamp_query != 0 || !timestamp_query_records.empty()) {
        throw std::logic_error("Vulkan timestamp query pool can grow only before recording");
    }
    if (timestamp_reset_mode == EVulkanTimestampQueryResetMode::Unsupported) {
        throw std::runtime_error("Vulkan queue family cannot safely reset timestamp queries");
    }
    if (_required_count > std::numeric_limits<uint32>::max()) {
        throw std::length_error("Vulkan timestamp query slot count exceeds uint32 capacity");
    }

    const uint32 required_count = static_cast<uint32>(_required_count);
    // Reserve every CPU-side record before native recording begins. A Query
    // begin writes commands immediately; allowing emplace_back to allocate
    // afterwards could turn an otherwise legal packet into a partial native
    // recording that cannot be replayed safely.
    timestamp_query_records.reserve(_required_count);
    if (!timestamp_pool) {
        timestamp_pool.emplace(device, VK_QUERY_TYPE_TIMESTAMP, required_count, queue_type);
    } else {
        timestamp_pool->EnsureCapacity(required_count);
    }
    if (timestamp_reset_mode == EVulkanTimestampQueryResetMode::Host) {
        timestamp_pool->ResetHost(0, required_count);
    }
}

uint32 VulkanCommandQueries::AllocateTimestampQuerySlot() {
    if (!timestamp_pool || next_timestamp_query >= timestamp_pool->GetCount()) {
        throw std::runtime_error("Vulkan timestamp query pool exhausted for one recorded command buffer");
    }
    return next_timestamp_query++;
}

void VulkanCommandQueries::RecordTimestampQuery(VulkanCmdList& _cmd_list, const QueryCmd& _cmd) {
    const QueryToken& token = _cmd.Token();
    if (!token.Valid() || token.Kind() != QueryKind::Timestamp) {
        throw std::runtime_error("invalid Vulkan timestamp query token");
    }
    if (_cmd.GetQueueType() != queue_type) {
        throw std::runtime_error("Vulkan timestamp query command queue does not match allocator");
    }
    if (timestamp_reset_mode == EVulkanTimestampQueryResetMode::Unsupported) {
        throw std::runtime_error("Vulkan timestamp query recording is unsupported on this queue");
    }

    constexpr uint32 invalid_slot = std::numeric_limits<uint32>::max();
    if (_cmd.IsBegin()) {
        if (FindTimestampQueryRecord(token.Id()) != nullptr) {
            throw std::runtime_error("duplicate Vulkan timestamp query begin");
        }
        const uint32 slot = AllocateTimestampQuerySlot();
        if (timestamp_reset_mode == EVulkanTimestampQueryResetMode::CommandBuffer) {
            _cmd_list.ResetQueryPool(*timestamp_pool, slot, 1);
        }
        _cmd_list.WriteTimeStamp(*timestamp_pool, slot, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
        timestamp_query_records.emplace_back(
            TimestampQueryRecord{
                .token      = token,
                .begin_slot = slot,
                .end_slot   = invalid_slot,
            }
        );
        return;
    }

    TimestampQueryRecord* record = FindTimestampQueryRecord(token.Id());
    if (record == nullptr || record->begin_slot == invalid_slot) {
        throw std::runtime_error("Vulkan timestamp query end has no matching begin");
    }
    if (record->end_slot != invalid_slot) {
        throw std::runtime_error("duplicate Vulkan timestamp query end");
    }
    const uint32 slot = AllocateTimestampQuerySlot();
    if (timestamp_reset_mode == EVulkanTimestampQueryResetMode::CommandBuffer) {
        _cmd_list.ResetQueryPool(*timestamp_pool, slot, 1);
    }
    _cmd_list.WriteTimeStamp(*timestamp_pool, slot, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    record->end_slot = slot;
}

VulkanCommandQueries::OcclusionQueryRecord*
VulkanCommandQueries::FindOcclusionQueryRecord(uint64 _token_id) noexcept {
    const auto iter = std::find_if(
        occlusion_query_records.begin(),
        occlusion_query_records.end(),
        [_token_id](const OcclusionQueryRecord& _record) {
            return _record.token.Id() == _token_id;
        }
    );
    return iter == occlusion_query_records.end() ? nullptr : &*iter;
}

void VulkanCommandQueries::EnsureOcclusionQueryCapacity(size_t _required_count) {
    if (_required_count == 0) {
        return;
    }
    if (queue_type != EQueueType::Graphics) {
        throw std::runtime_error("Vulkan occlusion queries require a Graphics allocator");
    }
    if (next_occlusion_query != 0 || !occlusion_query_records.empty()) {
        throw std::logic_error("Vulkan occlusion query pool can grow only before recording");
    }
    if (_required_count > std::numeric_limits<uint32>::max()) {
        throw std::length_error("Vulkan occlusion query pair count exceeds uint32 capacity");
    }

    const uint32 required_count = static_cast<uint32>(_required_count);
    occlusion_query_records.reserve(_required_count);
    if (!occlusion_pool) {
        occlusion_pool.emplace(device, VK_QUERY_TYPE_OCCLUSION, required_count, queue_type);
        return;
    }
    occlusion_pool->EnsureCapacity(required_count);
}

uint32 VulkanCommandQueries::AllocateOcclusionQuerySlot() {
    if (!occlusion_pool || next_occlusion_query >= occlusion_pool->GetCount()) {
        throw std::runtime_error("Vulkan occlusion query pool exhausted for one recorded command buffer");
    }
    return next_occlusion_query++;
}

void VulkanCommandQueries::RecordOcclusionQuery(VulkanCmdList& _cmd_list, const QueryCmd& _cmd) {
    const QueryToken& token = _cmd.Token();
    if (!token.Valid() || token.Kind() != QueryKind::Occlusion) {
        throw std::runtime_error("invalid Vulkan occlusion query token");
    }
    if (queue_type != EQueueType::Graphics || _cmd.GetQueueType() != EQueueType::Graphics) {
        throw std::runtime_error("Vulkan occlusion queries require the Graphics queue");
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
        const bool precise = device.GetCoreFeatures().GetCore10Features().occlusionQueryPrecise == VK_TRUE;
        const VkQueryControlFlags flags = precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
        _cmd_list.ResetQueryPool(*occlusion_pool, slot, 1);
        _cmd_list.BeginQuery(*occlusion_pool, slot, flags);
        occlusion_query_records.emplace_back(
            OcclusionQueryRecord{
                .token   = token,
                .slot    = slot,
                .precise = precise,
            }
        );
        return;
    }

    OcclusionQueryRecord* record = FindOcclusionQueryRecord(token.Id());
    if (record == nullptr || record->slot == invalid_slot) {
        throw std::runtime_error("Vulkan occlusion query end has no matching begin");
    }
    if (record->ended) {
        throw std::runtime_error("duplicate Vulkan occlusion query end");
    }
    _cmd_list.EndQuery(*occlusion_pool, record->slot);
    record->ended = true;
}

VkResult VulkanCommandQueries::PrepareTimestampQueriesAfterGpuCompletion(
    const VulkanOperationContext& _context
) noexcept {
    constexpr uint32 invalid_slot = std::numeric_limits<uint32>::max();
    const uint32     valid_bits   = std::min(timestamp_valid_bits, uint32{64});
    const uint64     valid_mask   = valid_bits == 64 ? std::numeric_limits<uint64>::max() :
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
            device.GetDevice(),
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
            query_context.operation              = EVulkanFaultOperation::QueryPoolResults;
            if (query_context.queue_type == EQueueType::Ignore) {
                query_context.queue_type = queue_type;
            }
            device.TryLatchFirstFault(query_context, result);
        }
        return result;
    };

    for (TimestampQueryRecord& record : timestamp_query_records) {
        record.prepared_state  = TimestampQueryRecord::EPreparedState::Unprepared;
        record.prepared_result = {};

        if (valid_bits == 0 || record.begin_slot == invalid_slot || record.end_slot == invalid_slot) {
            record.prepared_state = valid_bits == 0 ? TimestampQueryRecord::EPreparedState::InvalidValidBits :
                                                      TimestampQueryRecord::EPreparedState::Incomplete;
            continue;
        }

        uint64         begin_tick   = 0;
        uint64         end_tick     = 0;
        const VkResult begin_result = resolve_slot(record.begin_slot, begin_tick);
        if (begin_result != VK_SUCCESS) {
            record.prepared_state = begin_result == VK_NOT_READY ?
                                        TimestampQueryRecord::EPreparedState::Unavailable :
                                        TimestampQueryRecord::EPreparedState::NativeFault;
            if (begin_result != VK_NOT_READY) {
                return begin_result;
            }
            continue;
        }

        const VkResult end_result = resolve_slot(record.end_slot, end_tick);
        if (end_result != VK_SUCCESS) {
            record.prepared_state = end_result == VK_NOT_READY ?
                                        TimestampQueryRecord::EPreparedState::Unavailable :
                                        TimestampQueryRecord::EPreparedState::NativeFault;
            if (end_result != VK_NOT_READY) {
                return end_result;
            }
            continue;
        }

        const uint64 delta_tick = (end_tick - begin_tick) & valid_mask;
        record.prepared_result  = TimestampQueryResult{
             .begin_tick     = begin_tick,
             .end_tick       = end_tick,
             .valid_bits     = valid_bits,
             .tick_period_ns = timestamp_period_ns,
             .duration_ns    = static_cast<double>(delta_tick) * timestamp_period_ns,
        };
        record.prepared_state = TimestampQueryRecord::EPreparedState::Ready;
    }
    return VK_SUCCESS;
}

void VulkanCommandQueries::PublishTimestampQueriesAfterGpuCompletion(
    QueryPublishBatch _batch,
    bool              _gpu_success,
    std::string_view  _failure_reason
) noexcept {
    for (const TimestampQueryRecord& record : timestamp_query_records) {
        if (!_gpu_success) {
            QueryBackendAccess::PublishErrorIfPending(
                record.token,
                _failure_reason.empty() ? "Vulkan submission did not reach successful GPU completion" :
                                          _failure_reason,
                _batch
            );
            continue;
        }

        switch (record.prepared_state) {
            case TimestampQueryRecord::EPreparedState::Ready:
                QueryBackendAccess::PublishTimestamp(record.token, record.prepared_result, _batch);
                break;
            case TimestampQueryRecord::EPreparedState::InvalidValidBits:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token, "Vulkan queue family has timestampValidBits == 0", _batch
                );
                break;
            case TimestampQueryRecord::EPreparedState::Incomplete:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token, "Vulkan timestamp query pair is incomplete", _batch
                );
                break;
            case TimestampQueryRecord::EPreparedState::Unavailable:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token, "Vulkan timestamp query result is unavailable", _batch
                );
                break;
            case TimestampQueryRecord::EPreparedState::NativeFault:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token, "Vulkan timestamp query result readback faulted", _batch
                );
                break;
            case TimestampQueryRecord::EPreparedState::Unprepared:
            default:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token, "Vulkan timestamp query was not prepared for completion", _batch
                );
                break;
        }
    }
}

void VulkanCommandQueries::NotifyTimestampQueriesAfterGpuCompletion(QueryPublishBatch _batch) noexcept {
    for (const TimestampQueryRecord& record : timestamp_query_records) {
        QueryBackendAccess::NotifyTerminal(record.token, _batch);
    }
}

VkResult VulkanCommandQueries::PrepareOcclusionQueriesAfterGpuCompletion(
    const VulkanOperationContext& _context
) noexcept {
    constexpr uint32 invalid_slot = std::numeric_limits<uint32>::max();

    struct NativeOcclusionResult {
        uint64 sample_count{0};
        uint64 available{0};
    };

    for (OcclusionQueryRecord& record : occlusion_query_records) {
        record.prepared_state  = OcclusionQueryRecord::EPreparedState::Unprepared;
        record.prepared_result = {};

        if (record.slot == invalid_slot || !record.ended) {
            record.prepared_state = OcclusionQueryRecord::EPreparedState::Incomplete;
            continue;
        }

        NativeOcclusionResult native_result{};
        if (!occlusion_pool) {
            record.prepared_state = OcclusionQueryRecord::EPreparedState::NativeFault;
            return VK_ERROR_UNKNOWN;
        }
        const VkResult result = vkGetQueryPoolResults(
            device.GetDevice(),
            occlusion_pool->GetHandle(),
            record.slot,
            1,
            sizeof(native_result),
            &native_result,
            sizeof(native_result),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
        );
        if (result == VK_NOT_READY || (result == VK_SUCCESS && native_result.available == 0)) {
            record.prepared_state = OcclusionQueryRecord::EPreparedState::Unavailable;
            continue;
        }
        if (result != VK_SUCCESS) {
            record.prepared_state                = OcclusionQueryRecord::EPreparedState::NativeFault;
            VulkanOperationContext query_context = _context;
            query_context.operation              = EVulkanFaultOperation::QueryPoolResults;
            if (query_context.queue_type == EQueueType::Ignore) {
                query_context.queue_type = queue_type;
            }
            device.TryLatchFirstFault(query_context, result);
            return result;
        }

        record.prepared_result = OcclusionQueryResult{
            .sample_count = record.precise ? std::optional<uint64>{native_result.sample_count} : std::nullopt,
            .visible      = native_result.sample_count != 0,
        };
        record.prepared_state = OcclusionQueryRecord::EPreparedState::Ready;
    }
    return VK_SUCCESS;
}

void VulkanCommandQueries::PublishOcclusionQueriesAfterGpuCompletion(
    QueryPublishBatch _batch,
    bool              _gpu_success,
    std::string_view  _failure_reason
) noexcept {
    for (const OcclusionQueryRecord& record : occlusion_query_records) {
        if (!_gpu_success) {
            QueryBackendAccess::PublishErrorIfPending(
                record.token,
                _failure_reason.empty() ? "Vulkan submission did not reach successful GPU completion" :
                                          _failure_reason,
                _batch
            );
            continue;
        }

        switch (record.prepared_state) {
            case OcclusionQueryRecord::EPreparedState::Ready:
                QueryBackendAccess::PublishOcclusion(record.token, record.prepared_result, _batch);
                break;
            case OcclusionQueryRecord::EPreparedState::Incomplete:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token, "Vulkan occlusion query pair is incomplete", _batch
                );
                break;
            case OcclusionQueryRecord::EPreparedState::Unavailable:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token, "Vulkan occlusion query result is unavailable", _batch
                );
                break;
            case OcclusionQueryRecord::EPreparedState::NativeFault:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token, "Vulkan occlusion query result readback faulted", _batch
                );
                break;
            case OcclusionQueryRecord::EPreparedState::Unprepared:
            default:
                QueryBackendAccess::PublishErrorIfPending(
                    record.token, "Vulkan occlusion query was not prepared for completion", _batch
                );
                break;
        }
    }
}

void VulkanCommandQueries::NotifyOcclusionQueriesAfterGpuCompletion(QueryPublishBatch _batch) noexcept {
    for (const OcclusionQueryRecord& record : occlusion_query_records) {
        QueryBackendAccess::NotifyTerminal(record.token, _batch);
    }
}

} // namespace Moer::Render
