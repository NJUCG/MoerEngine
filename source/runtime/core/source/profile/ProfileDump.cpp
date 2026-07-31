#include "profile/ProfileDump.h"

#include "ProfileDumpTesting.h"
#include "platform/Platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#if PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace Moer::ProfileDump {

namespace {

using namespace std::chrono_literals;

std::atomic<std::uint64_t> g_temp_path_nonce{1};

[[nodiscard]] std::uint64_t CurrentProcessIdValue() noexcept {
#if PLATFORM_WINDOWS
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] std::filesystem::path MakeSessionTempPath(const std::filesystem::path& _output_path) {
    const auto clock_tick =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t nonce = g_temp_path_nonce.fetch_add(1, std::memory_order_relaxed);

    auto combine = [](std::uint64_t _seed, std::uint64_t _value) noexcept {
        return _seed ^ (_value + 0x9e3779b97f4a7c15ULL + (_seed << 6U) + (_seed >> 2U));
    };
    std::uint64_t token = combine(CurrentProcessIdValue(), clock_tick);
    token               = combine(token, nonce);
    token ^= token >> 30U;
    token *= 0xbf58476d1ce4e5b9ULL;
    token ^= token >> 27U;
    token *= 0x94d049bb133111ebULL;
    token ^= token >> 31U;

    char token_text[17]{};
    static_cast<void>(
        std::snprintf(token_text, sizeof(token_text), "%016llx", static_cast<unsigned long long>(token))
    );
    return _output_path.parent_path() / (std::string(".moer-profile-") + token_text + ".inprogress");
}

struct PendingRecord {
    std::uint64_t       schema_hash{0};
    std::uint64_t       sequence{0};
    Array<std::uint8_t> payload{};
};

struct ImmutableChunk {
    std::uint64_t              generation{0};
    std::vector<PendingRecord> records{};
    std::size_t                payload_bytes{0};
    std::uint64_t              first_sequence{0};
    std::uint64_t              last_sequence{0};
    std::uint64_t              value_bytes{0};
};

struct FlushFence {
    std::mutex              mutex{};
    std::condition_variable cv{};
    bool                    complete{false};
    FlushResult             result{FlushResult::Rejected};
};

struct FlushMessage {
    std::shared_ptr<FlushFence> fence{};
};

struct FinalizeMessage {
    std::shared_ptr<FlushFence> fence{};
};

using WriterMessage = std::variant<ImmutableChunk, FlushMessage, FinalizeMessage>;

struct SchemaEntry {
    SchemaDescriptor descriptor{};
    std::size_t      encoded_bytes{0};
};

using SchemaMap = std::unordered_map<std::uint64_t, std::shared_ptr<const SchemaEntry>>;

struct Admission {
    std::uint64_t              generation{0};
    RuntimeConfig              config{};
    std::atomic<std::uint64_t> next_sequence{1};
    // Protected by Hub::mutex. Shutdown closes admission first, then waits
    // until every registered emitter has either published its TLS shard or
    // left the active set.
    bool          closing{false};
    std::uint64_t active_emitters{0};
};

struct AtomicStats {
    std::atomic<std::uint64_t> records_committed{0};
    std::atomic<std::uint64_t> records_enqueued{0};
    std::atomic<std::uint64_t> records_written{0};
    std::atomic<std::uint64_t> records_dropped_stopped{0};
    std::atomic<std::uint64_t> records_dropped_stale_generation{0};
    std::atomic<std::uint64_t> records_dropped_oversized{0};
    std::atomic<std::uint64_t> records_dropped_queue_full{0};
    std::atomic<std::uint64_t> records_dropped_after_fault{0};

    std::atomic<std::uint64_t> chunks_enqueued{0};
    std::atomic<std::uint64_t> chunks_written{0};
    std::atomic<std::uint64_t> chunks_dropped{0};

    std::atomic<std::uint64_t> resident_chunks{0};
    std::atomic<std::uint64_t> resident_records{0};
    std::atomic<std::uint64_t> resident_bytes{0};
    std::atomic<std::uint64_t> high_water_chunks{0};
    std::atomic<std::uint64_t> high_water_records{0};
    std::atomic<std::uint64_t> high_water_bytes{0};

    std::atomic<std::uint64_t> file_bytes_written{0};
    std::atomic<std::uint64_t> flush_completed{0};
    std::atomic<std::uint64_t> flush_failed{0};
    std::atomic<std::uint64_t> io_faults{0};

    void Reset() noexcept {
        records_committed.store(0, std::memory_order_relaxed);
        records_enqueued.store(0, std::memory_order_relaxed);
        records_written.store(0, std::memory_order_relaxed);
        records_dropped_stopped.store(0, std::memory_order_relaxed);
        records_dropped_stale_generation.store(0, std::memory_order_relaxed);
        records_dropped_oversized.store(0, std::memory_order_relaxed);
        records_dropped_queue_full.store(0, std::memory_order_relaxed);
        records_dropped_after_fault.store(0, std::memory_order_relaxed);
        chunks_enqueued.store(0, std::memory_order_relaxed);
        chunks_written.store(0, std::memory_order_relaxed);
        chunks_dropped.store(0, std::memory_order_relaxed);
        resident_chunks.store(0, std::memory_order_relaxed);
        resident_records.store(0, std::memory_order_relaxed);
        resident_bytes.store(0, std::memory_order_relaxed);
        high_water_chunks.store(0, std::memory_order_relaxed);
        high_water_records.store(0, std::memory_order_relaxed);
        high_water_bytes.store(0, std::memory_order_relaxed);
        file_bytes_written.store(0, std::memory_order_relaxed);
        flush_completed.store(0, std::memory_order_relaxed);
        flush_failed.store(0, std::memory_order_relaxed);
        io_faults.store(0, std::memory_order_relaxed);
    }
};

struct TestHooks {
    Testing::FaultPoint fault_point{Testing::FaultPoint::None};
    std::uint64_t       trigger_hit{1};
    std::uint64_t       matching_hits{0};
    bool                fault_fired{false};
    bool                pause_before_temp_open{false};
    bool                pause_after_start{false};
    bool                writer_paused{false};
    bool                writer_resume{false};
    bool                pause_before_idle_wait{false};
    bool                writer_idle_wait_paused{false};
    bool                writer_idle_wait_resume{false};
    bool                pause_emitter_after_admission{false};
    bool                emitter_paused{false};
    bool                emitter_resume{false};
};

struct PendingLossSlot {
    bool          pending{false};
    std::uint64_t generation{0};
    LossNotice    notice{};
};

struct ThreadLocalShardNode;

struct Hub {
    std::mutex              lifecycle_mutex{};
    std::mutex              worker_join_mutex{};
    std::mutex              tls_registry_mutex{};
    std::mutex              mutex{};
    std::condition_variable work_cv{};
    std::condition_variable start_cv{};
    std::condition_variable test_cv{};
    std::condition_variable emitter_cv{};

    std::atomic<RuntimeState>  state{RuntimeState::Stopped};
    std::atomic<RuntimeFault>  last_fault{RuntimeFault::None};
    std::atomic<std::uint64_t> generation{0};
    std::uint64_t              generation_counter{0};

    // Crash flushing is intentionally independent of Hub::mutex and the
    // allocation-backed WriterMessage queue. A fatal caller publishes a
    // monotonically increasing ticket, wakes the writer, then polls these
    // atomics only until its bounded deadline.
    std::atomic<std::uint64_t> crash_flush_requested{0};
    std::atomic<std::uint64_t> crash_flush_completed{0};
    std::atomic<std::uint64_t> crash_flush_failed{0};
    std::atomic<std::uint64_t> crash_flush_epoch{0};
    std::atomic<std::uint32_t> writer_thread_id{0};
    std::atomic<bool>          test_pause_crash_flush_before_publish{false};
    std::atomic<bool>          test_crash_flush_before_publish_paused{false};
    std::atomic<bool>          test_crash_flush_before_publish_resume{false};

    RuntimeConfig             config{};
    std::filesystem::path     temp_path{};
    std::deque<WriterMessage> messages{};
    bool                      file_dirty{false};
    PendingLossSlot           pending_loss{};

    std::atomic<std::shared_ptr<Admission>>       admission{};
    std::atomic<std::shared_ptr<const SchemaMap>> schemas{std::make_shared<const SchemaMap>()};
    std::size_t                                   schema_bytes{0};

    std::jthread                worker{};
    bool                        start_complete{false};
    StartResult                 start_result{StartResult::FileOpenFailed};
    std::shared_ptr<FlushFence> shutdown_fence{};
    std::shared_ptr<FlushFence> active_flush_fence{};
    // Protected by mutex. Keeps a closing generation alive after admission is
    // unpublished so Shutdown can wait for every already-admitted Emit call.
    std::shared_ptr<Admission> retired_admission{};

    // Protected by tls_registry_mutex. Nodes are owned by live producer
    // threads and remain linked across capture generations until TLS
    // destruction.
    ThreadLocalShardNode* tls_shards_head{nullptr};

