#include "rhi/RHIGpuScope.h"

#include <algorithm>
#include <cassert>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#if defined(MOER_RHI_GPU_SCOPE_TEST_HOOKS)
#include <thread>
#endif
#include <tuple>
#include <utility>

namespace Moer::Render {
namespace GpuScopeDetail {

namespace {

constexpr std::size_t kInvalidStreamSlot =
    std::numeric_limits<std::size_t>::max();

#if defined(MOER_RHI_GPU_SCOPE_TEST_HOOKS)
std::atomic<std::atomic_uint32_t*> g_admission_pause_entered{nullptr};
std::atomic<std::atomic_bool*>     g_admission_pause_release{nullptr};

void PauseScopeAdmissionForTesting() noexcept {
    std::atomic_uint32_t* entered =
        g_admission_pause_entered.load(std::memory_order_acquire);
    if (entered == nullptr) {
        return;
    }
    entered->fetch_add(1, std::memory_order_acq_rel);
    std::atomic_bool* release =
        g_admission_pause_release.load(std::memory_order_acquire);
    while (release != nullptr &&
           !release->load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}
#endif

bool IsManagedQueue(EQueueType _queue) noexcept {
    return _queue == EQueueType::Graphics ||
           _queue == EQueueType::Compute ||
           _queue == EQueueType::Copy;
}

std::size_t QueueIndex(EQueueType _queue) noexcept {
    switch (_queue) {
        case EQueueType::Graphics:
            return 0;
        case EQueueType::Compute:
            return 1;
        case EQueueType::Copy:
            return 2;
        case EQueueType::Num:
        case EQueueType::Ignore:
        default:
            return 0;
    }
}

bool SameTimestampDomain(
    const RHIQueueBinding& _lhs,
    const RHIQueueBinding& _rhs
) noexcept {
    return _lhs.available && _rhs.available &&
           _lhs.native_queue_id == _rhs.native_queue_id &&
           _lhs.family_id == _rhs.family_id;
}

std::uint64_t TimestampMask(std::uint32_t _valid_bits) noexcept {
    return _valid_bits == 64 ?
        std::numeric_limits<std::uint64_t>::max() :
        (std::uint64_t{1} << _valid_bits) - 1;
}

std::uint64_t TimestampDelta(
    std::uint64_t _begin_tick,
    std::uint64_t _end_tick,
    std::uint32_t _valid_bits
) noexcept {
    return (_end_tick - _begin_tick) & TimestampMask(_valid_bits);
}

bool NearlyEqualDuration(double _lhs, double _rhs) noexcept {
    if (!std::isfinite(_lhs) || !std::isfinite(_rhs)) {
        return false;
    }
    const double tolerance = std::max(
        1.0e-6,
        std::max(std::abs(_lhs), std::abs(_rhs)) * 1.0e-9
    );
    return std::abs(_lhs - _rhs) <= tolerance;
}

void UpdateHighWater(
    std::atomic<std::uint64_t>& _high_water,
    std::uint64_t               _value
) noexcept {
    std::uint64_t observed = _high_water.load(std::memory_order_relaxed);
    while (observed < _value &&
           !_high_water.compare_exchange_weak(
               observed,
               _value,
               std::memory_order_relaxed,
               std::memory_order_relaxed
           )) {
    }
}

void AssignBoundedNoexcept(
    std::string&      _destination,
    std::string_view  _source,
    std::size_t       _max_bytes
) noexcept {
    try {
        _destination.assign(
            _source.data(),
            std::min(_source.size(), _max_bytes)
        );
    } catch (...) {
        _destination.clear();
    }
}

} // namespace

struct ScopeSlot {
    std::uint64_t scope_id{0};
    std::uint64_t parent_scope_id{0};
    std::uint64_t source_order{0};
    std::uint64_t local_order{0};
    std::uint32_t depth{0};
    RHIQueueBinding queue_binding{};
    std::string     name{};

    bool                   terminal{false};
    GpuScopeTerminalStatus status{GpuScopeTerminalStatus::Error};
    std::uint64_t          query_id{0};
    std::uint64_t          begin_tick{0};
    std::uint64_t          end_tick{0};
    std::uint32_t          valid_bits{0};
    double                 tick_period_ns{0.0};
    double                 duration_ns{0.0};
    std::string            error_reason{};
};

struct SourceKey {
    RHIQueueBinding queue_binding{};
    std::uint64_t   source_order{0};
};

struct StreamCounters {
    std::atomic<std::uint64_t> frames_opened{0};
    std::atomic<std::uint64_t> frames_sealed{0};
    std::atomic<std::uint64_t> frames_ready{0};
    std::atomic<std::uint64_t> frames_popped{0};
    std::atomic<std::uint64_t> frames_dropped_resident_full{0};
    std::atomic<std::uint64_t> frames_dropped_pending_full{0};
    std::atomic<std::uint64_t> frames_dropped_duplicate_id{0};
    std::atomic<std::uint64_t> frames_dropped_resource_exhaustion{0};
    std::atomic<std::uint64_t> frames_abandoned_on_close{0};

    std::atomic<std::uint64_t> sources_opened{0};
    std::atomic<std::uint64_t> sources_dropped_capacity{0};
    std::atomic<std::uint64_t> sources_dropped_duplicate_order{0};
    std::atomic<std::uint64_t> sources_dropped_after_seal{0};

    std::atomic<std::uint64_t> scopes_attempted{0};
    std::atomic<std::uint64_t> scopes_admitted{0};
    std::atomic<std::uint64_t> scopes_ready{0};
    std::atomic<std::uint64_t> scopes_error{0};
    std::atomic<std::uint64_t> scopes_dropped_resident_full{0};
    std::atomic<std::uint64_t> scopes_dropped_frame_full{0};
    std::atomic<std::uint64_t> scopes_dropped_name_too_large{0};
    std::atomic<std::uint64_t> scopes_dropped_invalid_hierarchy{0};
    std::atomic<std::uint64_t> scopes_dropped_suppressed_subtree{0};
    std::atomic<std::uint64_t> scopes_dropped_after_seal{0};
    std::atomic<std::uint64_t> scopes_dropped_resource_exhaustion{0};

    std::atomic<std::uint64_t> high_water_frames{0};
    std::atomic<std::uint64_t> high_water_pending_frames{0};
    std::atomic<std::uint64_t> high_water_ready_frames{0};
    std::atomic<std::uint64_t> high_water_scopes{0};
};

struct FrameState;

struct StreamState : std::enable_shared_from_this<StreamState> {
    explicit StreamState(const GpuScopeStreamConfig& _config) :
        config(_config),
        frame_slots(_config.max_resident_frames),
        frame_reservations(_config.max_resident_frames, false),
        reserved_frame_ids(_config.max_resident_frames, 0),
        ready_flags(_config.max_resident_frames, false),
        frame_order(_config.max_resident_frames, 0) {}

