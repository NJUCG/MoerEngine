#include "VulkanQueryRuntime.h"

#include "../RHIImpl.h"
#include "VulkanDevice.h"
#include "log/LogSystem.h"
#include <format>

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

Array<VulkanQueryRuntime::PoolInstance>& VulkanQueryRuntime::GetPools(QueryKind _kind) {
    return _kind == QueryKind::Timestamp ? timestamp_pools : occlusion_pools;
}

int& VulkanQueryRuntime::GetActivePoolIndex(RecordContext& _ctx, QueryKind _kind) {
    return _kind == QueryKind::Timestamp ? _ctx.active_timestamp_pool : _ctx.active_occlusion_pool;
}

uint32 VulkanQueryRuntime::GetInitialPoolSize(QueryKind _kind) const {
    return _kind == QueryKind::Timestamp ? k_initial_timestamp_pool_size : k_initial_occlusion_pool_size;
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
    auto& pools           = GetPools(_kind);
    int&  active_pool_idx = GetActivePoolIndex(_ctx, _kind);

    auto try_recycle = [](PoolInstance& _pool) {
        if (!_pool.backend) {
            return;
        }
        if (_pool.next_query >= _pool.backend->Capacity() && _pool.pending_slot_uses == 0) {
            _pool.next_query = 0;
        }
    };

    auto pool_matches_owner = [&](const PoolInstance& _pool) {
        return _pool.owner_thread == _ctx.owner_thread && _pool.owner_cmd == _ctx.owner_cmd;
    };

    auto reuse_pool_for_owner = [&](PoolInstance& _pool) {
        _pool.owner_thread = _ctx.owner_thread;
        _pool.owner_cmd    = _ctx.owner_cmd;
        _pool.next_query   = 0;
        _pool.in_use       = true;
    };

    if (active_pool_idx >= 0 && active_pool_idx < static_cast<int>(pools.size())) {
        PoolInstance& active_pool = pools[active_pool_idx];
        if (!pool_matches_owner(active_pool)) {
            LOG_WARNING(MOER_TEXT("Query pool owner mismatch, allocating a dedicated pool for this command buffer."));
            active_pool_idx = -1;
        } else {
            try_recycle(active_pool);
            if (active_pool.backend && active_pool.next_query < active_pool.backend->Capacity()) {
                active_pool.in_use = true;
                return PoolSlot{
                    .kind = _kind, .pool_index = uint32(active_pool_idx), .query_index = active_pool.next_query++
                };
            }
            active_pool_idx = -1;
        }
    }

    if (active_pool_idx < 0) {
        for (int i = 0; i < static_cast<int>(pools.size()); ++i) {
            PoolInstance& pool = pools[i];
            if (!pool_matches_owner(pool)) {
                continue;
            }
            try_recycle(pool);
            if (pool.backend && pool.next_query < pool.backend->Capacity()) {
                pool.in_use       = true;
                active_pool_idx   = i;
                return PoolSlot{.kind = _kind, .pool_index = uint32(i), .query_index = pool.next_query++};
            }
        }

        for (int i = 0; i < static_cast<int>(pools.size()); ++i) {
            PoolInstance& pool = pools[i];
            if (!pool.backend || pool.in_use || pool.pending_slot_uses != 0) {
                continue;
            }

            reuse_pool_for_owner(pool);
            active_pool_idx = i;
            return PoolSlot{.kind = _kind, .pool_index = uint32(i), .query_index = pool.next_query++};
        }

        const uint32 initial_capacity = GetInitialPoolSize(_kind);
        const uint32 last_capacity =
            !pools.empty() && pools.back().backend ? pools.back().backend->Capacity() : initial_capacity;
        const uint32 new_capacity =
            pools.empty() ? initial_capacity : std::max(initial_capacity, last_capacity * 2);

        PoolInstance instance{};
        instance.backend      = CreateBackend(_kind, new_capacity);
        if (!instance.backend) {
            LOG_ERROR(MOER_TEXT("Failed to create query pool backend for kind {}, falling back to built-in backend."), uint32(_kind));
            if (_kind == QueryKind::Timestamp) {
                instance.backend = UniquePtr<IQueryPoolBackend>(
                    MakeUnique<VulkanTimestampPoolBackend>(device, new_capacity)
                );
            } else {
                instance.backend = UniquePtr<IQueryPoolBackend>(
                    MakeUnique<VulkanOcclusionPoolBackend>(device, new_capacity)
                );
            }
        }
        instance.next_query   = 0;
        instance.owner_thread = _ctx.owner_thread;
        instance.owner_cmd    = _ctx.owner_cmd;
        instance.in_use       = true;
        pools.emplace_back(std::move(instance));
        active_pool_idx = static_cast<int>(pools.size() - 1);
    }

    PoolInstance& selected_pool = pools[active_pool_idx];
    return PoolSlot{
        .kind = _kind, .pool_index = uint32(active_pool_idx), .query_index = selected_pool.next_query++
    };
}