    AtomicStats stats{};
    TestHooks   test_hooks{};
};

std::atomic<Hub*> g_hub_instance{nullptr};

Hub& GetHub() {
    // Intentionally leaked. A TLS shard may be destroyed after ordinary
    // function-local statics; the process hub must remain a valid publication
    // target until the last producer thread exits.
    static Hub* hub = [] {
        Hub* created = new Hub();
        g_hub_instance.store(created, std::memory_order_release);
        return created;
    }();
    return *hub;
}

Hub* TryGetHub() noexcept {
    return g_hub_instance.load(std::memory_order_acquire);
}

enum class PublishResult : std::uint8_t {
    Nothing,
    Accepted,
    QueueFull,
    Rejected,
    Faulted,
    Stale,
};

struct ThreadLocalShardNode {
    std::mutex                 mutex{};
    std::uint64_t              generation{0};
    std::vector<PendingRecord> records{};
    std::size_t                payload_bytes{0};
    ThreadLocalShardNode*      registry_previous{nullptr};
    ThreadLocalShardNode*      registry_next{nullptr};
    std::atomic<bool>          registered{false};
};

struct ThreadLocalShard {
    // The registry points to this member rather than the TLS owner object.
    // Its lifetime extends through the owner's destructor body, where the node
    // is published and unlinked before member destruction begins.
    ThreadLocalShardNode node{};
    ~ThreadLocalShard();
};

thread_local ThreadLocalShard g_tls_shard{};

void UpdateHighWater(std::atomic<std::uint64_t>& _counter, std::uint64_t _value) noexcept {
    std::uint64_t observed = _counter.load(std::memory_order_relaxed);
    while (observed < _value && !_counter.compare_exchange_weak(
                                    observed, _value, std::memory_order_relaxed, std::memory_order_relaxed
                                )) {
    }
}

std::uint64_t SaturatingAdd(std::uint64_t _lhs, std::uint64_t _rhs) noexcept {
    constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    return _rhs > max - _lhs ? max : _lhs + _rhs;
}

bool WouldExceedLimit(std::uint64_t _resident, std::uint64_t _additional, std::uint64_t _limit) noexcept {
    return _resident > _limit || _additional > _limit - _resident;
}

std::uint64_t RecordValueBytes(const PendingRecord& _record) noexcept {
    constexpr std::size_t record_prefix_bytes = sizeof(std::uint64_t) * 2 + sizeof(std::uint32_t);
    return _record.payload.size() > record_prefix_bytes ?
               static_cast<std::uint64_t>(_record.payload.size() - record_prefix_bytes) :
               0;
}

std::uint64_t EncodedRecordValueBytes(std::span<const std::uint8_t> _payload) noexcept {
    constexpr std::size_t record_prefix_bytes = sizeof(std::uint64_t) * 2 + sizeof(std::uint32_t);
    return _payload.size() > record_prefix_bytes ?
               static_cast<std::uint64_t>(_payload.size() - record_prefix_bytes) :
               0;
}

std::uint64_t EstimateValueBytes(std::span<const FieldValueView> _values) noexcept {
    std::uint64_t bytes = 0;
    for (const FieldValueView& value : _values) {
        const std::uint64_t field_bytes = std::visit(
            [](const auto& field) -> std::uint64_t {
                using FieldT = std::decay_t<decltype(field)>;
                if constexpr (std::is_same_v<FieldT, bool>) {
                    return 1;
                } else if constexpr (std::is_same_v<FieldT, std::string_view>) {
                    return SaturatingAdd(sizeof(std::uint32_t), static_cast<std::uint64_t>(field.size()));
                } else {
                    return sizeof(FieldT);
                }
            },
            value
        );
        bytes = SaturatingAdd(bytes, field_bytes);
    }
    return bytes;
}

void AccumulateLossLocked(
    Hub&          _hub,
    std::uint64_t _generation,
    std::uint64_t _first_sequence,
    std::uint64_t _last_sequence,
    std::uint64_t _record_count,
    std::uint64_t _value_bytes,
    LossReason    _reason
) noexcept {
    if (_record_count == 0 || _hub.state.load(std::memory_order_acquire) != RuntimeState::Running ||
        _generation != _hub.generation.load(std::memory_order_acquire)) {
        return;
    }

    PendingLossSlot& slot = _hub.pending_loss;
    if (!slot.pending || slot.generation != _generation) {
        slot.pending    = true;
        slot.generation = _generation;
        slot.notice     = LossNotice{
                .first_sequence = _first_sequence,
                .last_sequence  = _last_sequence,
                .record_count   = _record_count,
                .value_bytes    = _value_bytes,
                .reason_mask    = static_cast<std::uint32_t>(_reason),
        };
        return;
    }

    slot.notice.first_sequence = std::min(slot.notice.first_sequence, _first_sequence);
    slot.notice.last_sequence  = std::max(slot.notice.last_sequence, _last_sequence);
    slot.notice.record_count   = SaturatingAdd(slot.notice.record_count, _record_count);
    slot.notice.value_bytes    = SaturatingAdd(slot.notice.value_bytes, _value_bytes);
    slot.notice.reason_mask |= static_cast<std::uint32_t>(_reason);
}

void AccumulateLoss(
    std::uint64_t _generation,
    std::uint64_t _first_sequence,
    std::uint64_t _last_sequence,
    std::uint64_t _record_count,
    std::uint64_t _value_bytes,
    LossReason    _reason
) noexcept {
    Hub& hub = GetHub();
    {
        std::lock_guard lock(hub.mutex);
        AccumulateLossLocked(
            hub, _generation, _first_sequence, _last_sequence, _record_count, _value_bytes, _reason
        );
    }
    hub.work_cv.notify_one();
}

std::optional<LossNotice> TakePendingLossLocked(Hub& _hub, std::uint64_t _generation) noexcept {
    if (!_hub.pending_loss.pending || _hub.pending_loss.generation != _generation) {
        return std::nullopt;
    }
    LossNotice notice = _hub.pending_loss.notice;
    _hub.pending_loss = {};
    return notice;
}

void SignalFence(const std::shared_ptr<FlushFence>& _fence, FlushResult _result) noexcept {
    if (!_fence) {
        return;
    }
    {
        std::lock_guard lock(_fence->mutex);
        _fence->result   = _result;
        _fence->complete = true;
    }
    _fence->cv.notify_all();
}

FlushResult WaitFence(const std::shared_ptr<FlushFence>& _fence) noexcept {
    if (!_fence) {
        return FlushResult::Rejected;
    }
    std::unique_lock lock(_fence->mutex);
    _fence->cv.wait(lock, [&] {
        return _fence->complete;
    });
    return _fence->result;
}

bool IsIoFault(RuntimeFault _fault) noexcept {
    return _fault == RuntimeFault::OpenTempFile || _fault == RuntimeFault::WritePacket ||
           _fault == RuntimeFault::FlushFile || _fault == RuntimeFault::CloseFile ||
           _fault == RuntimeFault::RenameFinal;
}

bool ShouldInjectLocked(TestHooks& _hooks, Testing::FaultPoint _point) noexcept {
    if (_hooks.fault_point != _point || _hooks.fault_fired) {
        return false;
    }
    ++_hooks.matching_hits;
    if (_hooks.matching_hits != std::max<std::uint64_t>(_hooks.trigger_hit, 1)) {
        return false;
    }
    _hooks.fault_fired = true;
    return true;
}

bool ShouldInject(Testing::FaultPoint _point) noexcept {
    Hub&            hub = GetHub();
    std::lock_guard lock(hub.mutex);
    return ShouldInjectLocked(hub.test_hooks, _point);
}

struct WriterFailure {
    RuntimeFault fault{RuntimeFault::WriterException};
};

void InjectOrContinue(Testing::FaultPoint _point, RuntimeFault _fault) {
    if (ShouldInject(_point)) {
        throw WriterFailure{_fault};
    }
}

void LinkTlsShardLocked(Hub& _hub, ThreadLocalShardNode& _shard) noexcept {
    if (_shard.registered.load(std::memory_order_relaxed)) {
        return;
    }
    _shard.registry_previous = nullptr;
    _shard.registry_next     = _hub.tls_shards_head;
    if (_hub.tls_shards_head) {
        _hub.tls_shards_head->registry_previous = &_shard;
    }
    _hub.tls_shards_head = &_shard;
    _shard.registered.store(true, std::memory_order_release);
}

void UnlinkTlsShardLocked(Hub& _hub, ThreadLocalShardNode& _shard) noexcept {
    if (!_shard.registered.load(std::memory_order_relaxed)) {
        return;
    }
    if (_shard.registry_previous) {
        _shard.registry_previous->registry_next = _shard.registry_next;
    } else {
        _hub.tls_shards_head = _shard.registry_next;
    }
    if (_shard.registry_next) {
        _shard.registry_next->registry_previous = _shard.registry_previous;
    }
    _shard.registry_previous = nullptr;
    _shard.registry_next     = nullptr;
    _shard.registered.store(false, std::memory_order_release);
}

std::unique_lock<std::mutex> LockRegisteredTlsShard(ThreadLocalShardNode& _shard) {
    if (_shard.registered.load(std::memory_order_acquire)) {
        return std::unique_lock(_shard.mutex);
    }

    Hub&                         hub = GetHub();
    std::unique_lock<std::mutex> registry_lock(hub.tls_registry_mutex);
    std::unique_lock<std::mutex> shard_lock(_shard.mutex);
    if (!_shard.registered.load(std::memory_order_relaxed)) {
        LinkTlsShardLocked(hub, _shard);
    }
    registry_lock.unlock();
    return shard_lock;
}

void CountDroppedChunk(Hub& _hub, std::uint64_t _record_count, PublishResult _reason) noexcept {
    if (_record_count == 0) {
        return;
    }
    _hub.stats.chunks_dropped.fetch_add(1, std::memory_order_relaxed);
    switch (_reason) {
        case PublishResult::QueueFull:
            _hub.stats.records_dropped_queue_full.fetch_add(_record_count, std::memory_order_relaxed);
            break;
        case PublishResult::Faulted:
            _hub.stats.records_dropped_after_fault.fetch_add(_record_count, std::memory_order_relaxed);
            break;
        case PublishResult::Stale:
            _hub.stats.records_dropped_stale_generation.fetch_add(_record_count, std::memory_order_relaxed);
            break;
        default:
            _hub.stats.records_dropped_stopped.fetch_add(_record_count, std::memory_order_relaxed);
            break;
    }
}

PublishResult PublishDetachedChunk(Hub& _hub, ImmutableChunk&& _chunk) noexcept {
    const std::uint64_t record_count = static_cast<std::uint64_t>(_chunk.records.size());
    if (record_count == 0) {
        return PublishResult::Nothing;
    }

    const std::uint64_t generation     = _chunk.generation;
    const std::uint64_t payload_bytes  = static_cast<std::uint64_t>(_chunk.payload_bytes);
    const std::uint64_t first_sequence = _chunk.first_sequence;
    const std::uint64_t last_sequence  = _chunk.last_sequence;
    const std::uint64_t value_bytes    = _chunk.value_bytes;
    PublishResult       result         = PublishResult::Rejected;
    {
        std::lock_guard     lock(_hub.mutex);
        const RuntimeState  state             = _hub.state.load(std::memory_order_acquire);
        const std::uint64_t active_generation = _hub.generation.load(std::memory_order_acquire);

        if (state == RuntimeState::Faulted) {
            result = PublishResult::Faulted;
        } else if (generation != active_generation || generation == 0) {
            result = PublishResult::Stale;
        } else if (state != RuntimeState::Running) {
            result = PublishResult::Rejected;
        } else {
            const std::uint64_t resident_chunks = _hub.stats.resident_chunks.load(std::memory_order_relaxed);
            const std::uint64_t resident_records =
                _hub.stats.resident_records.load(std::memory_order_relaxed);
            const std::uint64_t resident_bytes = _hub.stats.resident_bytes.load(std::memory_order_relaxed);

            const bool full =
                WouldExceedLimit(
                    resident_chunks, 1, static_cast<std::uint64_t>(_hub.config.queue_max_chunks)
                ) ||
                WouldExceedLimit(
                    resident_records, record_count, static_cast<std::uint64_t>(_hub.config.queue_max_records)
                ) ||
                WouldExceedLimit(
                    resident_bytes, payload_bytes, static_cast<std::uint64_t>(_hub.config.queue_max_bytes)
                );
            if (full) {
                result = PublishResult::QueueFull;
            } else {
                try {
                    _hub.messages.emplace_back(std::move(_chunk));
                    _hub.stats.resident_chunks.store(resident_chunks + 1, std::memory_order_relaxed);
                    _hub.stats.resident_records.store(
                        resident_records + record_count, std::memory_order_relaxed
                    );
                    _hub.stats.resident_bytes.store(
                        resident_bytes + payload_bytes, std::memory_order_relaxed
                    );
                    UpdateHighWater(_hub.stats.high_water_chunks, resident_chunks + 1);
                    UpdateHighWater(_hub.stats.high_water_records, resident_records + record_count);
                    UpdateHighWater(_hub.stats.high_water_bytes, resident_bytes + payload_bytes);
                    _hub.stats.records_enqueued.fetch_add(record_count, std::memory_order_relaxed);
                    _hub.stats.chunks_enqueued.fetch_add(1, std::memory_order_relaxed);
                    result = PublishResult::Accepted;
                } catch (...) {
                    result = PublishResult::QueueFull;
                }
            }
        }
        if (result == PublishResult::QueueFull) {
            AccumulateLossLocked(
                _hub,
                generation,
                first_sequence,
                last_sequence,
                record_count,
                value_bytes,
                LossReason::QueueFull
            );
        }
    }

    if (result == PublishResult::Accepted) {
        _hub.work_cv.notify_one();
    } else {
        CountDroppedChunk(_hub, record_count, result);
        if (result == PublishResult::QueueFull) {
            _hub.work_cv.notify_one();
        }
    }
    return result;
}

PublishResult PublishTlsShardLocked(Hub& _hub, ThreadLocalShardNode& _shard) noexcept {
    if (_shard.records.empty()) {
        _shard.payload_bytes = 0;
        _shard.generation    = 0;
        return PublishResult::Nothing;
    }

    ImmutableChunk chunk{};
    chunk.generation    = _shard.generation;
    chunk.payload_bytes = _shard.payload_bytes;
    chunk.records.swap(_shard.records);
    _shard.payload_bytes = 0;
    _shard.generation    = 0;

    chunk.first_sequence = chunk.records.front().sequence;
    chunk.last_sequence  = chunk.records.front().sequence;
    for (const PendingRecord& record : chunk.records) {
        chunk.first_sequence = std::min(chunk.first_sequence, record.sequence);
        chunk.last_sequence  = std::max(chunk.last_sequence, record.sequence);
        chunk.value_bytes    = SaturatingAdd(chunk.value_bytes, RecordValueBytes(record));
    }
    return PublishDetachedChunk(_hub, std::move(chunk));
}

PublishResult PublishTlsShard(ThreadLocalShardNode& _shard) noexcept {
    Hub&            hub = GetHub();
    std::lock_guard lock(_shard.mutex);
    return PublishTlsShardLocked(hub, _shard);
}

int PublishResultPriority(PublishResult _result) noexcept {
    switch (_result) {
        case PublishResult::Faulted:
            return 5;
        case PublishResult::QueueFull:
            return 4;
        case PublishResult::Rejected:
        case PublishResult::Stale:
            return 3;
        case PublishResult::Accepted:
            return 2;
        case PublishResult::Nothing:
            return 1;
    }
    return 0;
}

PublishResult MergePublishResult(PublishResult _aggregate, PublishResult _next) noexcept {
    return PublishResultPriority(_next) > PublishResultPriority(_aggregate) ? _next : _aggregate;
}

// The caller owns tls_registry_mutex for the entire raw-pointer traversal.
PublishResult HarvestRegisteredTlsShardsLocked(Hub& _hub) noexcept {
    PublishResult aggregate = PublishResult::Nothing;
    for (ThreadLocalShardNode* shard = _hub.tls_shards_head; shard; shard = shard->registry_next) {
        std::lock_guard shard_lock(shard->mutex);
        aggregate = MergePublishResult(aggregate, PublishTlsShardLocked(_hub, *shard));
    }
    return aggregate;
}

ThreadLocalShard::~ThreadLocalShard() {
    Hub&            hub = GetHub();
    std::lock_guard registry_lock(hub.tls_registry_mutex);
    std::lock_guard shard_lock(node.mutex);
    (void)PublishTlsShardLocked(hub, node);
    UnlinkTlsShardLocked(hub, node);
}

std::shared_ptr<Admission> AcquireEmitter(Hub& _hub) noexcept {
    const auto candidate = _hub.admission.load(std::memory_order_acquire);
    if (!candidate) {
        return {};
    }

    std::lock_guard lock(_hub.mutex);
    const auto      current = _hub.admission.load(std::memory_order_acquire);
    if (current != candidate || _hub.state.load(std::memory_order_acquire) != RuntimeState::Running ||
        candidate->closing) {
        return {};
    }
    ++candidate->active_emitters;
    return candidate;
}

void PauseEmitterAfterAdmissionIfRequested(Hub& _hub) noexcept {
    std::unique_lock lock(_hub.mutex);
    if (!_hub.test_hooks.pause_emitter_after_admission) {
        return;
    }
    _hub.test_hooks.emitter_paused = true;
    _hub.test_cv.notify_all();
    _hub.test_cv.wait(lock, [&] {
        return _hub.test_hooks.emitter_resume ||
               _hub.state.load(std::memory_order_acquire) == RuntimeState::Faulted;
    });
    _hub.test_hooks.emitter_paused = false;
}

EmitStatus ReleaseEmitter(const std::shared_ptr<Admission>& _admission, EmitStatus _result) noexcept {
    Hub& hub           = GetHub();
    bool force_publish = false;
    {
        std::lock_guard lock(hub.mutex);
        if (_admission->active_emitters == 0) {
            return _result;
        }
        if (!_admission->closing) {
            --_admission->active_emitters;
            if (_admission->active_emitters == 0) {
                hub.emitter_cv.notify_all();
            }
            return _result;
        }
        // Keep this emitter active while publishing. Shutdown leaves the hub
        // in Running and waits for active_emitters to reach zero, so the
        // sub-threshold TLS chunk remains admissible before Finalize.
        force_publish = true;
    }

    const PublishResult publish_result =
        force_publish ? PublishTlsShard(g_tls_shard.node) : PublishResult::Nothing;

    {
        std::lock_guard lock(hub.mutex);
        if (_admission->active_emitters > 0) {
            --_admission->active_emitters;
        }
        if (_admission->active_emitters == 0) {
            hub.emitter_cv.notify_all();
        }
    }

    if (_result != EmitStatus::Accepted || publish_result == PublishResult::Nothing ||
        publish_result == PublishResult::Accepted) {
        return _result;
    }
    if (publish_result == PublishResult::QueueFull) {
        return EmitStatus::QueueFull;
    }
    if (publish_result == PublishResult::Faulted) {
        return EmitStatus::SinkFault;
    }
    return EmitStatus::Disabled;
}

void DecrementResidentLocked(Hub& _hub, const ImmutableChunk& _chunk) noexcept {
    const std::uint64_t records = static_cast<std::uint64_t>(_chunk.records.size());
    const std::uint64_t bytes   = static_cast<std::uint64_t>(_chunk.payload_bytes);
    _hub.stats.resident_chunks.fetch_sub(1, std::memory_order_relaxed);
    _hub.stats.resident_records.fetch_sub(records, std::memory_order_relaxed);
    _hub.stats.resident_bytes.fetch_sub(bytes, std::memory_order_relaxed);
}

void CountChunkAfterFault(
    Hub&                  _hub,
    const ImmutableChunk& _chunk,
    std::size_t           _records_already_written
) noexcept {
    const std::size_t total = _chunk.records.size();
    if (_records_already_written < total) {
        _hub.stats.records_dropped_after_fault.fetch_add(
            static_cast<std::uint64_t>(total - _records_already_written), std::memory_order_relaxed
        );
    }
    _hub.stats.chunks_dropped.fetch_add(1, std::memory_order_relaxed);
}

void CloseFileNoThrow(std::FILE*& _stream) noexcept {
    if (_stream) {
        std::FILE* closing = std::exchange(_stream, nullptr);
        static_cast<void>(std::fclose(closing));
    }
}

std::FILE* OpenTempFileExclusive(const std::filesystem::path& _path) noexcept {
#if PLATFORM_WINDOWS
    std::FILE* stream = nullptr;
    return ::_wfopen_s(&stream, _path.c_str(), L"wbx") == 0 ? stream : nullptr;
#else
    return std::fopen(_path.c_str(), "wbx");
#endif
}

void FailPendingCrashFlushRequest(Hub& _hub) noexcept;

void FailWriter(
    Hub&                          _hub,
    RuntimeFault                  _fault,
    std::optional<WriterMessage>& _current,
    std::size_t                   _records_already_written,
    std::FILE*&                   _stream
) noexcept {
    _hub.crash_flush_epoch.store(0, std::memory_order_seq_cst);
    CloseFileNoThrow(_stream);

    {
        std::lock_guard lock(_hub.mutex);
        const auto      fault_admission = _hub.admission.load(std::memory_order_acquire);
        if (fault_admission) {
            fault_admission->closing = true;
            _hub.retired_admission   = fault_admission;
        }
        _hub.admission.store({}, std::memory_order_release);
        _hub.state.store(RuntimeState::Faulted, std::memory_order_release);
        _hub.last_fault.store(_fault, std::memory_order_release);
        _hub.file_dirty = false;
        if (IsIoFault(_fault)) {
            _hub.stats.io_faults.fetch_add(1, std::memory_order_relaxed);
        }

        auto reject_message = [&](WriterMessage& message, bool current) {
            if (auto* chunk = std::get_if<ImmutableChunk>(&message)) {
                CountChunkAfterFault(_hub, *chunk, current ? _records_already_written : 0);
                DecrementResidentLocked(_hub, *chunk);
            } else if (auto* flush = std::get_if<FlushMessage>(&message)) {
                _hub.stats.flush_failed.fetch_add(1, std::memory_order_relaxed);
                SignalFence(flush->fence, FlushResult::Faulted);
            } else if (auto* finalize = std::get_if<FinalizeMessage>(&message)) {
                _hub.stats.flush_failed.fetch_add(1, std::memory_order_relaxed);
                SignalFence(finalize->fence, FlushResult::Faulted);
            }
        };

        if (_current) {
            reject_message(*_current, true);
        }
        while (!_hub.messages.empty()) {
            WriterMessage message = std::move(_hub.messages.front());
            _hub.messages.pop_front();
            reject_message(message, false);
        }
        _hub.active_flush_fence.reset();
    }

    FailPendingCrashFlushRequest(_hub);
    _hub.work_cv.notify_all();
    _hub.test_cv.notify_all();
    _hub.emitter_cv.notify_all();
}

void WritePacketChecked(
    Hub&                          _hub,
    std::FILE*                    _stream,
    PacketType                    _type,
    std::uint64_t&                _packet_index,
    std::span<const std::uint8_t> _payload
) {
    InjectOrContinue(Testing::FaultPoint::WritePacket, RuntimeFault::WritePacket);

    Array<std::uint8_t> packet;
    if (WrapPacket(_type, _packet_index++, _payload, _hub.config.codec_limits, packet) != EncodeStatus::Ok) {
        throw WriterFailure{RuntimeFault::WriterException};
    }

    const std::size_t written = std::fwrite(packet.data(), 1, packet.size(), _stream);
    if (written != packet.size() || std::ferror(_stream) != 0) {
        throw WriterFailure{RuntimeFault::WritePacket};
    }
    _hub.stats.file_bytes_written.fetch_add(
        static_cast<std::uint64_t>(packet.size()), std::memory_order_relaxed
    );
}

void WriteLossChecked(Hub& _hub, std::FILE* _stream, std::uint64_t& _packet_index, const LossNotice& _loss) {
    Array<std::uint8_t> loss_payload;
    EncodeLossPayload(_loss, loss_payload);
    WritePacketChecked(_hub, _stream, PacketType::Loss, _packet_index, loss_payload);
    std::lock_guard lock(_hub.mutex);
    _hub.file_dirty = true;
}

void FlushFileChecked(std::FILE* _stream) {
    InjectOrContinue(Testing::FaultPoint::FlushFile, RuntimeFault::FlushFile);
    if (std::fflush(_stream) != 0) {
        throw WriterFailure{RuntimeFault::FlushFile};
    }
}

void CloseFileChecked(std::FILE*& _stream) {
    InjectOrContinue(Testing::FaultPoint::CloseFile, RuntimeFault::CloseFile);
    std::FILE* closing = std::exchange(_stream, nullptr);
    if (!closing || std::fclose(closing) != 0) {
        throw WriterFailure{RuntimeFault::CloseFile};
    }
}

void RenameFinalChecked(Hub& _hub) {
    InjectOrContinue(Testing::FaultPoint::RenameFinal, RuntimeFault::RenameFinal);
#if PLATFORM_WINDOWS
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (_hub.config.replace_existing) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }

