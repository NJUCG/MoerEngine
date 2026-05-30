#include "VulkanQueryRuntime.h"

#include "../RHIImpl.h"
#include "VulkanDevice.h"
#include "log/LogSystem.h"
#include "string/Format.h"

namespace Moer::Render {

VulkanQueryRuntime::VulkanTimestampPoolBackend::VulkanTimestampPoolBackend(
    VulkanDevice& _device,
    uint32        _capacity
) :
    pool(MakeUnique<VkNativeQueryPool>(_device, VK_QUERY_TYPE_TIMESTAMP, _capacity)),
    capacity(_capacity) {}

VulkanQueryRuntime::VulkanOcclusionPoolBackend::VulkanOcclusionPoolBackend(
    VulkanDevice& _device,
    uint32        _capacity
) :
    pool(MakeUnique<VkNativeQueryPool>(_device, VK_QUERY_TYPE_OCCLUSION, _capacity)),
    capacity(_capacity) {}

VulkanQueryRuntime::VulkanQueryRuntime(VulkanDevice& _device) : device(_device) {
    timestamp_period = device.GetCoreProperties().core_1_0.limits.timestampPeriod;
    RegisterPoolBackendFactory(QueryKind::Timestamp, [](VulkanDevice& _device, uint32 _capacity) {
        return UniquePtr<IQueryPoolBackend>(MakeUnique<VulkanTimestampPoolBackend>(_device, _capacity));
    });
    RegisterPoolBackendFactory(QueryKind::Occlusion, [](VulkanDevice& _device, uint32 _capacity) {
        return UniquePtr<IQueryPoolBackend>(MakeUnique<VulkanOcclusionPoolBackend>(_device, _capacity));
    });
}

void VulkanQueryRuntime::RegisterPoolBackendFactory(QueryKind _kind, PoolBackendFactory _factory) {
    if (!_factory) {
        return;
    }
    std::scoped_lock lock(runtime_mtx);
    backend_factories[_kind] = std::move(_factory);
}

Array<UniquePtr<VulkanQueryRuntime::PoolInstance>>& VulkanQueryRuntime::GetAvailablePools(QueryKind _kind) {
    return _kind == QueryKind::Timestamp ? available_timestamp_pools : available_occlusion_pools;
}

Array<UniquePtr<VulkanQueryRuntime::PoolInstance>>&
VulkanQueryRuntime::GetSubmissionPools(SubmissionState& _submission, QueryKind _kind) {
    return _kind == QueryKind::Timestamp ? _submission.timestamp_pools : _submission.occlusion_pools;
}

VulkanQueryRuntime::PoolInstance*& VulkanQueryRuntime::GetActivePool(RecordContext& _ctx, QueryKind _kind) {
    return _kind == QueryKind::Timestamp ? _ctx.active_timestamp_pool : _ctx.active_occlusion_pool;
}

UniquePtr<VulkanQueryRuntime::IQueryPoolBackend>
VulkanQueryRuntime::CreateBackend(QueryKind _kind, uint32 _capacity) {
    auto iter = backend_factories.find(_kind);
    if (iter == backend_factories.end()) {
        LOG_WARNING(MOER_TEXT("No query backend factory registered for kind {}, using default backend."), uint32(_kind));
        if (_kind == QueryKind::Timestamp) {
            return UniquePtr<IQueryPoolBackend>(MakeUnique<VulkanTimestampPoolBackend>(device, _capacity));
        }
        if (_kind == QueryKind::Occlusion) {
            return UniquePtr<IQueryPoolBackend>(MakeUnique<VulkanOcclusionPoolBackend>(device, _capacity));
        }
        return nullptr;
    }
    return iter->second(device, _capacity);
}

UniquePtr<VulkanQueryRuntime::PoolInstance> VulkanQueryRuntime::AcquirePoolChunk(QueryKind _kind) {
    auto& available_pools = GetAvailablePools(_kind);
    if (!available_pools.empty()) {
        UniquePtr<PoolInstance> pool = std::move(available_pools.back());
        available_pools.pop_back();
        pool->next_query = 0;
        return pool;
    }

    UniquePtr<PoolInstance> pool = MakeUnique<PoolInstance>();
    pool->backend = CreateBackend(_kind, k_query_pool_chunk_size);
    if (!pool->backend) {
        LOG_ERROR(MOER_TEXT("Failed to create query pool backend for kind {}, falling back to built-in backend."), uint32(_kind));
        if (_kind == QueryKind::Timestamp) {
            pool->backend = UniquePtr<IQueryPoolBackend>(
                MakeUnique<VulkanTimestampPoolBackend>(device, k_query_pool_chunk_size)
            );
        } else {
            pool->backend = UniquePtr<IQueryPoolBackend>(
                MakeUnique<VulkanOcclusionPoolBackend>(device, k_query_pool_chunk_size)
            );
        }
    }
    pool->next_query = 0;
    return pool;
}

void VulkanQueryRuntime::RecycleSubmissionPools(SubmissionState& _submission) {
    auto recycle_kind = [this](QueryKind _kind, Array<UniquePtr<PoolInstance>>& submission_pools) {
        auto& available_pools = GetAvailablePools(_kind);
        for (auto& pool : submission_pools) {
            if (!pool) {
                continue;
            }
            pool->next_query = 0;
            available_pools.emplace_back(std::move(pool));
        }
        submission_pools.clear();
    };

    recycle_kind(QueryKind::Timestamp, _submission.timestamp_pools);
    recycle_kind(QueryKind::Occlusion, _submission.occlusion_pools);
}

VulkanQueryRuntime::QueryRecord&
VulkanQueryRuntime::GetOrCreateRecord(RecordContext& _ctx, const QueryToken& _token) {
    auto iter = _ctx.records.find(_token.id);
    if (iter == _ctx.records.end()) {
        QueryRecord record{};
        record.token = _token;
        record.kind  = _token.kind;
        record.name  = _token.name;
        iter         = _ctx.records.emplace(_token.id, std::move(record)).first;
    }
    return iter->second;
}

VulkanQueryRuntime::RecordContext* VulkanQueryRuntime::FindActiveContext(VkCommandBuffer _owner_cmd) {
    const auto iter = active_contexts.find(_owner_cmd);
    if (iter == active_contexts.end()) {
        return nullptr;
    }
    return &iter->second;
}

VulkanQueryRuntime::PoolSlot VulkanQueryRuntime::AcquireSlot(QueryKind _kind, RecordContext& _ctx) {
    PoolInstance*& active_pool = GetActivePool(_ctx, _kind);
    if (active_pool == nullptr || !active_pool->backend || active_pool->next_query >= active_pool->backend->Capacity()) {
        UniquePtr<PoolInstance> pool = AcquirePoolChunk(_kind);
        active_pool = pool.get();
        GetSubmissionPools(_ctx.submission, _kind).emplace_back(std::move(pool));
    }

    return PoolSlot{
        .kind = _kind,
        .pool = active_pool,
        .query_index = active_pool->next_query++
    };
}

void VulkanQueryRuntime::BeginRecord(VulkanCmdList& _cmd_list, std::span<const QueryToken> _issued_tokens) {
    std::scoped_lock lock(runtime_mtx);

    RecordContext context{};
    context.owner_cmd    = _cmd_list.GetHandle();
    context.issued_tokens.reserve(_issued_tokens.size());
    for (const auto& token : _issued_tokens) {
        if (!token.Valid()) {
            continue;
        }
        context.issued_tokens.emplace(token.id, token);
    }
    active_contexts[_cmd_list.GetHandle()] = std::move(context);
}

void VulkanQueryRuntime::EndRecord() {
    std::scoped_lock lock(runtime_mtx);
    if (active_contexts.empty()) {
        return;
    }

    for (auto& [owner_cmd, ctx] : active_contexts) {
        (void)owner_cmd;
        RecycleSubmissionPools(ctx.submission);
    }
    active_contexts.clear();
}

void VulkanQueryRuntime::HandleQueryCommand(VulkanCmdList& _cmd_list, const QueryCmd& _cmd) {
    switch (_cmd.Op()) {
        case QueryCmd::EOp::BeginTimestamp:
            BeginTimestamp(_cmd_list, _cmd.Token());
            break;
        case QueryCmd::EOp::EndTimestamp:
            EndTimestamp(_cmd_list, _cmd.Token());
            break;
        case QueryCmd::EOp::BeginOcclusion:
            BeginOcclusion(_cmd_list, _cmd.Token());
            break;
        case QueryCmd::EOp::EndOcclusion:
            EndOcclusion(_cmd_list, _cmd.Token());
            break;
    }
}

void VulkanQueryRuntime::BeginTimestamp(VulkanCmdList& _cmd_list, const QueryToken& _token) {
    std::scoped_lock lock(runtime_mtx);
    auto* context = FindActiveContext(_cmd_list.GetHandle());
    if (context == nullptr) {
        return;
    }

    QueryRecord& record = GetOrCreateRecord(*context, _token);
    record.kind         = QueryKind::Timestamp;
    PoolSlot slot       = AcquireSlot(QueryKind::Timestamp, *context);
    auto&    native_pool = slot.pool->backend->NativePool();
    _cmd_list.ResetQueryPool(native_pool, slot.query_index, 1);
    _cmd_list.WriteTimeStamp(native_pool, slot.query_index, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    record.begin_slot = slot;
}

void VulkanQueryRuntime::EndTimestamp(VulkanCmdList& _cmd_list, const QueryToken& _token) {
    std::scoped_lock lock(runtime_mtx);
    auto* context = FindActiveContext(_cmd_list.GetHandle());
    if (context == nullptr) {
        return;
    }

    QueryRecord& record = GetOrCreateRecord(*context, _token);
    record.kind         = QueryKind::Timestamp;
    PoolSlot slot       = AcquireSlot(QueryKind::Timestamp, *context);
    auto&    native_pool = slot.pool->backend->NativePool();
    _cmd_list.ResetQueryPool(native_pool, slot.query_index, 1);
    _cmd_list.WriteTimeStamp(native_pool, slot.query_index, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    record.end_slot = slot;
}

void VulkanQueryRuntime::BeginOcclusion(VulkanCmdList& _cmd_list, const QueryToken& _token) {
    std::scoped_lock lock(runtime_mtx);
    auto* context = FindActiveContext(_cmd_list.GetHandle());
    if (context == nullptr) {
        return;
    }

    QueryRecord& record = GetOrCreateRecord(*context, _token);
    record.kind         = QueryKind::Occlusion;
    PoolSlot slot       = AcquireSlot(QueryKind::Occlusion, *context);
    auto&    native_pool = slot.pool->backend->NativePool();
    _cmd_list.ResetQueryPool(native_pool, slot.query_index, 1);
    _cmd_list.BeginQuery(native_pool, slot.query_index, VK_QUERY_CONTROL_PRECISE_BIT);
    record.occlusion_slot = slot;
}

void VulkanQueryRuntime::EndOcclusion(VulkanCmdList& _cmd_list, const QueryToken& _token) {
    std::scoped_lock lock(runtime_mtx);
    auto* context = FindActiveContext(_cmd_list.GetHandle());
    if (context == nullptr) {
        return;
    }

    QueryRecord& record = GetOrCreateRecord(*context, _token);
    record.kind         = QueryKind::Occlusion;
    if (!record.occlusion_slot.has_value()) {
        return;
    }
    const PoolSlot slot = record.occlusion_slot.value();
    if (slot.pool == nullptr || !slot.pool->backend) {
        return;
    }
    _cmd_list.EndQuery(slot.pool->backend->NativePool(), slot.query_index);
}

std::optional<VulkanQueryRuntime::SubmissionState>
VulkanQueryRuntime::FinalizeSubmit(uint64 _timeline, VkCommandBuffer _owner_cmd) {
    std::scoped_lock lock(runtime_mtx);
    if (_owner_cmd == VK_NULL_HANDLE) {
        return std::nullopt;
    }
    const auto iter = active_contexts.find(_owner_cmd);
    if (iter == active_contexts.end()) {
        return std::nullopt;
    }
    RecordContext context = std::move(iter->second);
    active_contexts.erase(iter);

    for (const auto& [id, token] : context.issued_tokens) {
        if (context.records.find(id) == context.records.end()) {
            QueryRecord record{};
            record.token = token;
            record.kind  = token.kind;
            record.name  = token.name;
            context.records.emplace(id, std::move(record));
        }
    }

    context.submission.timeline = _timeline;
    context.submission.records.reserve(context.records.size());
    for (auto& [id, record] : context.records) {
        context.submission.records.emplace_back(std::move(record));
    }
    return std::move(context.submission);
}

void VulkanQueryRuntime::ResolveCompleted(SubmissionState&& _submission) {
    for (auto& record : _submission.records) {
        QueryResult result{};
        result.kind     = record.kind;
        result.query_id = record.token.id;
        result.name     = record.name;
        result.status   = QueryStatus::Ready;

        if (record.kind == QueryKind::Timestamp) {
            if (!record.begin_slot.has_value() && !record.end_slot.has_value()) {
                result.status = QueryStatus::Error;
            } else {
                const uint64 begin_tick = record.begin_slot.has_value()
                                              ? ResolveTimestampTick(record.begin_slot.value())
                                              : ResolveTimestampTick(record.end_slot.value());
                const uint64 end_tick   = record.end_slot.has_value()
                                            ? ResolveTimestampTick(record.end_slot.value())
                                            : begin_tick;
                if (end_tick < begin_tick) {
                    result.status = QueryStatus::Error;
                } else {
                    result.payload = TimestampQueryResult{
                        .begin_tick = begin_tick,
                        .end_tick = end_tick,
                        .tick_period_ns = double(timestamp_period),
                        .duration_ns = double(end_tick - begin_tick) * double(timestamp_period)
                    };
                }
            }
        } else if (record.kind == QueryKind::Occlusion) {
            if (!record.occlusion_slot.has_value()) {
                result.status = QueryStatus::Error;
            } else {
                const uint64 sample_count = ResolveOcclusionSamples(record.occlusion_slot.value());
                result.payload = OcclusionQueryResult{
                    .sample_count = sample_count,
                    .visible = sample_count > 0
                };
            }
        } else {
            result.status = QueryStatus::Error;
        }

        record.token.Resolve(std::move(result));
    }

    std::scoped_lock lock(runtime_mtx);
    RecycleSubmissionPools(_submission);
}

uint64 VulkanQueryRuntime::ResolveTimestampTick(const PoolSlot& _slot) {
    if (_slot.pool == nullptr || !_slot.pool->backend) {
        return 0;
    }
    VkQueryPool query_pool = _slot.pool->backend->NativePool().GetHandle();
    uint64 tick = 0;
    VkResult result = vkGetQueryPoolResults(
        device.GetDevice(),
        query_pool,
        _slot.query_index,
        1,
        sizeof(tick),
        &tick,
        sizeof(uint64),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT
    );
    if (result != VK_SUCCESS) {
        return 0;
    }
    return tick;
}

uint64 VulkanQueryRuntime::ResolveOcclusionSamples(const PoolSlot& _slot) {
    if (_slot.pool == nullptr || !_slot.pool->backend) {
        return 0;
    }
    VkQueryPool query_pool = _slot.pool->backend->NativePool().GetHandle();
    uint64 samples = 0;
    VkResult result  = vkGetQueryPoolResults(
        device.GetDevice(),
        query_pool,
        _slot.query_index,
        1,
        sizeof(samples),
        &samples,
        sizeof(uint64),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT
    );
    if (result != VK_SUCCESS) {
        return 0;
    }
    return samples;
}

void VulkanQueryRuntime::ResolveAsError(std::span<const QueryToken> _tokens, StringView _reason) {
    for (const auto& token : _tokens) {
        if (!token.Valid()) {
            continue;
        }
        QueryResult result{};
        result.kind     = token.kind;
        result.query_id = token.id;
        result.name     = token.name;
        result.status   = QueryStatus::Error;
        if (!_reason.empty()) {
            result.name = Printf(MOER_TEXT("{} ({})"), token.name, _reason);
        }
        token.Resolve(std::move(result));
    }
}

} // namespace Moer::Render