void VulkanQueryRuntime::BeginRecord(VulkanCmdList& _cmd_list, std::span<const QueryToken> _issued_tokens) {
    std::scoped_lock lock(runtime_mtx);

    RecordContext context{};
    context.owner_thread = std::this_thread::get_id();
    context.owner_cmd    = _cmd_list.GetHandle();
    context.active_timestamp_pool = -1;
    context.active_occlusion_pool = -1;
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

    auto release_ctx = [&](const RecordContext& _ctx) {
        for (auto& pool : timestamp_pools) {
            if (pool.owner_thread == _ctx.owner_thread && pool.owner_cmd == _ctx.owner_cmd) {
                pool.in_use = false;
            }
        }
        for (auto& pool : occlusion_pools) {
            if (pool.owner_thread == _ctx.owner_thread && pool.owner_cmd == _ctx.owner_cmd) {
                pool.in_use = false;
            }
        }
    };
    for (const auto& [owner_cmd, ctx] : active_contexts) {
        (void)owner_cmd;
        release_ctx(ctx);
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
    auto&    pool       = timestamp_pools[slot.pool_index];
    auto&    native_pool = pool.backend->NativePool();
    _cmd_list.ResetQueryPool(native_pool, slot.query_index, 1);
    _cmd_list.WriteTimeStamp(native_pool, slot.query_index, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    record.begin_slot = slot;
    context->used_slots.emplace_back(slot);
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
    auto&    pool       = timestamp_pools[slot.pool_index];
    auto&    native_pool = pool.backend->NativePool();
    _cmd_list.ResetQueryPool(native_pool, slot.query_index, 1);
    _cmd_list.WriteTimeStamp(native_pool, slot.query_index, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
    record.end_slot = slot;
    context->used_slots.emplace_back(slot);
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
    auto&    pool       = occlusion_pools[slot.pool_index];
    auto&    native_pool = pool.backend->NativePool();
    _cmd_list.ResetQueryPool(native_pool, slot.query_index, 1);
    _cmd_list.BeginQuery(native_pool, slot.query_index, VK_QUERY_CONTROL_PRECISE_BIT);
    record.occlusion_slot = slot;
    context->used_slots.emplace_back(slot);
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
    if (slot.pool_index >= occlusion_pools.size()) {
        return;
    }
    auto& backend = occlusion_pools[slot.pool_index].backend;
    if (!backend) {
        return;
    }
    _cmd_list.EndQuery(backend->NativePool(), slot.query_index);
}

void VulkanQueryRuntime::FinalizeSubmit(uint64 _timeline, VkCommandBuffer _owner_cmd) {
    std::scoped_lock lock(runtime_mtx);
    if (_owner_cmd == VK_NULL_HANDLE) {
        return;
    }
    const auto iter = active_contexts.find(_owner_cmd);
    if (iter == active_contexts.end()) {
        return;
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

    PendingSubmission submission{};
    submission.timeline = _timeline;
    submission.records.reserve(context.records.size());
    for (auto& [id, record] : context.records) {
        submission.records.emplace_back(std::move(record));
    }
    submission.used_slots = std::move(context.used_slots);
    pending_submissions.emplace_back(std::move(submission));

    for (const auto& slot : pending_submissions.back().used_slots) {
        auto& pools = GetPools(slot.kind);
        if (slot.pool_index >= pools.size()) {
            continue;
        }
        pools[slot.pool_index].pending_slot_uses += 1;
    }

    for (auto& pool : timestamp_pools) {
        if (pool.owner_thread == context.owner_thread && pool.owner_cmd == context.owner_cmd) {
            pool.in_use = false;
        }
    }
    for (auto& pool : occlusion_pools) {
        if (pool.owner_thread == context.owner_thread && pool.owner_cmd == context.owner_cmd) {
            pool.in_use = false;
        }
    }
}

void VulkanQueryRuntime::ResolveCompleted(uint64 _timeline) {
    Array<PendingSubmission> resolved_submissions{};
    {
        std::scoped_lock lock(runtime_mtx);
        auto             iter = pending_submissions.begin();
        while (iter != pending_submissions.end()) {
            if (iter->timeline <= _timeline) {
                resolved_submissions.emplace_back(std::move(*iter));
                iter = pending_submissions.erase(iter);
            } else {
                ++iter;
            }
        }
    }

    for (auto& submission : resolved_submissions) {
        for (auto& record : submission.records) {
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
                            .end_tick   = end_tick,
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
                    result.payload            = OcclusionQueryResult{
                                   .sample_count = sample_count,
                                   .visible      = sample_count > 0
                    };
                }
            } else {
                result.status = QueryStatus::Error;
            }

            record.token.Resolve(std::move(result));
        }

        for (const auto& slot : submission.used_slots) {
            MarkSlotPendingUse(slot, -1);
            RecyclePoolIfPossible(slot);
        }
    }
}

void VulkanQueryRuntime::ResolveAsError(std::span<const QueryToken> _tokens, std::string_view _reason) {
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
            result.name = std::format("{} ({})", token.name, _reason);
        }
        token.Resolve(std::move(result));
    }
}