    constexpr std::uint32_t max_attempts = 8;
    for (std::uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
        if (::MoveFileExW(_hub.temp_path.c_str(), _hub.config.output_path.c_str(), flags)) {
            return;
        }
        const DWORD error = ::GetLastError();
        const bool  transient =
            error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_ACCESS_DENIED;
        if (!transient || attempt + 1 == max_attempts) {
            throw WriterFailure{RuntimeFault::RenameFinal};
        }
        const auto backoff = std::chrono::milliseconds(1U << std::min<std::uint32_t>(attempt, 5U));
        std::this_thread::sleep_for(backoff);
    }
#else
    if (_hub.config.replace_existing) {
        std::error_code error;
        std::filesystem::rename(_hub.temp_path, _hub.config.output_path, error);
        if (error) {
            throw WriterFailure{RuntimeFault::RenameFinal};
        }
    } else {
        // POSIX link() creates the final name atomically and fails with
        // EEXIST instead of replacing a concurrently-created final. The temp
        // and final live in the same directory, so they are on one filesystem.
        if (::link(_hub.temp_path.c_str(), _hub.config.output_path.c_str()) != 0) {
            throw WriterFailure{RuntimeFault::RenameFinal};
        }
        // Publication already succeeded. A best-effort unlink avoids turning
        // a cleanup-only failure into a false capture failure with a visible
        // valid final.
        static_cast<void>(::unlink(_hub.temp_path.c_str()));
    }
#endif
}