    bool TryReserveScope(
        const std::shared_ptr<FrameState>& _frame
    ) noexcept;
    bool ContainsFrame(
        const std::shared_ptr<FrameState>& _frame
    ) const noexcept;
    void ReleaseScopeReservation() noexcept;
    void MarkFrameReady(
        const std::shared_ptr<FrameState>& _frame
    ) noexcept;
    void CancelFrameReservation(
        std::size_t   _slot,
        std::uint64_t _frame_id
    ) noexcept;

    GpuScopeStreamConfig config{};
    mutable std::mutex   mutex{};
    bool                 accepting{true};
    bool                 pop_in_progress{false};
    std::size_t          pop_slot{kInvalidStreamSlot};

    Array<std::shared_ptr<FrameState>> frame_slots{};
    Array<bool>                        frame_reservations{};
    Array<std::uint64_t>               reserved_frame_ids{};
    Array<bool>                        ready_flags{};
    Array<std::size_t>                 frame_order{};
    std::size_t                        order_head{0};
    std::size_t                        order_tail{0};
    std::size_t                        order_count{0};
    std::size_t                        pending_count{0};
    std::size_t                        ready_count{0};
    std::size_t                        resident_scope_count{0};
    StreamCounters                     counters{};
};

struct FrameState : std::enable_shared_from_this<FrameState> {
    FrameState(
        std::weak_ptr<StreamState> _stream,
        std::uint64_t              _frame_id,
        const GpuScopeStreamConfig& _config
    ) :
        stream(std::move(_stream)),
        frame_id(_frame_id),
        max_scopes(_config.max_scopes_per_frame),
        max_sources(_config.max_sources_per_frame),
        max_scope_depth(_config.max_scope_depth),
        max_scope_name_bytes(_config.max_scope_name_bytes),
        max_error_reason_bytes(_config.max_error_reason_bytes) {
        scopes.reserve(max_scopes);
        sources.reserve(max_sources);
    }

    GpuScopeCompletionTicket TryBeginScope(
        const std::shared_ptr<RecorderState>& _recorder,
        std::string_view                      _name,
        std::uint64_t                         _parent_scope_id,
        std::uint32_t                         _depth
    ) noexcept;
    void ResolveScope(
        std::uint32_t       _slot,
        std::uint64_t       _scope_id,
        const QueryResult*  _result,
        std::string_view    _forced_error
    ) noexcept;
    bool Seal() noexcept;
    ResolvedGpuScopeFrame Materialize() const;
    std::size_t AdmittedScopeCount() const noexcept;

    [[nodiscard]] bool TryStartAdmission() noexcept {
        std::scoped_lock lock(mutex);
        if (sealed) {
            return false;
        }
        ++in_flight_admission_count;
        return true;
    }

    void FinishAdmission() noexcept;

    void RecordDroppedScopeDuringAdmission() noexcept {
        std::scoped_lock lock(mutex);
        assert(in_flight_admission_count != 0);
        ++dropped_scope_count;
    }

    std::weak_ptr<StreamState> stream{};
    std::size_t                stream_slot{kInvalidStreamSlot};
    const std::uint64_t        frame_id{0};
    const std::size_t          max_scopes{0};
    const std::size_t          max_sources{0};
    const std::uint32_t        max_scope_depth{0};
    const std::size_t          max_scope_name_bytes{0};
    const std::size_t          max_error_reason_bytes{0};

    mutable std::mutex mutex{};
    Array<ScopeSlot>   scopes{};
    Array<SourceKey>   sources{};
    std::uint64_t      next_scope_id{1};
    std::uint64_t      terminal_scope_count{0};
    std::uint64_t      error_scope_count{0};
    std::uint64_t      dropped_scope_count{0};
    std::uint64_t      in_flight_admission_count{0};
    bool               sealed{false};
    bool               ready_notified{false};
};

struct RecorderState {
    RecorderState(
        std::shared_ptr<FrameState> _frame,
        RHIQueueBinding             _queue_binding,
        std::uint64_t               _source_order
    ) :
        frame(std::move(_frame)),
        queue_binding(_queue_binding),
        source_order(_source_order) {}