uint64 VulkanQueryRuntime::ResolveTimestampTick(const PoolSlot& _slot) {
    VkQueryPool query_pool = VK_NULL_HANDLE;
    {
        std::scoped_lock lock(runtime_mtx);
        if (_slot.pool_index >= timestamp_pools.size()) {
            return 0;
        }
        auto& backend = timestamp_pools[_slot.pool_index].backend;
        if (!backend) {
            return 0;
        }
        query_pool = backend->NativePool().GetHandle();
    }
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
    VkQueryPool query_pool = VK_NULL_HANDLE;
    {
        std::scoped_lock lock(runtime_mtx);
        if (_slot.pool_index >= occlusion_pools.size()) {
            return 0;
        }
        auto& backend = occlusion_pools[_slot.pool_index].backend;
        if (!backend) {
            return 0;
        }
        query_pool = backend->NativePool().GetHandle();
    }
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

void VulkanQueryRuntime::MarkSlotPendingUse(const PoolSlot& _slot, int _delta) {
    std::scoped_lock lock(runtime_mtx);
    auto& pools = GetPools(_slot.kind);
    if (_slot.pool_index >= pools.size()) {
        return;
    }
    PoolInstance& pool = pools[_slot.pool_index];
    if (_delta > 0) {
        pool.pending_slot_uses += uint32(_delta);
    } else {
        const uint32 delta = uint32(-_delta);
        pool.pending_slot_uses = pool.pending_slot_uses > delta ? pool.pending_slot_uses - delta : 0;
    }
}

void VulkanQueryRuntime::RecyclePoolIfPossible(const PoolSlot& _slot) {
    std::scoped_lock lock(runtime_mtx);
    auto& pools = GetPools(_slot.kind);
    if (_slot.pool_index >= pools.size()) {
        return;
    }
    PoolInstance& pool = pools[_slot.pool_index];
    if (pool.backend && pool.pending_slot_uses == 0 && pool.next_query >= pool.backend->Capacity()) {
        pool.next_query = 0;
    }
}

} // namespace Moer::Render