void SignalStartFailure(Hub& _hub, RuntimeFault _fault, StartResult _result, std::FILE*& _stream) noexcept {
    const bool owns_temp = _stream != nullptr;
    CloseFileNoThrow(_stream);
    if (owns_temp) {
        std::error_code ignored;
        std::filesystem::remove(_hub.temp_path, ignored);
    }

    {
        std::lock_guard lock(_hub.mutex);
        _hub.admission.store({}, std::memory_order_release);
        _hub.state.store(RuntimeState::Stopped, std::memory_order_release);
        _hub.last_fault.store(_fault, std::memory_order_release);
        if (IsIoFault(_fault)) {
            _hub.stats.io_faults.fetch_add(1, std::memory_order_relaxed);
        }
        _hub.start_result   = _result;
        _hub.start_complete = true;
    }
    _hub.start_cv.notify_all();
}

void CompleteCrashFlushRequest(Hub& _hub, std::uint64_t _target, bool _succeeded) noexcept {
    if (_target == 0) {
        return;
    }
    if (!_succeeded) {
        _hub.crash_flush_failed.store(_target, std::memory_order_release);
    }
    _hub.crash_flush_completed.store(_target, std::memory_order_release);
}

void FailPendingCrashFlushRequest(Hub& _hub) noexcept {
    const std::uint64_t requested =
        _hub.crash_flush_requested.load(std::memory_order_acquire);
    const std::uint64_t completed =
        _hub.crash_flush_completed.load(std::memory_order_acquire);
    if (requested > completed) {
        CompleteCrashFlushRequest(_hub, requested, false);
    }
}

class WriterThreadIdentity final {
public:
    explicit WriterThreadIdentity(Hub& _hub) noexcept : hub(_hub) {
        hub.writer_thread_id.store(
            Platform::GetCurrentThreadID(), std::memory_order_release
        );
    }

    ~WriterThreadIdentity() {
        hub.crash_flush_epoch.store(0, std::memory_order_seq_cst);
        hub.writer_thread_id.store(0, std::memory_order_seq_cst);
        const std::uint64_t requested =
            hub.crash_flush_requested.load(std::memory_order_seq_cst);
        const std::uint64_t completed =
            hub.crash_flush_completed.load(std::memory_order_acquire);
        if (requested > completed) {
            CompleteCrashFlushRequest(hub, requested, durable_exit);
        }
    }

    void MarkDurableExit() noexcept {
        durable_exit = true;
    }

    WriterThreadIdentity(const WriterThreadIdentity&)            = delete;
    WriterThreadIdentity& operator=(const WriterThreadIdentity&) = delete;

private:
    Hub& hub;
    bool durable_exit{false};
};

std::uint64_t DroppedRecordCount(const Hub& _hub) noexcept {
    return _hub.stats.records_dropped_stopped.load(std::memory_order_relaxed) +
           _hub.stats.records_dropped_stale_generation.load(std::memory_order_relaxed) +
           _hub.stats.records_dropped_oversized.load(std::memory_order_relaxed) +
           _hub.stats.records_dropped_queue_full.load(std::memory_order_relaxed) +
           _hub.stats.records_dropped_after_fault.load(std::memory_order_relaxed);
}