    std::shared_ptr<FrameState> frame{};
    RHIQueueBinding             queue_binding{};
    std::uint64_t               source_order{0};
    std::atomic<std::uint64_t>  next_local_order{0};
};

bool StreamState::TryReserveScope(
    const std::shared_ptr<FrameState>& _frame
) noexcept {
    std::scoped_lock lock(mutex);
    if (!accepting || !_frame ||
        _frame->stream_slot >= frame_slots.size() ||
        frame_slots[_frame->stream_slot].get() != _frame.get() ||
        resident_scope_count >= config.max_resident_scopes) {
        counters.scopes_dropped_resident_full.fetch_add(
            1, std::memory_order_relaxed
        );
        return false;
    }
    ++resident_scope_count;
    UpdateHighWater(
        counters.high_water_scopes,
        static_cast<std::uint64_t>(resident_scope_count)
    );
    return true;
}

bool StreamState::ContainsFrame(
    const std::shared_ptr<FrameState>& _frame
) const noexcept {
    std::scoped_lock lock(mutex);
    return accepting && _frame &&
           _frame->stream_slot < frame_slots.size() &&
           frame_slots[_frame->stream_slot].get() == _frame.get();
}

void StreamState::ReleaseScopeReservation() noexcept {
    std::scoped_lock lock(mutex);
    if (resident_scope_count != 0) {
        --resident_scope_count;
    }
}

void StreamState::MarkFrameReady(
    const std::shared_ptr<FrameState>& _frame
) noexcept {
    std::scoped_lock lock(mutex);
    if (!accepting || !_frame ||
        _frame->stream_slot >= frame_slots.size() ||
        frame_slots[_frame->stream_slot].get() != _frame.get() ||
        ready_flags[_frame->stream_slot]) {
        return;
    }

    ready_flags[_frame->stream_slot] = true;
    if (pending_count != 0) {
        --pending_count;
    }
    ++ready_count;
    counters.frames_ready.fetch_add(1, std::memory_order_relaxed);
    UpdateHighWater(
        counters.high_water_ready_frames,
        static_cast<std::uint64_t>(ready_count)
    );
}

void StreamState::CancelFrameReservation(
    std::size_t   _slot,
    std::uint64_t _frame_id
) noexcept {
    std::scoped_lock lock(mutex);
    if (_slot >= frame_reservations.size() ||
        !frame_reservations[_slot] ||
        reserved_frame_ids[_slot] != _frame_id) {
        return;
    }

    std::size_t order_offset = order_count;
    for (std::size_t offset = 0; offset < order_count; ++offset) {
        const std::size_t order_index =
            (order_head + offset) % frame_order.size();
        if (frame_order[order_index] == _slot) {
            order_offset = offset;
            break;
        }
    }
    if (order_offset == order_count) {
        return;
    }

    for (std::size_t offset = order_offset;
         offset + 1 < order_count;
         ++offset) {
        const std::size_t destination =
            (order_head + offset) % frame_order.size();
        const std::size_t source =
            (order_head + offset + 1) % frame_order.size();
        frame_order[destination] = frame_order[source];
    }

    frame_reservations[_slot] = false;
    reserved_frame_ids[_slot] = 0;
    ready_flags[_slot] = false;
    --order_count;
    order_tail = (order_head + order_count) % frame_order.size();
    if (pending_count != 0) {
        --pending_count;
    }
}

GpuScopeCompletionTicket FrameState::TryBeginScope(
    const std::shared_ptr<RecorderState>& _recorder,
    std::string_view                      _name,
    std::uint64_t                         _parent_scope_id,
    std::uint32_t                         _depth
) noexcept {
    const std::shared_ptr<StreamState> owner = stream.lock();
    if (!owner || !_recorder || _recorder->frame.get() != this) {
        return {};
    }

    owner->counters.scopes_attempted.fetch_add(
        1, std::memory_order_relaxed
    );
    if (!TryStartAdmission()) {
        owner->counters.scopes_dropped_after_seal.fetch_add(
            1, std::memory_order_relaxed
        );
        return {};
    }
    struct AdmissionGuard {
        FrameState* frame{nullptr};
        ~AdmissionGuard() {
            frame->FinishAdmission();
        }
    } admission_guard{this};

    if (_name.size() > max_scope_name_bytes) {
        owner->counters.scopes_dropped_name_too_large.fetch_add(
            1, std::memory_order_relaxed
        );
        RecordDroppedScopeDuringAdmission();
        return {};
    }
    if (_depth > max_scope_depth) {
        owner->counters.scopes_dropped_invalid_hierarchy.fetch_add(
            1, std::memory_order_relaxed
        );
        RecordDroppedScopeDuringAdmission();
        return {};
    }
    if (!owner->TryReserveScope(shared_from_this())) {
        RecordDroppedScopeDuringAdmission();
        return {};
    }

#if defined(MOER_RHI_GPU_SCOPE_TEST_HOOKS)
    PauseScopeAdmissionForTesting();
#endif

    ScopeSlot candidate{};
    try {
        candidate.name.assign(_name);
    } catch (...) {
        owner->ReleaseScopeReservation();
        owner->counters.scopes_dropped_resource_exhaustion.fetch_add(
            1, std::memory_order_relaxed
        );
        RecordDroppedScopeDuringAdmission();
        return {};
    }

    bool             release_reservation = false;
    std::uint32_t    slot_index = 0;
    std::uint64_t    scope_id = 0;
    {
        std::scoped_lock lock(mutex);
        // TryStartAdmission linearizes before Seal. Once admitted here, Seal
        // may already have set sealed=true, but readiness must wait for this
        // in-flight producer and the scope remains part of the frozen frame.
        if (scopes.size() >= max_scopes) {
            ++dropped_scope_count;
            release_reservation = true;
            owner->counters.scopes_dropped_frame_full.fetch_add(
                1, std::memory_order_relaxed
            );
        } else {
            const ScopeSlot* parent = nullptr;
            if (_parent_scope_id != 0) {
                const auto parent_it = std::find_if(
                    scopes.begin(),
                    scopes.end(),
                    [_parent_scope_id](const ScopeSlot& _slot) {
                        return _slot.scope_id == _parent_scope_id;
                    }
                );
                if (parent_it != scopes.end()) {
                    parent = &*parent_it;
                }
            }

            const bool root_shape =
                _parent_scope_id == 0 && _depth == 0;
            const bool child_shape =
                parent != nullptr &&
                parent->queue_binding == _recorder->queue_binding &&
                parent->source_order == _recorder->source_order &&
                _depth == parent->depth + 1;
            if (!root_shape && !child_shape) {
                ++dropped_scope_count;
                release_reservation = true;
                owner->counters.scopes_dropped_invalid_hierarchy.fetch_add(
                    1, std::memory_order_relaxed
                );
            } else {
                scope_id = next_scope_id++;
                if (scope_id == 0) {
                    scope_id = next_scope_id++;
                }
                candidate.scope_id = scope_id;
                candidate.parent_scope_id = _parent_scope_id;
                candidate.source_order = _recorder->source_order;
                candidate.local_order =
                    _recorder->next_local_order.fetch_add(
                        1, std::memory_order_relaxed
                    );
                candidate.depth = _depth;
                candidate.queue_binding = _recorder->queue_binding;
                slot_index = static_cast<std::uint32_t>(scopes.size());
                try {
                    scopes.emplace_back(std::move(candidate));
                } catch (...) {
                    ++dropped_scope_count;
                    release_reservation = true;
                    scope_id = 0;
                    owner->counters.
                        scopes_dropped_resource_exhaustion.fetch_add(
                            1, std::memory_order_relaxed
                        );
                }
            }
        }
    }

    if (release_reservation) {
        owner->ReleaseScopeReservation();
        return {};
    }
    owner->counters.scopes_admitted.fetch_add(
        1, std::memory_order_relaxed
    );
    return GpuScopeCompletionTicket(
        shared_from_this(), slot_index, scope_id
    );
}

void FrameState::FinishAdmission() noexcept {
    bool ready_frame = false;
    {
        std::scoped_lock lock(mutex);
        assert(in_flight_admission_count != 0);
        if (in_flight_admission_count != 0) {
            --in_flight_admission_count;
        }
        if (sealed && in_flight_admission_count == 0 &&
            terminal_scope_count == scopes.size() &&
            !ready_notified) {
            ready_notified = true;
            ready_frame = true;
        }
    }

    if (ready_frame) {
        if (const std::shared_ptr<StreamState> owner = stream.lock()) {
            owner->MarkFrameReady(shared_from_this());
        }
    }
}

void FrameState::ResolveScope(
    std::uint32_t      _slot,
    std::uint64_t      _scope_id,
    const QueryResult* _result,
    std::string_view   _forced_error
) noexcept {
    bool ready_frame = false;
    bool ready_scope = false;
    {
        std::scoped_lock lock(mutex);
        if (_slot >= scopes.size()) {
            return;
        }
        ScopeSlot& slot = scopes[_slot];
        if (slot.scope_id != _scope_id || slot.terminal) {
            return;
        }

        slot.terminal = true;
        ++terminal_scope_count;
        if (_result != nullptr) {
            slot.query_id = _result->query_id;
        }

        const auto set_error = [&](std::string_view _reason) {
            slot.status = GpuScopeTerminalStatus::Error;
            AssignBoundedNoexcept(
                slot.error_reason,
                _reason,
                max_error_reason_bytes
            );
            ++error_scope_count;
        };

        if (!_forced_error.empty()) {
            set_error(_forced_error);
        } else if (_result == nullptr ||
                   _result->status != QueryStatus::Ready ||
                   _result->kind != QueryKind::Timestamp) {
            const std::string_view reason =
                _result != nullptr && !_result->error_reason.empty() ?
                    std::string_view(_result->error_reason) :
                    std::string_view("GPU scope query did not resolve Ready");
            set_error(reason);
        } else {
            const auto* timestamp =
                std::get_if<TimestampQueryResult>(&_result->payload);
            bool payload_valid =
                timestamp != nullptr &&
                timestamp->valid_bits != 0 &&
                timestamp->valid_bits <= 64 &&
                std::isfinite(timestamp->tick_period_ns) &&
                timestamp->tick_period_ns > 0.0 &&
                std::isfinite(timestamp->duration_ns) &&
                timestamp->duration_ns >= 0.0;
            if (payload_valid) {
                const std::uint64_t delta_tick = TimestampDelta(
                    timestamp->begin_tick,
                    timestamp->end_tick,
                    timestamp->valid_bits
                );
                const double expected_duration_ns =
                    static_cast<double>(delta_tick) *
                    timestamp->tick_period_ns;
                payload_valid = NearlyEqualDuration(
                    timestamp->duration_ns,
                    expected_duration_ns
                );
            }
            if (!payload_valid) {
                set_error("GPU scope timestamp payload is invalid");
            } else {
                slot.status = GpuScopeTerminalStatus::Ready;
                slot.query_id = _result->query_id;
                slot.begin_tick = timestamp->begin_tick;
                slot.end_tick = timestamp->end_tick;
                slot.valid_bits = timestamp->valid_bits;
                slot.tick_period_ns = timestamp->tick_period_ns;
                slot.duration_ns = timestamp->duration_ns;
                slot.error_reason.clear();
                ready_scope = true;
            }
        }

        if (sealed && in_flight_admission_count == 0 &&
            terminal_scope_count == scopes.size() && !ready_notified) {
            ready_notified = true;
            ready_frame = true;
        }
    }

    if (const std::shared_ptr<StreamState> owner = stream.lock()) {
        if (ready_scope) {
            owner->counters.scopes_ready.fetch_add(
                1, std::memory_order_relaxed
            );
        } else {
            owner->counters.scopes_error.fetch_add(
                1, std::memory_order_relaxed
            );
        }
        if (ready_frame) {
            owner->MarkFrameReady(shared_from_this());
        }
    }
}

bool FrameState::Seal() noexcept {
    bool ready_frame = false;
    {
        std::scoped_lock lock(mutex);
        if (sealed) {
            return false;
        }
        sealed = true;
        if (in_flight_admission_count == 0 &&
            terminal_scope_count == scopes.size() && !ready_notified) {
            ready_notified = true;
            ready_frame = true;
        }
    }

    if (const std::shared_ptr<StreamState> owner = stream.lock()) {
        owner->counters.frames_sealed.fetch_add(
            1, std::memory_order_relaxed
        );
        if (ready_frame) {
            owner->MarkFrameReady(shared_from_this());
        }
    }
    return true;
}

std::size_t FrameState::AdmittedScopeCount() const noexcept {
    std::scoped_lock lock(mutex);
    return scopes.size();
}

ResolvedGpuScopeFrame FrameState::Materialize() const {
    Array<ScopeSlot> slots{};
    std::uint64_t dropped = 0;
    std::uint64_t errors = 0;
    {
        std::scoped_lock lock(mutex);
        slots = scopes;
        dropped = dropped_scope_count;
        errors = error_scope_count;
    }

    ResolvedGpuScopeFrame result{};
    result.frame_id = frame_id;
    result.admitted_scope_count =
        static_cast<std::uint64_t>(slots.size());
    result.dropped_scope_count = dropped;
    result.error_scope_count = errors;

    Array<std::size_t> order(slots.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(
        order.begin(),
        order.end(),
        [&](std::size_t _lhs, std::size_t _rhs) {
            const ScopeSlot& lhs = slots[_lhs];
            const ScopeSlot& rhs = slots[_rhs];
            return std::tie(
                       lhs.queue_binding.queue,
                       lhs.queue_binding.native_queue_id,
                       lhs.queue_binding.family_id,
                       lhs.source_order,
                       lhs.local_order,
                       lhs.scope_id
                   ) <
                   std::tie(
                       rhs.queue_binding.queue,
                       rhs.queue_binding.native_queue_id,
                       rhs.queue_binding.family_id,
                       rhs.source_order,
                       rhs.local_order,
                       rhs.scope_id
                   );
        }
    );

    struct BuildNode {
        GpuScopeNode      node{};
        Array<std::size_t> children{};
    };

    Array<BuildNode> arena{};
    arena.reserve(slots.size());
    UnorderedMap<std::uint64_t, std::size_t> scope_to_arena{};
    scope_to_arena.reserve(slots.size());
    std::array<Array<std::size_t>, 3> root_indices{};
    bool structure_valid = true;

    for (std::size_t source_index : order) {
        const ScopeSlot& slot = slots[source_index];
        if (!slot.terminal || !IsManagedQueue(slot.queue_binding.queue)) {
            structure_valid = false;
            continue;
        }

        BuildNode build{};
        build.node.scope_id = slot.scope_id;
        build.node.parent_scope_id = slot.parent_scope_id;
        build.node.source_order = slot.source_order;
        build.node.local_order = slot.local_order;
        build.node.depth = slot.depth;
        build.node.queue_binding = slot.queue_binding;
        build.node.name = slot.name;
        build.node.status = slot.status;
        build.node.query_id = slot.query_id;
        build.node.begin_tick = slot.begin_tick;
        build.node.end_tick = slot.end_tick;
        build.node.valid_bits = slot.valid_bits;
        build.node.tick_period_ns = slot.tick_period_ns;
        build.node.total_duration_ns = slot.duration_ns;
        build.node.error_reason = slot.error_reason;

        const std::size_t arena_index = arena.size();
        arena.emplace_back(std::move(build));
        const bool inserted =
            scope_to_arena.emplace(slot.scope_id, arena_index).second;
        if (!inserted) {
            structure_valid = false;
            continue;
        }

        if (slot.parent_scope_id == 0) {
            if (slot.depth != 0) {
                structure_valid = false;
                continue;
            }
            root_indices[QueueIndex(slot.queue_binding.queue)].
                emplace_back(arena_index);
            continue;
        }

        const auto parent_it =
            scope_to_arena.find(slot.parent_scope_id);
        if (parent_it == scope_to_arena.end()) {
            structure_valid = false;
            continue;
        }
        BuildNode& parent = arena[parent_it->second];
        if (slot.depth != parent.node.depth + 1 ||
            slot.source_order != parent.node.source_order ||
            slot.queue_binding != parent.node.queue_binding) {
            structure_valid = false;
            continue;
        }
        parent.children.emplace_back(arena_index);
    }

    // A Ready query payload is not sufficient on its own: direct children
    // recorded by one source must lie inside the parent interval and siblings
    // must not overlap. Fail the whole frame closed if backend data violates
    // that topology instead of silently clamping an impossible exclusive
    // duration to zero.
    for (const BuildNode& parent : arena) {
        if (parent.node.status != GpuScopeTerminalStatus::Ready) {
            continue;
        }
        if (parent.node.valid_bits == 0 ||
            parent.node.valid_bits > 64) {
            structure_valid = false;
            continue;
        }

        const std::uint64_t parent_delta = TimestampDelta(
            parent.node.begin_tick,
            parent.node.end_tick,
            parent.node.valid_bits
        );
        std::uint64_t previous_child_end = 0;
        bool          has_previous_child = false;
        for (std::size_t child_index : parent.children) {
            const GpuScopeNode& child = arena[child_index].node;
            if (child.status != GpuScopeTerminalStatus::Ready ||
                !SameTimestampDomain(
                    parent.node.queue_binding,
                    child.queue_binding
                ) ||
                child.valid_bits != parent.node.valid_bits ||
                !NearlyEqualDuration(
                    child.tick_period_ns,
                    parent.node.tick_period_ns
                )) {
                structure_valid = false;
                continue;
            }

            const std::uint64_t child_begin_offset =
                (child.begin_tick - parent.node.begin_tick) &
                TimestampMask(parent.node.valid_bits);
            const std::uint64_t child_delta = TimestampDelta(
                child.begin_tick,
                child.end_tick,
                child.valid_bits
            );
            const bool contained =
                child_begin_offset <= parent_delta &&
                child_delta <= parent_delta - child_begin_offset;
            const bool ordered =
                !has_previous_child ||
                child_begin_offset >= previous_child_end;
            if (!contained || !ordered) {
                structure_valid = false;
                continue;
            }
            previous_child_end = child_begin_offset + child_delta;
            has_previous_child = true;
        }
    }

    // Root scopes from different recording sources may overlap, but roots
    // emitted by one source are a serial command stream just like siblings
    // under a parent. Validate each contiguous (queue binding, source order)
    // group in local-order order. The half-range rule disambiguates forward
    // timestamp wrap from an impossible backward/overlapping interval.
    for (const Array<std::size_t>& roots : root_indices) {
        const GpuScopeNode* previous_root = nullptr;
        for (std::size_t root_index : roots) {
            const GpuScopeNode& root = arena[root_index].node;
            const bool same_source =
                previous_root != nullptr &&
                root.queue_binding == previous_root->queue_binding &&
                root.source_order == previous_root->source_order;
            if (!same_source) {
                previous_root = &root;
                continue;
            }

            if (root.status != GpuScopeTerminalStatus::Ready ||
                previous_root->status !=
                    GpuScopeTerminalStatus::Ready ||
                root.valid_bits == 0 ||
                root.valid_bits != previous_root->valid_bits ||
                !NearlyEqualDuration(
                    root.tick_period_ns,
                    previous_root->tick_period_ns
                )) {
                structure_valid = false;
                previous_root = &root;
                continue;
            }

            const std::uint64_t begin_distance =
                (root.begin_tick - previous_root->begin_tick) &
                TimestampMask(root.valid_bits);
            const std::uint64_t previous_duration = TimestampDelta(
                previous_root->begin_tick,
                previous_root->end_tick,
                previous_root->valid_bits
            );
            const std::uint64_t half_range =
                root.valid_bits == 64 ?
                    (std::uint64_t{1} << 63) :
                    (std::uint64_t{1} <<
                     (root.valid_bits - 1));
            if (begin_distance < previous_duration ||
                begin_distance >= half_range) {
                structure_valid = false;
            }
            previous_root = &root;
        }
    }

    const bool exclusive_trustworthy =
        structure_valid && dropped == 0 && errors == 0;
    const auto materialize =
        [&](const auto& self, std::size_t _node_index) -> GpuScopeNode {
        GpuScopeNode node = arena[_node_index].node;
        node.children.reserve(arena[_node_index].children.size());
        double child_duration = 0.0;
        for (std::size_t child_index :
             arena[_node_index].children) {
            GpuScopeNode child = self(self, child_index);
            if (exclusive_trustworthy &&
                child.status == GpuScopeTerminalStatus::Ready &&
                SameTimestampDomain(
                    node.queue_binding, child.queue_binding
                )) {
                child_duration += child.total_duration_ns;
            }
            node.children.emplace_back(std::move(child));
        }
        node.exclusive_duration_ns =
            exclusive_trustworthy &&
                    node.status == GpuScopeTerminalStatus::Ready ?
                std::max(0.0, node.total_duration_ns - child_duration) :
                0.0;
        return node;
    };

    for (std::size_t queue_index = 0;
         queue_index < root_indices.size();
         ++queue_index) {
        Array<GpuScopeNode>& roots = result.queue_roots[queue_index];
        roots.reserve(root_indices[queue_index].size());
        for (std::size_t root_index : root_indices[queue_index]) {
            roots.emplace_back(materialize(materialize, root_index));
        }
    }

    result.valid =
        structure_valid && dropped == 0 && errors == 0;
    return result;
}

} // namespace GpuScopeDetail

GpuScopeCompletionTicket::GpuScopeCompletionTicket(
    std::shared_ptr<GpuScopeDetail::FrameState> _frame,
    std::uint32_t                               _slot,
    std::uint64_t                               _scope_id
) noexcept :
    frame_(std::move(_frame)),
    slot_(_slot),
    scope_id_(_scope_id) {}

bool GpuScopeCompletionTicket::Valid() const noexcept {
    return frame_ != nullptr && scope_id_ != 0;
}

std::uint64_t GpuScopeCompletionTicket::ScopeId() const noexcept {
    return scope_id_;
}

void GpuScopeCompletionTicket::Resolve(
    const QueryResult& _result
) const noexcept {
    if (frame_) {
        frame_->ResolveScope(slot_, scope_id_, &_result, {});
    }
}

void GpuScopeCompletionTicket::Fail(
    std::string_view _reason
) const noexcept {
    if (frame_) {
        frame_->ResolveScope(
            slot_,
            scope_id_,
            nullptr,
            _reason.empty() ?
                std::string_view("GPU scope recording failed") :
                _reason
        );
    }
}

GpuScopeRecorder::GpuScopeRecorder(
    std::shared_ptr<GpuScopeDetail::RecorderState> _state
) noexcept :
    state_(std::move(_state)) {}

bool GpuScopeRecorder::Valid() const noexcept {
    if (state_ == nullptr || state_->frame == nullptr) {
        return false;
    }
    const std::shared_ptr<GpuScopeDetail::StreamState> owner =
        state_->frame->stream.lock();
    return owner && owner->ContainsFrame(state_->frame);
}

std::uint64_t GpuScopeRecorder::FrameId() const noexcept {
    return Valid() ? state_->frame->frame_id : 0;
}

std::uint64_t GpuScopeRecorder::SourceOrder() const noexcept {
    return Valid() ? state_->source_order : 0;
}

RHIQueueBinding GpuScopeRecorder::QueueBinding() const noexcept {
    return Valid() ?
        state_->queue_binding :
        RHIQueueBinding{
            .queue = EQueueType::Ignore,
            .native_queue_id = 0,
            .family_id = 0,
            .available = false,
        };
}

bool GpuScopeRecorder::IsBound() const noexcept {
    return state_ != nullptr;
}

RHIQueueBinding GpuScopeRecorder::BoundQueueBinding() const noexcept {
    return state_ ?
        state_->queue_binding :
        RHIQueueBinding{
            .queue = EQueueType::Ignore,
            .native_queue_id = 0,
            .family_id = 0,
            .available = false,
        };
}

GpuScopeCompletionTicket GpuScopeRecorder::TryBeginScope(
    std::string_view _name,
    std::uint64_t    _parent_scope_id,
    std::uint32_t    _depth
) const noexcept {
    if (!Valid()) {
        return {};
    }
    return state_->frame->TryBeginScope(
        state_, _name, _parent_scope_id, _depth
    );
}

void GpuScopeRecorder::RecordSuppressedScope() const noexcept {
    if (!Valid()) {
        return;
    }
    if (const std::shared_ptr<GpuScopeDetail::StreamState> owner =
            state_->frame->stream.lock()) {
        owner->counters.scopes_attempted.fetch_add(
            1, std::memory_order_relaxed
        );
        if (!state_->frame->TryStartAdmission()) {
            owner->counters.scopes_dropped_after_seal.fetch_add(
                1, std::memory_order_relaxed
            );
            return;
        }
        struct AdmissionGuard {
            GpuScopeDetail::FrameState* frame{nullptr};
            ~AdmissionGuard() {
                frame->FinishAdmission();
            }
        } admission_guard{state_->frame.get()};
        owner->counters.scopes_dropped_suppressed_subtree.fetch_add(
            1, std::memory_order_relaxed
        );
        state_->frame->RecordDroppedScopeDuringAdmission();
    }
}

GpuScopeFrameHandle::GpuScopeFrameHandle(
    std::shared_ptr<GpuScopeDetail::FrameState> _state
) noexcept :
    state_(std::move(_state)) {}

bool GpuScopeFrameHandle::Valid() const noexcept {
    if (!state_) {
        return false;
    }
    const std::shared_ptr<GpuScopeDetail::StreamState> owner =
        state_->stream.lock();
    return owner && owner->ContainsFrame(state_);
}

std::uint64_t GpuScopeFrameHandle::FrameId() const noexcept {
    return state_ ? state_->frame_id : 0;
}

GpuScopeRecorder GpuScopeFrameHandle::CreateRecorder(
    RHIQueueBinding _queue_binding,
    std::uint64_t   _source_order
) const noexcept {
    if (!state_ ||
        !GpuScopeDetail::IsManagedQueue(_queue_binding.queue) ||
        !_queue_binding.available) {
        return {};
    }
    const std::shared_ptr<GpuScopeDetail::StreamState> owner =
        state_->stream.lock();
    if (!owner) {
        return {};
    }
    if (!owner->ContainsFrame(state_)) {
        return {};
    }

    std::shared_ptr<GpuScopeDetail::RecorderState> recorder{};
    try {
        recorder = std::make_shared<GpuScopeDetail::RecorderState>(
            state_, _queue_binding, _source_order
        );
    } catch (...) {
        owner->counters.sources_dropped_capacity.fetch_add(
            1, std::memory_order_relaxed
        );
        return {};
    }

    {
        std::scoped_lock lock(state_->mutex);
        if (state_->sealed) {
            owner->counters.sources_dropped_after_seal.fetch_add(
                1, std::memory_order_relaxed
            );
            return {};
        }
        if (state_->sources.size() >= state_->max_sources) {
            owner->counters.sources_dropped_capacity.fetch_add(
                1, std::memory_order_relaxed
            );
            return {};
        }
        const bool duplicate = std::any_of(
            state_->sources.begin(),
            state_->sources.end(),
            [&](const GpuScopeDetail::SourceKey& _source) {
                return _source.queue_binding == _queue_binding &&
                       _source.source_order == _source_order;
            }
        );
        if (duplicate) {
            owner->counters.sources_dropped_duplicate_order.fetch_add(
                1, std::memory_order_relaxed
            );
            return {};
        }
        try {
            state_->sources.emplace_back(
                GpuScopeDetail::SourceKey{
                    .queue_binding = _queue_binding,
                    .source_order = _source_order,
                }
            );
        } catch (...) {
            owner->counters.sources_dropped_capacity.fetch_add(
                1, std::memory_order_relaxed
            );
            return {};
        }
    }

    owner->counters.sources_opened.fetch_add(
        1, std::memory_order_relaxed
    );
    return GpuScopeRecorder(std::move(recorder));
}

GpuScopeStream::GpuScopeStream(
    const GpuScopeStreamConfig& _config
) {
    if (_config.max_resident_frames == 0 ||
        _config.max_pending_frames == 0 ||
        _config.max_pending_frames > _config.max_resident_frames ||
        _config.max_resident_scopes == 0 ||
        _config.max_scopes_per_frame == 0 ||
        _config.max_scopes_per_frame >
            _config.max_resident_scopes ||
        _config.max_sources_per_frame == 0 ||
        _config.max_scope_depth == 0 ||
        _config.max_scope_name_bytes == 0 ||
        _config.max_error_reason_bytes == 0) {
        throw std::invalid_argument(
            "GpuScopeStreamConfig contains an invalid zero or capacity relationship"
        );
    }
    state_ =
        std::make_shared<GpuScopeDetail::StreamState>(_config);
}

GpuScopeStream::~GpuScopeStream() {
    Close();
}

GpuScopeStream::GpuScopeStream(
    GpuScopeStream&& _other
) noexcept :
    state_(std::move(_other.state_)) {}

GpuScopeStream& GpuScopeStream::operator=(
    GpuScopeStream&& _other
) noexcept {
    if (this != &_other) {
        Close();
        state_ = std::move(_other.state_);
    }
    return *this;
}

bool GpuScopeStream::Valid() const noexcept {
    return state_ != nullptr;
}

GpuScopeFrameHandle GpuScopeStream::BeginFrame(
    std::uint64_t _frame_id
) noexcept {
    const std::shared_ptr<GpuScopeDetail::StreamState> owner = state_;
    if (!owner) {
        return {};
    }

    std::size_t reserved_slot = GpuScopeDetail::kInvalidStreamSlot;
    {
        std::scoped_lock lock(owner->mutex);
        if (!owner->accepting) {
            return {};
        }
        if (owner->order_count >= owner->config.max_resident_frames) {
            owner->counters.frames_dropped_resident_full.fetch_add(
                1, std::memory_order_relaxed
            );
            return {};
        }
        if (owner->pending_count >= owner->config.max_pending_frames) {
            owner->counters.frames_dropped_pending_full.fetch_add(
                1, std::memory_order_relaxed
            );
            return {};
        }
        bool duplicate = false;
        for (std::size_t slot = 0;
             slot < owner->frame_slots.size();
             ++slot) {
            if ((owner->frame_slots[slot] &&
                 owner->frame_slots[slot]->frame_id == _frame_id) ||
                (owner->frame_reservations[slot] &&
                 owner->reserved_frame_ids[slot] == _frame_id)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            owner->counters.frames_dropped_duplicate_id.fetch_add(
                1, std::memory_order_relaxed
            );
            return {};
        }

        for (std::size_t slot = 0;
             slot < owner->frame_slots.size();
             ++slot) {
            if (!owner->frame_slots[slot] &&
                !owner->frame_reservations[slot]) {
                reserved_slot = slot;
                break;
            }
        }
        if (reserved_slot == GpuScopeDetail::kInvalidStreamSlot) {
            owner->counters.frames_dropped_resident_full.fetch_add(
                1, std::memory_order_relaxed
            );
            return {};
        }

        // Reserve the fixed slot and FIFO position before allocating the
        // frame's configured scope/source payload. The permit bounds all
        // concurrent heavy allocations without holding the stream mutex, so
        // Completion can still terminalize unrelated resident frames.
        owner->frame_reservations[reserved_slot] = true;
        owner->reserved_frame_ids[reserved_slot] = _frame_id;
        owner->ready_flags[reserved_slot] = false;
        owner->frame_order[owner->order_tail] = reserved_slot;
        owner->order_tail =
            (owner->order_tail + 1) %
            owner->frame_order.size();
        ++owner->order_count;
        ++owner->pending_count;
        GpuScopeDetail::UpdateHighWater(
            owner->counters.high_water_frames,
            static_cast<std::uint64_t>(owner->order_count)
        );
        GpuScopeDetail::UpdateHighWater(
            owner->counters.high_water_pending_frames,
            static_cast<std::uint64_t>(owner->pending_count)
        );
    }

    std::shared_ptr<GpuScopeDetail::FrameState> frame{};
    try {
        frame = std::make_shared<GpuScopeDetail::FrameState>(
            owner, _frame_id, owner->config
        );
    } catch (...) {
        owner->counters.frames_dropped_resource_exhaustion.fetch_add(
            1, std::memory_order_relaxed
        );
        owner->CancelFrameReservation(reserved_slot, _frame_id);
        return {};
    }

    {
        std::scoped_lock lock(owner->mutex);
        if (!owner->accepting ||
            reserved_slot >= owner->frame_slots.size() ||
            !owner->frame_reservations[reserved_slot] ||
            owner->reserved_frame_ids[reserved_slot] != _frame_id ||
            owner->frame_slots[reserved_slot]) {
            return {};
        }
        frame->stream_slot = reserved_slot;
        owner->frame_slots[reserved_slot] = frame;
        owner->frame_reservations[reserved_slot] = false;
        owner->reserved_frame_ids[reserved_slot] = 0;
        owner->counters.frames_opened.fetch_add(
            1, std::memory_order_relaxed
        );
    }
    return GpuScopeFrameHandle(std::move(frame));
}

bool GpuScopeStream::SealFrame(
    const GpuScopeFrameHandle& _frame
) noexcept {
    if (!state_ || !_frame.state_) {
        return false;
    }
    const std::shared_ptr<GpuScopeDetail::StreamState> frame_owner =
        _frame.state_->stream.lock();
    if (frame_owner.get() != state_.get()) {
        return false;
    }
    if (!state_->ContainsFrame(_frame.state_)) {
        return false;
    }
    return _frame.state_->Seal();
}

bool GpuScopeStream::TryPopFrame(
    ResolvedGpuScopeFrame& _frame
) {
    const std::shared_ptr<GpuScopeDetail::StreamState> owner = state_;
    if (!owner) {
        return false;
    }

    std::shared_ptr<GpuScopeDetail::FrameState> frame{};
    std::size_t slot = GpuScopeDetail::kInvalidStreamSlot;
    {
        std::scoped_lock lock(owner->mutex);
        if (owner->pop_in_progress || owner->order_count == 0) {
            return false;
        }
        slot = owner->frame_order[owner->order_head];
        if (slot >= owner->frame_slots.size() ||
            !owner->frame_slots[slot] ||
            !owner->ready_flags[slot]) {
            return false;
        }
        owner->pop_in_progress = true;
        owner->pop_slot = slot;
        frame = owner->frame_slots[slot];
    }

    ResolvedGpuScopeFrame resolved{};
    try {
        resolved = frame->Materialize();
    } catch (...) {
        std::scoped_lock lock(owner->mutex);
        if (owner->pop_in_progress && owner->pop_slot == slot) {
            owner->pop_in_progress = false;
            owner->pop_slot = GpuScopeDetail::kInvalidStreamSlot;
        }
        throw;
    }

    const std::size_t released_scopes =
        frame->AdmittedScopeCount();
    {
        std::scoped_lock lock(owner->mutex);
        if (!owner->pop_in_progress ||
            owner->pop_slot != slot ||
            owner->order_count == 0 ||
            owner->frame_order[owner->order_head] != slot ||
            owner->frame_slots[slot].get() != frame.get() ||
            !owner->ready_flags[slot]) {
            owner->pop_in_progress = false;
            owner->pop_slot = GpuScopeDetail::kInvalidStreamSlot;
            return false;
        }

        owner->frame_slots[slot].reset();
        owner->ready_flags[slot] = false;
        owner->order_head =
            (owner->order_head + 1) %
            owner->frame_order.size();
        --owner->order_count;
        if (owner->ready_count != 0) {
            --owner->ready_count;
        }
        owner->resident_scope_count =
            released_scopes <= owner->resident_scope_count ?
                owner->resident_scope_count - released_scopes :
                0;
        owner->pop_in_progress = false;
        owner->pop_slot = GpuScopeDetail::kInvalidStreamSlot;
        owner->counters.frames_popped.fetch_add(
            1, std::memory_order_relaxed
        );
    }

    _frame = std::move(resolved);
    return true;
}

GpuScopeStreamStats GpuScopeStream::GetStats() const noexcept {
    GpuScopeStreamStats stats{};
    const std::shared_ptr<GpuScopeDetail::StreamState> owner = state_;
    if (!owner) {
        return stats;
    }

#define MOER_LOAD_GPU_SCOPE_COUNTER(name) \
    stats.name = owner->counters.name.load(std::memory_order_relaxed)
    MOER_LOAD_GPU_SCOPE_COUNTER(frames_opened);
    MOER_LOAD_GPU_SCOPE_COUNTER(frames_sealed);
    MOER_LOAD_GPU_SCOPE_COUNTER(frames_ready);
    MOER_LOAD_GPU_SCOPE_COUNTER(frames_popped);
    MOER_LOAD_GPU_SCOPE_COUNTER(frames_dropped_resident_full);
    MOER_LOAD_GPU_SCOPE_COUNTER(frames_dropped_pending_full);
    MOER_LOAD_GPU_SCOPE_COUNTER(frames_dropped_duplicate_id);
    MOER_LOAD_GPU_SCOPE_COUNTER(frames_dropped_resource_exhaustion);
    MOER_LOAD_GPU_SCOPE_COUNTER(frames_abandoned_on_close);
    MOER_LOAD_GPU_SCOPE_COUNTER(sources_opened);
    MOER_LOAD_GPU_SCOPE_COUNTER(sources_dropped_capacity);
    MOER_LOAD_GPU_SCOPE_COUNTER(sources_dropped_duplicate_order);
    MOER_LOAD_GPU_SCOPE_COUNTER(sources_dropped_after_seal);
    MOER_LOAD_GPU_SCOPE_COUNTER(scopes_attempted);
    MOER_LOAD_GPU_SCOPE_COUNTER(scopes_admitted);
    MOER_LOAD_GPU_SCOPE_COUNTER(scopes_ready);
    MOER_LOAD_GPU_SCOPE_COUNTER(scopes_error);
    MOER_LOAD_GPU_SCOPE_COUNTER(scopes_dropped_resident_full);
    MOER_LOAD_GPU_SCOPE_COUNTER(scopes_dropped_frame_full);
    MOER_LOAD_GPU_SCOPE_COUNTER(scopes_dropped_name_too_large);
    MOER_LOAD_GPU_SCOPE_COUNTER(scopes_dropped_invalid_hierarchy);
    MOER_LOAD_GPU_SCOPE_COUNTER(scopes_dropped_suppressed_subtree);
    MOER_LOAD_GPU_SCOPE_COUNTER(scopes_dropped_after_seal);
    MOER_LOAD_GPU_SCOPE_COUNTER(scopes_dropped_resource_exhaustion);
    MOER_LOAD_GPU_SCOPE_COUNTER(high_water_frames);
    MOER_LOAD_GPU_SCOPE_COUNTER(high_water_pending_frames);
    MOER_LOAD_GPU_SCOPE_COUNTER(high_water_ready_frames);
    MOER_LOAD_GPU_SCOPE_COUNTER(high_water_scopes);
#undef MOER_LOAD_GPU_SCOPE_COUNTER

    {
        std::scoped_lock lock(owner->mutex);
        stats.accepting = owner->accepting;
        stats.resident_frames =
            static_cast<std::uint64_t>(owner->order_count);
        stats.resident_pending_frames =
            static_cast<std::uint64_t>(owner->pending_count);
        stats.resident_ready_frames =
            static_cast<std::uint64_t>(owner->ready_count);
        stats.resident_scopes =
            static_cast<std::uint64_t>(
                owner->resident_scope_count
            );
    }
    return stats;
}

void GpuScopeStream::Close() noexcept {
    const std::shared_ptr<GpuScopeDetail::StreamState> owner = state_;
    if (!owner) {
        return;
    }

    Array<std::shared_ptr<GpuScopeDetail::FrameState>> detached_frames{};
    {
        std::scoped_lock lock(owner->mutex);
        if (!owner->accepting && owner->order_count == 0) {
            return;
        }
        owner->accepting = false;
        const std::uint64_t opened_frames_abandoned =
            static_cast<std::uint64_t>(std::count_if(
                owner->frame_slots.begin(),
                owner->frame_slots.end(),
                [](const auto& _frame) {
                    return _frame != nullptr;
                }
            ));
        owner->counters.frames_abandoned_on_close.fetch_add(
            opened_frames_abandoned,
            std::memory_order_relaxed
        );

        // Detach ownership in O(1) while holding the state lock. Destroying a
        // full frame releases thousands of names/slots and must happen after
        // Completion can reacquire the mutex.
        detached_frames.swap(owner->frame_slots);
        std::fill(
            owner->frame_reservations.begin(),
            owner->frame_reservations.end(),
            false
        );
        std::fill(
            owner->reserved_frame_ids.begin(),
            owner->reserved_frame_ids.end(),
            0
        );
        std::fill(
            owner->ready_flags.begin(),
            owner->ready_flags.end(),
            false
        );
        owner->order_head = 0;
        owner->order_tail = 0;
        owner->order_count = 0;
        owner->pending_count = 0;
        owner->ready_count = 0;
        owner->resident_scope_count = 0;
        owner->pop_in_progress = false;
        owner->pop_slot = GpuScopeDetail::kInvalidStreamSlot;
    }
    // detached_frames releases its payload outside StreamState::mutex.
}

#if defined(MOER_RHI_GPU_SCOPE_TEST_HOOKS)
namespace GpuScopeTesting {

void InstallAdmissionPause(
    std::atomic_uint32_t& _entered_count,
    std::atomic_bool&     _release
) noexcept {
    GpuScopeDetail::g_admission_pause_release.store(
        &_release, std::memory_order_release
    );
    GpuScopeDetail::g_admission_pause_entered.store(
        &_entered_count, std::memory_order_release
    );
}

void ClearAdmissionPause() noexcept {
    GpuScopeDetail::g_admission_pause_entered.store(
        nullptr, std::memory_order_release
    );
    GpuScopeDetail::g_admission_pause_release.store(
        nullptr, std::memory_order_release
    );
}

} // namespace GpuScopeTesting
#endif

} // namespace Moer::Render