void WriterMain(Hub& _hub, std::shared_ptr<Admission> _session_admission) noexcept {
    WriterThreadIdentity writer_identity(_hub);
    Platform::SetCurrentThreadName("Moer ProfileDump Writer");

    std::FILE*    stream       = nullptr;
    std::uint64_t packet_index = 0;
    try {
        InjectOrContinue(Testing::FaultPoint::OpenTempFile, RuntimeFault::OpenTempFile);
        const std::filesystem::path parent = _hub.temp_path.parent_path();
        if (!parent.empty()) {
            std::error_code directory_error;
            std::filesystem::create_directories(parent, directory_error);
            if (directory_error) {
                throw WriterFailure{RuntimeFault::OpenTempFile};
            }
        }
        {
            std::unique_lock lock(_hub.mutex);
            if (_hub.test_hooks.pause_before_temp_open) {
                _hub.test_hooks.writer_paused = true;
                _hub.test_cv.notify_all();
                _hub.test_cv.wait(lock, [&] {
                    return _hub.test_hooks.writer_resume ||
                           _hub.state.load(std::memory_order_acquire) != RuntimeState::Starting;
                });
                _hub.test_hooks.writer_paused = false;
            }
        }
        stream = OpenTempFileExclusive(_hub.temp_path);
        if (!stream) {
            throw WriterFailure{RuntimeFault::OpenTempFile};
        }

        Array<std::uint8_t> session_payload;
        const auto          now = std::chrono::system_clock::now().time_since_epoch();
        EncodeSessionBeginPayload(
            SessionBeginInfo{
                .generation      = _hub.generation.load(std::memory_order_acquire),
                .started_unix_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
                ),
            },
            session_payload
        );
        WritePacketChecked(_hub, stream, PacketType::SessionBegin, packet_index, session_payload);
    } catch (const WriterFailure& failure) {
        SignalStartFailure(_hub, failure.fault, StartResult::FileOpenFailed, stream);
        return;
    } catch (const std::bad_alloc&) {
        SignalStartFailure(_hub, RuntimeFault::WriterException, StartResult::ResourceExhausted, stream);
        return;
    } catch (...) {
        SignalStartFailure(_hub, RuntimeFault::WriterException, StartResult::FileOpenFailed, stream);
        return;
    }

    {
        std::lock_guard lock(_hub.mutex);
        _hub.file_dirty = true;
        _hub.state.store(RuntimeState::Running, std::memory_order_release);
        _hub.admission.store(std::move(_session_admission), std::memory_order_release);
        _hub.crash_flush_epoch.store(
            _hub.generation.load(std::memory_order_acquire),
            std::memory_order_seq_cst
        );
        _hub.start_result   = StartResult::Started;
        _hub.start_complete = true;
    }
    _hub.start_cv.notify_all();

    {
        std::unique_lock lock(_hub.mutex);
        if (_hub.test_hooks.pause_after_start) {
            _hub.test_hooks.writer_paused = true;
            _hub.test_cv.notify_all();
            _hub.test_cv.wait(lock, [&] {
                const RuntimeState state = _hub.state.load(std::memory_order_acquire);
                return _hub.test_hooks.writer_resume || state == RuntimeState::Draining ||
                       state == RuntimeState::Faulted;
            });
            _hub.test_hooks.writer_paused = false;
        }
    }

    std::unordered_set<std::uint64_t> emitted_schemas;
    for (;;) {
        std::optional<WriterMessage> current;
        std::optional<LossNotice>    pending_loss;
        std::uint64_t                crash_flush_target = 0;
        std::size_t                  records_written_from_current = 0;
        try {
            InjectOrContinue(Testing::FaultPoint::WriterException, RuntimeFault::WriterException);

            {
                std::unique_lock lock(_hub.mutex);
                const auto has_work = [&] {
                    return !_hub.messages.empty() || _hub.pending_loss.pending ||
                           _hub.crash_flush_requested.load(std::memory_order_acquire) >
                               _hub.crash_flush_completed.load(std::memory_order_acquire) ||
                           _hub.state.load(std::memory_order_acquire) == RuntimeState::Faulted;
                };
                while (!has_work()) {
                    if (_hub.test_hooks.pause_before_idle_wait &&
                        !_hub.test_hooks.writer_idle_wait_resume) {
                        _hub.test_hooks.writer_idle_wait_paused = true;
                        _hub.test_cv.notify_all();
                        _hub.test_cv.wait(lock, [&] {
                            return _hub.test_hooks.writer_idle_wait_resume;
                        });
                        _hub.test_hooks.writer_idle_wait_paused = false;
                    }
                    // The crash publisher deliberately cannot take Hub::mutex:
                    // it may be running because the faulting thread already
                    // owns that mutex. A notification can therefore land after
                    // has_work() and before this wait. The bounded publisher
                    // repeats notify_one() until its ticket is settled.
                    _hub.work_cv.wait_for(lock, 10ms);
                }
                if (_hub.state.load(std::memory_order_acquire) == RuntimeState::Faulted) {
                    RuntimeFault fault = _hub.last_fault.load(std::memory_order_acquire);
                    if (fault == RuntimeFault::None) {
                        fault = RuntimeFault::WriterException;
                    }
                    throw WriterFailure{fault};
                }
                const std::uint64_t requested =
                    _hub.crash_flush_requested.load(std::memory_order_acquire);
                const std::uint64_t completed =
                    _hub.crash_flush_completed.load(std::memory_order_acquire);
                if (requested > completed) {
                    crash_flush_target = requested;
                } else if (_hub.messages.empty() && !_hub.pending_loss.pending) {
                    return;
                } else {
                    pending_loss =
                        TakePendingLossLocked(
                            _hub, _hub.generation.load(std::memory_order_acquire)
                        );
                    if (pending_loss) {
                        // A concurrent FlushAll must observe work in flight
                        // even after the fixed loss slot has been atomically
                        // taken.
                        _hub.file_dirty = true;
                    }
                    if (!_hub.messages.empty()) {
                        current.emplace(std::move(_hub.messages.front()));
                        _hub.messages.pop_front();
                        if (auto* flush = std::get_if<FlushMessage>(&*current)) {
                            _hub.active_flush_fence = flush->fence;
                        } else if (std::holds_alternative<FinalizeMessage>(
                                       *current
                                   )) {
                            // Close crash-flush admission while the same mutex
                            // still protects the finalization queue frontier.
                            _hub.crash_flush_epoch.store(
                                0, std::memory_order_seq_cst
                            );
                        }
                    }
                }
            }

            if (crash_flush_target != 0) {
                FlushFileChecked(stream);
                CompleteCrashFlushRequest(_hub, crash_flush_target, true);
                continue;
            }

            // Loss has one fixed process slot. The writer atomically takes
            // the current aggregate before each queued message and writes it
            // first. Sequence bounds describe the observed dropped interval;
            // they do not impose a total order on TLS chunks that publish
            // later with an older reserved sequence.
            if (pending_loss) {
                WriteLossChecked(_hub, stream, packet_index, *pending_loss);
            }
            if (!current) {
                continue;
            }

            if (auto* chunk = std::get_if<ImmutableChunk>(&*current)) {
                const auto schemas = _hub.schemas.load(std::memory_order_acquire);
                for (const PendingRecord& record : chunk->records) {
                    const auto schema_it = schemas->find(record.schema_hash);
                    if (schema_it == schemas->end()) {
                        throw WriterFailure{RuntimeFault::WriterException};
                    }

                    if (!emitted_schemas.contains(record.schema_hash)) {
                        Array<std::uint8_t> schema_payload;
                        if (EncodeSchemaPayload(
                                schema_it->second->descriptor, _hub.config.codec_limits, schema_payload
                            ) != EncodeStatus::Ok) {
                            throw WriterFailure{RuntimeFault::WriterException};
                        }
                        WritePacketChecked(_hub, stream, PacketType::Schema, packet_index, schema_payload);
                        emitted_schemas.emplace(record.schema_hash);
                    }

                    WritePacketChecked(_hub, stream, PacketType::Record, packet_index, record.payload);
                    ++records_written_from_current;
                    _hub.stats.records_written.fetch_add(1, std::memory_order_relaxed);
                }

                {
                    std::lock_guard lock(_hub.mutex);
                    DecrementResidentLocked(_hub, *chunk);
                    _hub.file_dirty = true;
                }
                _hub.stats.chunks_written.fetch_add(1, std::memory_order_relaxed);
                current.reset();
                continue;
            }

            if (auto* flush = std::get_if<FlushMessage>(&*current)) {
                FlushFileChecked(stream);
                {
                    std::lock_guard lock(_hub.mutex);
                    _hub.file_dirty = false;
                    if (_hub.active_flush_fence == flush->fence) {
                        _hub.active_flush_fence.reset();
                    }
                }
                _hub.stats.flush_completed.fetch_add(1, std::memory_order_relaxed);
                SignalFence(flush->fence, FlushResult::Completed);
                current.reset();
                continue;
            }

            auto* finalize = std::get_if<FinalizeMessage>(&*current);
            if (!finalize) {
                throw WriterFailure{RuntimeFault::WriterException};
            }

            // Admission is closed before Finalize is enqueued. Drain the
            // single slot once more immediately before SessionEnd so every
            // clean-session loss published before the shutdown linearization
            // point is represented in the file.
            std::optional<LossNotice> final_loss;
            {
                std::lock_guard lock(_hub.mutex);
                final_loss = TakePendingLossLocked(_hub, _hub.generation.load(std::memory_order_acquire));
            }
            if (final_loss) {
                WriteLossChecked(_hub, stream, packet_index, *final_loss);
            }

            Array<std::uint8_t> end_payload;
            EncodeSessionEndPayload(
                SessionEndInfo{
                    .generation      = _hub.generation.load(std::memory_order_acquire),
                    .records_written = _hub.stats.records_written.load(std::memory_order_relaxed),
                    .records_dropped = DroppedRecordCount(_hub),
                },
                end_payload
            );
            WritePacketChecked(_hub, stream, PacketType::SessionEnd, packet_index, end_payload);
            FlushFileChecked(stream);
            CompleteCrashFlushRequest(
                _hub,
                _hub.crash_flush_requested.load(std::memory_order_seq_cst),
                true
            );
            CloseFileChecked(stream);
            RenameFinalChecked(_hub);

            {
                std::lock_guard lock(_hub.mutex);
                _hub.file_dirty = false;
                _hub.state.store(RuntimeState::Stopped, std::memory_order_release);
            }
            writer_identity.MarkDurableExit();
            _hub.stats.flush_completed.fetch_add(1, std::memory_order_relaxed);
            SignalFence(finalize->fence, FlushResult::Completed);
            _hub.work_cv.notify_all();
            return;
        } catch (const WriterFailure& failure) {
            FailWriter(_hub, failure.fault, current, records_written_from_current, stream);
            return;
        } catch (...) {
            FailWriter(_hub, RuntimeFault::WriterException, current, records_written_from_current, stream);
            return;
        }
    }
}

bool ValidateConfig(const RuntimeConfig& _config) noexcept {
    if (_config.output_path.empty() || _config.max_schemas == 0 || _config.max_schema_bytes == 0 ||
        _config.max_record_bytes == 0 || _config.tls_publish_records == 0 || _config.tls_publish_bytes == 0 ||
        _config.tls_max_records == 0 || _config.tls_max_bytes == 0 || _config.queue_max_chunks == 0 ||
        _config.queue_max_records == 0 || _config.queue_max_bytes == 0) {
        return false;
    }
    if (_config.tls_publish_records > _config.tls_max_records ||
        _config.tls_publish_bytes > _config.tls_max_bytes ||
        _config.max_record_bytes > _config.tls_max_bytes ||
        _config.max_record_bytes > _config.queue_max_bytes ||
        _config.tls_max_records > _config.queue_max_records ||
        _config.tls_max_bytes > _config.queue_max_bytes ||
        _config.max_record_bytes > _config.codec_limits.max_packet_payload_bytes ||
        _config.codec_limits.max_fields == 0 || _config.codec_limits.max_string_bytes == 0 ||
        _config.codec_limits.max_packet_payload_bytes == 0) {
        return false;
    }
    return true;
}

void ReapWorker(Hub& _hub) noexcept {
    std::lock_guard join_lock(_hub.worker_join_mutex);
    if (_hub.worker.joinable()) {
        _hub.worker.join();
    }
}

EmitStatus MapEncodeStatus(EncodeStatus _status) noexcept {
    switch (_status) {
        case EncodeStatus::Ok:
            return EmitStatus::Accepted;
        case EncodeStatus::ValueCountMismatch:
            return EmitStatus::ValueCountMismatch;
        case EncodeStatus::ValueTypeMismatch:
            return EmitStatus::ValueTypeMismatch;
        case EncodeStatus::StringTooLarge:
            return EmitStatus::StringTooLarge;
        case EncodeStatus::PayloadTooLarge:
            return EmitStatus::RecordTooLarge;
        default:
            return EmitStatus::InvalidHandle;
    }
}

std::shared_ptr<FlushFence> PrepareFlushLocked(Hub& _hub, FlushResult& _immediate_result) noexcept {
    const RuntimeState state = _hub.state.load(std::memory_order_acquire);
    if (state == RuntimeState::Faulted) {
        _immediate_result = FlushResult::Faulted;
        return {};
    }
    if (state != RuntimeState::Running) {
        _immediate_result = FlushResult::Rejected;
        return {};
    }
    if (_hub.stats.resident_chunks.load(std::memory_order_relaxed) == 0 && !_hub.file_dirty &&
        !_hub.pending_loss.pending) {
        _immediate_result = FlushResult::NothingPending;
        return {};
    }

    std::shared_ptr<FlushFence> fence;
    if (!_hub.messages.empty()) {
        if (auto* tail_flush = std::get_if<FlushMessage>(&_hub.messages.back())) {
            fence = tail_flush->fence;
        }
    } else if (_hub.active_flush_fence && !_hub.pending_loss.pending) {
        fence = _hub.active_flush_fence;
    }

    if (!fence) {
        try {
            fence = std::make_shared<FlushFence>();
            _hub.messages.emplace_back(FlushMessage{.fence = fence});
        } catch (...) {
            _immediate_result = FlushResult::Rejected;
            return {};
        }
    }
    _immediate_result = FlushResult::Completed;
    return fence;
}

FlushResult WaitPreparedFlush(
    Hub&                               _hub,
    const std::shared_ptr<FlushFence>& _fence,
    FlushResult                        _immediate_result
) noexcept {
    if (!_fence) {
        return _immediate_result;
    }
    _hub.work_cv.notify_one();
    return WaitFence(_fence);
}

FlushResult FlushPublishedInterval() noexcept {
    Hub&                        hub = GetHub();
    std::shared_ptr<FlushFence> fence;
    FlushResult                 immediate_result = FlushResult::Rejected;
    {
        std::lock_guard lock(hub.mutex);
        fence = PrepareFlushLocked(hub, immediate_result);
    }
    return WaitPreparedFlush(hub, fence, immediate_result);
}

FlushResult ResolveHarvestedFlush(PublishResult _published, FlushResult _flushed) noexcept {
    if (_published == PublishResult::Faulted || _flushed == FlushResult::Faulted) {
        return FlushResult::Faulted;
    }
    if (_flushed == FlushResult::Rejected || _published == PublishResult::QueueFull ||
        _published == PublishResult::Rejected || _published == PublishResult::Stale) {
        return FlushResult::Rejected;
    }
    if (_published == PublishResult::Nothing && _flushed == FlushResult::NothingPending) {
        return FlushResult::NothingPending;
    }
    return _flushed == FlushResult::NothingPending ? FlushResult::Completed : _flushed;
}

} // namespace

StartResult Start(const RuntimeConfig& _config) noexcept {
    try {
        Hub&            hub = GetHub();
        std::lock_guard lifecycle_lock(hub.lifecycle_mutex);

        const RuntimeState initial_state = hub.state.load(std::memory_order_acquire);
        if (initial_state == RuntimeState::Running) {
            return StartResult::AlreadyRunning;
        }
        if (initial_state != RuntimeState::Stopped) {
            return StartResult::Busy;
        }
        if (!ValidateConfig(_config)) {
            return StartResult::InvalidConfig;
        }

        ReapWorker(hub);

        if (ShouldInject(Testing::FaultPoint::StartAllocation)) {
            throw std::bad_alloc{};
        }
        RuntimeConfig         prepared_config   = _config;
        std::filesystem::path temp_path         = MakeSessionTempPath(prepared_config.output_path);
        auto                  initial_schemas   = std::make_shared<const SchemaMap>();
        auto                  session_admission = std::make_shared<Admission>();
        session_admission->config               = prepared_config;

        std::error_code error;
        const bool      final_exists = std::filesystem::exists(prepared_config.output_path, error);
        if (error) {
            return StartResult::FileOpenFailed;
        }
        if (!prepared_config.replace_existing && final_exists) {
            return StartResult::OutputExists;
        }
        // Every session exclusively creates and owns a unique temp path.
        // Replacement applies only to the final output and must never unlink
        // another process's live or forensic in-progress capture.

        // A completed Shutdown normally leaves every live TLS node empty.
        // Defensively discard any stopped-generation residue before resetting
        // statistics and publishing the next generation.
        {
            std::lock_guard registry_lock(hub.tls_registry_mutex);
            (void)HarvestRegisteredTlsShardsLocked(hub);
        }

        {
            std::lock_guard lock(hub.mutex);
            hub.config    = std::move(prepared_config);
            hub.temp_path = std::move(temp_path);
            hub.messages.clear();
            hub.file_dirty   = false;
            hub.pending_loss = {};
            hub.schema_bytes = 0;
            hub.shutdown_fence.reset();
            hub.active_flush_fence.reset();
            hub.retired_admission.reset();
            hub.start_complete = false;
            hub.start_result   = StartResult::FileOpenFailed;
            hub.stats.Reset();
            const std::uint64_t generation = ++hub.generation_counter;
            session_admission->generation  = generation;
            hub.generation.store(generation, std::memory_order_seq_cst);
            const std::uint64_t settled_crash_request =
                hub.crash_flush_requested.load(std::memory_order_seq_cst);
            hub.crash_flush_completed.store(
                settled_crash_request, std::memory_order_seq_cst
            );
            hub.crash_flush_failed.store(0, std::memory_order_seq_cst);
            hub.crash_flush_epoch.store(0, std::memory_order_seq_cst);
            hub.last_fault.store(RuntimeFault::None, std::memory_order_release);
            hub.schemas.store(std::move(initial_schemas), std::memory_order_release);
            hub.admission.store({}, std::memory_order_release);
            hub.test_hooks.matching_hits  = 0;
            hub.test_hooks.fault_fired    = false;
            hub.test_hooks.writer_paused  = false;
            hub.test_hooks.writer_resume  = false;
            hub.test_hooks.writer_idle_wait_paused = false;
            hub.test_hooks.writer_idle_wait_resume = false;
            hub.test_hooks.emitter_paused = false;
            hub.test_hooks.emitter_resume = false;
            hub.test_crash_flush_before_publish_paused.store(
                false, std::memory_order_release
            );
            hub.test_crash_flush_before_publish_resume.store(
                false, std::memory_order_release
            );
            hub.state.store(RuntimeState::Starting, std::memory_order_release);
        }

        if (ShouldInject(Testing::FaultPoint::BeforeThreadCreate)) {
            hub.state.store(RuntimeState::Stopped, std::memory_order_release);
            return StartResult::ThreadCreateFailed;
        }

        try {
            hub.worker = std::jthread([&hub, admission = std::move(session_admission)]() mutable {
                WriterMain(hub, std::move(admission));
            });
        } catch (...) {
            hub.state.store(RuntimeState::Stopped, std::memory_order_release);
            return StartResult::ThreadCreateFailed;
        }

        StartResult result = StartResult::FileOpenFailed;
        {
            std::unique_lock lock(hub.mutex);
            hub.start_cv.wait(lock, [&] {
                return hub.start_complete;
            });
            result = hub.start_result;
        }
        if (result != StartResult::Started) {
            ReapWorker(hub);
        }
        return result;
    } catch (...) {
        return StartResult::ResourceExhausted;
    }
}

SchemaRegistration RegisterSchema(const SchemaDescriptor& _schema) noexcept {
    Hub&       hub       = GetHub();
    const auto admission = hub.admission.load(std::memory_order_acquire);
    if (!admission) {
        return {.status = SchemaStatus::NotRunning};
    }

    Array<std::uint8_t> encoded;
    try {
        if (EncodeSchemaPayload(_schema, admission->config.codec_limits, encoded) != EncodeStatus::Ok) {
            return {.status = SchemaStatus::InvalidSchema};
        }
    } catch (...) {
        return {.status = SchemaStatus::InvalidSchema};
    }

    const std::uint64_t hash = ComputeSchemaHash(_schema);
    if (hash == 0) {
        return {.status = SchemaStatus::InvalidSchema};
    }

    std::lock_guard lock(hub.mutex);
    const auto      current_admission = hub.admission.load(std::memory_order_acquire);
    if (!current_admission || current_admission->generation != admission->generation ||
        hub.state.load(std::memory_order_acquire) != RuntimeState::Running) {
        return {.status = SchemaStatus::NotRunning};
    }

    const std::uint64_t generation = admission->generation;
    const auto          current    = hub.schemas.load(std::memory_order_acquire);
    if (const auto found = current->find(hash); found != current->end()) {
        if (found->second->descriptor == _schema) {
            return {
                .status = SchemaStatus::AlreadyRegistered,
                .handle =
                    SchemaHandle{
                        .hash       = hash,
                        .generation = generation,
                    },
            };
        }
        return {.status = SchemaStatus::HashCollision};
    }

    if (current->size() >= admission->config.max_schemas) {
        return {.status = SchemaStatus::SchemaLimit};
    }
    if (encoded.size() > admission->config.max_schema_bytes - hub.schema_bytes) {
        return {.status = SchemaStatus::SchemaBytesLimit};
    }

    try {
        auto next = std::make_shared<SchemaMap>(*current);
        next->emplace(
            hash,
            std::make_shared<const SchemaEntry>(SchemaEntry{
                .descriptor    = _schema,
                .encoded_bytes = encoded.size(),
            })
        );
        hub.schema_bytes += encoded.size();
        hub.schemas.store(std::move(next), std::memory_order_release);
    } catch (...) {
        return {.status = SchemaStatus::SchemaLimit};
    }

    return {
        .status = SchemaStatus::Registered,
        .handle =
            SchemaHandle{
                .hash       = hash,
                .generation = generation,
            },
    };
}

EmitStatus Emit(SchemaHandle _schema, std::span<const FieldValueView> _values) noexcept {
    Hub&       hub       = GetHub();
    const auto admission = AcquireEmitter(hub);
    if (!admission) {
        if (hub.state.load(std::memory_order_acquire) == RuntimeState::Faulted) {
            hub.stats.records_dropped_after_fault.fetch_add(1, std::memory_order_relaxed);
            return EmitStatus::SinkFault;
        }
        hub.stats.records_dropped_stopped.fetch_add(1, std::memory_order_relaxed);
        return EmitStatus::Disabled;
    }
    const auto finish = [&](EmitStatus _result) noexcept {
        return ReleaseEmitter(admission, _result);
    };
    PauseEmitterAfterAdmissionIfRequested(hub);

    if (!_schema || _schema.generation != admission->generation) {
        hub.stats.records_dropped_stale_generation.fetch_add(1, std::memory_order_relaxed);
        return finish(EmitStatus::InvalidHandle);
    }

    const auto schemas   = hub.schemas.load(std::memory_order_acquire);
    const auto schema_it = schemas->find(_schema.hash);
    if (schema_it == schemas->end()) {
        return finish(EmitStatus::InvalidHandle);
    }

    const std::uint64_t sequence = admission->next_sequence.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t attempted_value_bytes = EstimateValueBytes(_values);
    Array<std::uint8_t> payload;
    EncodeStatus        encode_status = EncodeStatus::InvalidSchema;
    try {
        encode_status = EncodeRecordPayload(
            _schema.hash,
            sequence,
            schema_it->second->descriptor.fields,
            _values,
            admission->config.codec_limits,
            payload
        );
    } catch (...) {
        hub.stats.records_dropped_queue_full.fetch_add(1, std::memory_order_relaxed);
        AccumulateLoss(
            admission->generation, sequence, sequence, 1, attempted_value_bytes, LossReason::QueueFull
        );
        return finish(EmitStatus::QueueFull);
    }
    if (encode_status != EncodeStatus::Ok) {
        if (encode_status == EncodeStatus::PayloadTooLarge || encode_status == EncodeStatus::StringTooLarge) {
            hub.stats.records_dropped_oversized.fetch_add(1, std::memory_order_relaxed);
            AccumulateLoss(
                admission->generation, sequence, sequence, 1, attempted_value_bytes, LossReason::Oversized
            );
        }
        return finish(MapEncodeStatus(encode_status));
    }
    if (payload.size() > admission->config.max_record_bytes ||
        payload.size() > admission->config.tls_max_bytes) {
        hub.stats.records_dropped_oversized.fetch_add(1, std::memory_order_relaxed);
        AccumulateLoss(
            admission->generation,
            sequence,
            sequence,
            1,
            EncodedRecordValueBytes(payload),
            LossReason::Oversized
        );
        return finish(EmitStatus::RecordTooLarge);
    }

    const std::size_t     record_bytes = payload.size();
    PublishResult         current_publish_result{PublishResult::Nothing};
    bool                  append_failed{false};
    ThreadLocalShardNode& tls_shard = g_tls_shard.node;
    {
        // The first append links this TLS node while holding registry -> shard.
        // Subsequent appends use only the shard lock. Publication may then
        // acquire Hub::mutex, preserving the one-way lock order.
        auto shard_lock = LockRegisteredTlsShard(tls_shard);

        if (tls_shard.generation != 0 && tls_shard.generation != admission->generation) {
            (void)PublishTlsShardLocked(hub, tls_shard);
        }
        if (tls_shard.generation == 0) {
            tls_shard.generation = admission->generation;
        }

        if (!tls_shard.records.empty() &&
            (tls_shard.records.size() >= admission->config.tls_max_records ||
             payload.size() > admission->config.tls_max_bytes - tls_shard.payload_bytes)) {
            (void)PublishTlsShardLocked(hub, tls_shard);
            tls_shard.generation = admission->generation;
        }

        try {
            tls_shard.records.emplace_back(PendingRecord{
                .schema_hash = _schema.hash,
                .sequence    = sequence,
                .payload     = std::move(payload),
            });
            tls_shard.payload_bytes += record_bytes;
        } catch (...) {
            append_failed = true;
        }

        if (!append_failed) {
            hub.stats.records_committed.fetch_add(1, std::memory_order_relaxed);
            const bool publish = tls_shard.records.size() >= admission->config.tls_publish_records ||
                                 tls_shard.payload_bytes >= admission->config.tls_publish_bytes ||
                                 tls_shard.records.size() >= admission->config.tls_max_records ||
                                 tls_shard.payload_bytes >= admission->config.tls_max_bytes;
            if (publish) {
                current_publish_result = PublishTlsShardLocked(hub, tls_shard);
            }
        }
    }

    // Release shard ownership before any path reaches ReleaseEmitter(); a
    // closing admission may publish this same TLS shard from that function.
    if (append_failed) {
        hub.stats.records_dropped_queue_full.fetch_add(1, std::memory_order_relaxed);
        AccumulateLoss(
            admission->generation, sequence, sequence, 1, attempted_value_bytes, LossReason::QueueFull
        );
        return finish(EmitStatus::QueueFull);
    }

    if (current_publish_result == PublishResult::QueueFull) {
        return finish(EmitStatus::QueueFull);
    }
    if (current_publish_result == PublishResult::Faulted) {
        return finish(EmitStatus::SinkFault);
    }
    if (current_publish_result != PublishResult::Nothing &&
        current_publish_result != PublishResult::Accepted) {
        return finish(EmitStatus::Disabled);
    }
    return finish(EmitStatus::Accepted);
}

FlushResult FlushThreadLocal() noexcept {
    const PublishResult published = PublishTlsShard(g_tls_shard.node);
    return ResolveHarvestedFlush(published, FlushPublishedInterval());
}

FlushResult FlushAll() noexcept {
    Hub&                        hub = GetHub();
    std::shared_ptr<FlushFence> fence;
    FlushResult                 immediate_result = FlushResult::Rejected;
    PublishResult               harvested        = PublishResult::Nothing;
    {
        // Pin every intrusive node through traversal and append the writer
        // fence before a TLS destructor or first registration can cross the
        // registry frontier.
        std::lock_guard registry_lock(hub.tls_registry_mutex);
        harvested = HarvestRegisteredTlsShardsLocked(hub);
        std::lock_guard hub_lock(hub.mutex);
        fence = PrepareFlushLocked(hub, immediate_result);
    }
    const FlushResult flushed = WaitPreparedFlush(hub, fence, immediate_result);
    return ResolveHarvestedFlush(harvested, flushed);
}

CrashFlushResult FlushCrashPublishedPrefix(std::uint32_t _timeout_ms) noexcept {
    Hub* const hub_ptr = TryGetHub();
    if (!hub_ptr) {
        return CrashFlushResult::NotRunning;
    }
    Hub&               hub   = *hub_ptr;
    const RuntimeState state = hub.state.load(std::memory_order_acquire);
    if (state == RuntimeState::Faulted) {
        return CrashFlushResult::Faulted;
    }
    if (state != RuntimeState::Running) {
        return CrashFlushResult::NotRunning;
    }

    const std::uint64_t generation =
        hub.generation.load(std::memory_order_seq_cst);
    const std::uint64_t epoch_before =
        hub.crash_flush_epoch.load(std::memory_order_seq_cst);
    if (epoch_before == 0 || epoch_before != generation) {
        return CrashFlushResult::NotRunning;
    }
    const std::uint32_t writer_thread_id =
        hub.writer_thread_id.load(std::memory_order_acquire);
    if (writer_thread_id == 0 ||
        writer_thread_id == Platform::GetCurrentThreadID()) {
        return CrashFlushResult::Busy;
    }

    if (hub.test_pause_crash_flush_before_publish.load(
            std::memory_order_acquire
        )) {
        hub.test_crash_flush_before_publish_paused.store(
            true, std::memory_order_release
        );
        while (!hub.test_crash_flush_before_publish_resume.load(
            std::memory_order_acquire
        )) {
            std::this_thread::yield();
        }
        hub.test_crash_flush_before_publish_paused.store(
            false, std::memory_order_release
        );
    }

    const std::uint64_t ticket =
        hub.crash_flush_requested.fetch_add(1, std::memory_order_seq_cst) + 1;
    const std::uint64_t epoch_after =
        hub.crash_flush_epoch.load(std::memory_order_seq_cst);
    if (epoch_after != epoch_before ||
        hub.generation.load(std::memory_order_seq_cst) != generation) {
        return CrashFlushResult::NotRunning;
    }
    hub.work_cv.notify_one();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(_timeout_ms);
    for (;;) {
        if (hub.generation.load(std::memory_order_seq_cst) != generation) {
            return CrashFlushResult::NotRunning;
        }
        const std::uint64_t completed =
            hub.crash_flush_completed.load(std::memory_order_acquire);
        if (completed >= ticket) {
            if (hub.generation.load(std::memory_order_seq_cst) !=
                generation) {
                return CrashFlushResult::NotRunning;
            }
            const std::uint64_t failed =
                hub.crash_flush_failed.load(std::memory_order_acquire);
            return failed >= ticket ? CrashFlushResult::Faulted :
                                      CrashFlushResult::Completed;
        }

        if (hub.state.load(std::memory_order_acquire) == RuntimeState::Faulted) {
            return CrashFlushResult::Faulted;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            // One final acquire closes the completion-at-deadline race.
            if (hub.generation.load(std::memory_order_seq_cst) !=
                generation) {
                return CrashFlushResult::NotRunning;
            }
            const std::uint64_t final_completed =
                hub.crash_flush_completed.load(std::memory_order_acquire);
            if (final_completed >= ticket) {
                if (hub.generation.load(std::memory_order_seq_cst) !=
                    generation) {
                    return CrashFlushResult::NotRunning;
                }
                const std::uint64_t failed =
                    hub.crash_flush_failed.load(std::memory_order_acquire);
                return failed >= ticket ? CrashFlushResult::Faulted :
                                          CrashFlushResult::Completed;
            }
            return CrashFlushResult::TimedOut;
        }

        // A fatal publisher cannot synchronize through Hub::mutex. Repeating
        // the notification closes the condition-variable lost-wakeup window
        // while preserving the caller's hard deadline.
        hub.work_cv.notify_one();
        if (_timeout_ms <= 1) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
}

ShutdownResult Shutdown() noexcept {
    Hub&            hub = GetHub();
    std::lock_guard lifecycle_lock(hub.lifecycle_mutex);

    RuntimeState state = hub.state.load(std::memory_order_acquire);
    if (state == RuntimeState::Stopped) {
        ReapWorker(hub);
        return ShutdownResult::AlreadyStopped;
    }
    if (state == RuntimeState::Starting) {
        std::unique_lock lock(hub.mutex);
        hub.start_cv.wait(lock, [&] {
            return hub.start_complete;
        });
        state = hub.state.load(std::memory_order_acquire);
    }

    std::shared_ptr<FlushFence> fence;
    bool                        harvest_live_shards = false;
    {
        std::unique_lock lock(hub.mutex);
        state = hub.state.load(std::memory_order_acquire);
        if (state == RuntimeState::Faulted) {
            // FailWriter retains and closes the last admission. Do not allow a
            // new generation to start until every emitter admitted by that
            // generation has left its critical section.
            hub.emitter_cv.wait(lock, [&] {
                return !hub.retired_admission || hub.retired_admission->active_emitters == 0;
            });
            hub.retired_admission.reset();
            harvest_live_shards = true;
        } else if (state == RuntimeState::Stopped) {
            // Reap outside Hub::mutex below.
        } else if (state == RuntimeState::Running) {
            const auto closing_admission = hub.admission.load(std::memory_order_acquire);
            if (closing_admission) {
                closing_admission->closing = true;
                hub.retired_admission      = closing_admission;
            }
            hub.admission.store({}, std::memory_order_release);

            // Keep RuntimeState::Running while admitted emitters finish. Their
            // exit path publishes the current TLS shard before decrementing
            // active_emitters, so every such chunk is ordered before
            // Finalize in the writer FIFO.
            hub.emitter_cv.wait(lock, [&] {
                return !closing_admission || closing_admission->active_emitters == 0;
            });
            hub.retired_admission.reset();
            harvest_live_shards = true;
        } else if (state == RuntimeState::Draining) {
            fence = hub.shutdown_fence;
        }
    }

    if (state == RuntimeState::Stopped) {
        ReapWorker(hub);
        return ShutdownResult::AlreadyStopped;
    }

    if (harvest_live_shards) {
        // No admitted Emit remains. Keep Running while every parked live TLS
        // shard is published, then append Finalize while the registry frontier
        // is still pinned so no destructor can reorder an old chunk after it.
        std::lock_guard registry_lock(hub.tls_registry_mutex);
        (void)HarvestRegisteredTlsShardsLocked(hub);

        std::lock_guard hub_lock(hub.mutex);
        state = hub.state.load(std::memory_order_acquire);
        if (state == RuntimeState::Running) {
            hub.state.store(RuntimeState::Draining, std::memory_order_release);
            try {
                if (ShouldInjectLocked(hub.test_hooks, Testing::FaultPoint::ShutdownFinalizeAllocation)) {
                    throw std::bad_alloc{};
                }
                fence              = std::make_shared<FlushFence>();
                hub.shutdown_fence = fence;
                hub.messages.emplace_back(FinalizeMessage{.fence = fence});
            } catch (...) {
                hub.shutdown_fence.reset();
                fence.reset();
                hub.state.store(RuntimeState::Faulted, std::memory_order_release);
                hub.last_fault.store(RuntimeFault::WriterException, std::memory_order_release);
                hub.test_hooks.writer_resume = true;
            }
        } else if (state == RuntimeState::Draining) {
            fence = hub.shutdown_fence;
        }
    }

    if (hub.state.load(std::memory_order_acquire) == RuntimeState::Faulted) {
        hub.work_cv.notify_all();
        hub.test_cv.notify_all();
        ReapWorker(hub);
        hub.state.store(RuntimeState::Stopped, std::memory_order_release);
        return ShutdownResult::Faulted;
    }

    hub.work_cv.notify_one();
    hub.test_cv.notify_all();
    const FlushResult result = WaitFence(fence);
    ReapWorker(hub);
    if (result == FlushResult::Completed) {
        return ShutdownResult::Completed;
    }
    hub.state.store(RuntimeState::Stopped, std::memory_order_release);
    return ShutdownResult::Faulted;
}

RuntimeState GetRuntimeState() noexcept {
    return GetHub().state.load(std::memory_order_acquire);
}

std::uint64_t GetRuntimeGeneration() noexcept {
    return GetHub().generation.load(std::memory_order_acquire);
}

RuntimeStats GetRuntimeStats() noexcept {
    Hub&         hub = GetHub();
    RuntimeStats stats{};
    stats.state      = hub.state.load(std::memory_order_acquire);
    stats.last_fault = hub.last_fault.load(std::memory_order_acquire);
    stats.generation = hub.generation.load(std::memory_order_acquire);

#define MOER_LOAD_PROFILE_STAT(Name) stats.Name = hub.stats.Name.load(std::memory_order_relaxed)
    MOER_LOAD_PROFILE_STAT(records_committed);
    MOER_LOAD_PROFILE_STAT(records_enqueued);
    MOER_LOAD_PROFILE_STAT(records_written);
    MOER_LOAD_PROFILE_STAT(records_dropped_stopped);
    MOER_LOAD_PROFILE_STAT(records_dropped_stale_generation);
    MOER_LOAD_PROFILE_STAT(records_dropped_oversized);
    MOER_LOAD_PROFILE_STAT(records_dropped_queue_full);
    MOER_LOAD_PROFILE_STAT(records_dropped_after_fault);
    MOER_LOAD_PROFILE_STAT(chunks_enqueued);
    MOER_LOAD_PROFILE_STAT(chunks_written);
    MOER_LOAD_PROFILE_STAT(chunks_dropped);
    MOER_LOAD_PROFILE_STAT(resident_chunks);
    MOER_LOAD_PROFILE_STAT(resident_records);
    MOER_LOAD_PROFILE_STAT(resident_bytes);
    MOER_LOAD_PROFILE_STAT(high_water_chunks);
    MOER_LOAD_PROFILE_STAT(high_water_records);
    MOER_LOAD_PROFILE_STAT(high_water_bytes);
    MOER_LOAD_PROFILE_STAT(file_bytes_written);
    MOER_LOAD_PROFILE_STAT(flush_completed);
    MOER_LOAD_PROFILE_STAT(flush_failed);
    MOER_LOAD_PROFILE_STAT(io_faults);
#undef MOER_LOAD_PROFILE_STAT
    return stats;
}

namespace Testing {

bool ConfigureFault(FaultPoint _point, std::uint64_t _trigger_hit) noexcept {
    Hub&            hub = GetHub();
    std::lock_guard lock(hub.mutex);
    if (hub.state.load(std::memory_order_acquire) != RuntimeState::Stopped) {
        return false;
    }
    hub.test_hooks.fault_point   = _point;
    hub.test_hooks.trigger_hit   = std::max<std::uint64_t>(_trigger_hit, 1);
    hub.test_hooks.matching_hits = 0;
    hub.test_hooks.fault_fired   = false;
    return true;
}

bool ConfigureWriterPauseAfterStart(bool _enabled) noexcept {
    Hub&            hub = GetHub();
    std::lock_guard lock(hub.mutex);
    if (hub.state.load(std::memory_order_acquire) != RuntimeState::Stopped) {
        return false;
    }
    hub.test_hooks.pause_after_start = _enabled;
    hub.test_hooks.writer_paused     = false;
    hub.test_hooks.writer_resume     = false;
    return true;
}

bool ConfigureWriterPauseBeforeIdleWait(bool _enabled) noexcept {
    Hub&            hub = GetHub();
    std::lock_guard lock(hub.mutex);
    if (hub.state.load(std::memory_order_acquire) != RuntimeState::Stopped) {
        return false;
    }
    hub.test_hooks.pause_before_idle_wait     = _enabled;
    hub.test_hooks.writer_idle_wait_paused   = false;
    hub.test_hooks.writer_idle_wait_resume   = false;
    return true;
}

bool ConfigureCrashFlushPauseBeforePublish(bool _enabled) noexcept {
    Hub& hub = GetHub();
    if (hub.state.load(std::memory_order_acquire) != RuntimeState::Stopped) {
        return false;
    }
    hub.test_pause_crash_flush_before_publish.store(
        _enabled, std::memory_order_release
    );
    hub.test_crash_flush_before_publish_paused.store(
        false, std::memory_order_release
    );
    hub.test_crash_flush_before_publish_resume.store(
        false, std::memory_order_release
    );
    return true;
}

bool ConfigureWriterPauseBeforeTempOpen(bool _enabled) noexcept {
    Hub&            hub = GetHub();
    std::lock_guard lock(hub.mutex);
    if (hub.state.load(std::memory_order_acquire) != RuntimeState::Stopped) {
        return false;
    }
    hub.test_hooks.pause_before_temp_open = _enabled;
    hub.test_hooks.writer_paused          = false;
    hub.test_hooks.writer_resume          = false;
    return true;
}

bool ConfigureEmitterPauseAfterAdmission(bool _enabled) noexcept {
    Hub&            hub = GetHub();
    std::lock_guard lock(hub.mutex);
    if (hub.state.load(std::memory_order_acquire) != RuntimeState::Stopped) {
        return false;
    }
    hub.test_hooks.pause_emitter_after_admission = _enabled;
    hub.test_hooks.emitter_paused                = false;
    hub.test_hooks.emitter_resume                = false;
    return true;
}

bool WaitForWriterPaused(std::uint32_t _timeout_ms) noexcept {
    Hub&             hub = GetHub();
    std::unique_lock lock(hub.mutex);
    return hub.test_cv.wait_for(lock, std::chrono::milliseconds(_timeout_ms), [&] {
        return hub.test_hooks.writer_paused ||
               hub.state.load(std::memory_order_acquire) == RuntimeState::Faulted;
    }) && hub.test_hooks.writer_paused;
}

bool WaitForWriterIdleWaitPaused(std::uint32_t _timeout_ms) noexcept {
    Hub&             hub = GetHub();
    std::unique_lock lock(hub.mutex);
    return hub.test_cv.wait_for(
               lock, std::chrono::milliseconds(_timeout_ms), [&] {
                   return hub.test_hooks.writer_idle_wait_paused ||
                          hub.state.load(std::memory_order_acquire) ==
                              RuntimeState::Faulted;
               }
           ) &&
           hub.test_hooks.writer_idle_wait_paused;
}

bool WaitForCrashFlushBeforePublishPaused(
    std::uint32_t _timeout_ms
) noexcept {
    Hub&       hub      = GetHub();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (hub.test_crash_flush_before_publish_paused.load(
                std::memory_order_acquire
            )) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return hub.test_crash_flush_before_publish_paused.load(
        std::memory_order_acquire
    );
}

bool WaitForCrashFlushRequestPending(std::uint32_t _timeout_ms) noexcept {
    Hub&       hub      = GetHub();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (hub.crash_flush_requested.load(std::memory_order_acquire) >
            hub.crash_flush_completed.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return hub.crash_flush_requested.load(std::memory_order_acquire) >
           hub.crash_flush_completed.load(std::memory_order_acquire);
}

bool WaitForEmitterPaused(std::uint32_t _timeout_ms) noexcept {
    Hub&             hub = GetHub();
    std::unique_lock lock(hub.mutex);
    return hub.test_cv.wait_for(lock, std::chrono::milliseconds(_timeout_ms), [&] {
        return hub.test_hooks.emitter_paused ||
               hub.state.load(std::memory_order_acquire) == RuntimeState::Faulted;
    }) && hub.test_hooks.emitter_paused;
}

bool CreateActiveTempCollision(const std::uint8_t* _bytes, std::size_t _byte_count) noexcept {
    Hub&            hub = GetHub();
    std::lock_guard lock(hub.mutex);
    if (hub.state.load(std::memory_order_acquire) != RuntimeState::Starting ||
        !hub.test_hooks.pause_before_temp_open || !hub.test_hooks.writer_paused || hub.temp_path.empty() ||
        (!_bytes && _byte_count != 0)) {
        return false;
    }

    std::FILE* stream = OpenTempFileExclusive(hub.temp_path);
    if (!stream) {
        return false;
    }
    const bool wrote = _byte_count == 0 || std::fwrite(_bytes, 1, _byte_count, stream) == _byte_count;
    return std::fclose(stream) == 0 && wrote;
}

void ResumeWriter() noexcept {
    Hub& hub = GetHub();
    {
        std::lock_guard lock(hub.mutex);
        hub.test_hooks.writer_resume = true;
    }
    hub.test_cv.notify_all();
}

void ResumeWriterIdleWait() noexcept {
    Hub& hub = GetHub();
    {
        std::lock_guard lock(hub.mutex);
        hub.test_hooks.writer_idle_wait_resume = true;
    }
    hub.test_cv.notify_all();
}

void ResumeCrashFlushBeforePublish() noexcept {
    Hub& hub = GetHub();
    hub.test_crash_flush_before_publish_resume.store(
        true, std::memory_order_release
    );
}

void ResumeEmitter() noexcept {
    Hub& hub = GetHub();
    {
        std::lock_guard lock(hub.mutex);
        hub.test_hooks.emitter_resume = true;
    }
    hub.test_cv.notify_all();
}

void ClearHooks() noexcept {
    Hub&            hub = GetHub();
    std::lock_guard lock(hub.mutex);
    if (hub.state.load(std::memory_order_acquire) != RuntimeState::Stopped) {
        return;
    }
    hub.test_hooks = {};
    hub.test_pause_crash_flush_before_publish.store(
        false, std::memory_order_release
    );
    hub.test_crash_flush_before_publish_paused.store(
        false, std::memory_order_release
    );
    hub.test_crash_flush_before_publish_resume.store(
        false, std::memory_order_release
    );
}

} // namespace Testing

} // namespace Moer::ProfileDump
