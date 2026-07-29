#include "profile_consumer/ProfileTimelineIndex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Moer::ProfileDump {
namespace {

struct TimelineIntervalTree {
    std::uint64_t tree_offset{0};
    std::uint64_t leaf_count{0};
};

struct GpuFrameSlicePlan {
    std::uint32_t track_index{0};
    std::uint32_t domain_index{0};
    std::uint64_t frame_id{0};
    std::uint64_t source_frame_index{kInvalidSessionIndex};
    std::uint64_t first_scope{0};
    std::uint64_t scope_count{0};
    std::uint64_t ready_scope_count{0};
    std::uint64_t error_scope_count{0};
    std::uint64_t axis_frame_index{kInvalidSessionIndex};
    std::uint64_t tree_offset{0};
    std::uint64_t tree_leaf_count{0};
};

struct GpuAxisFramePlan {
    std::uint32_t         domain_index{0};
    std::uint64_t         frame_id{0};
    std::uint64_t         source_frame_index{kInvalidSessionIndex};
    std::uint64_t         origin_tick{0};
    std::uint64_t         extent_ticks{0};
    std::uint64_t         ready_scope_count{0};
    std::uint64_t         error_scope_count{0};
    ProfileGpuFrameStatus frame_status{ProfileGpuFrameStatus::Invalid};
    bool                  has_frame_record{false};
    bool                  timing_available{false};
    bool                  materialization_complete{false};
    bool                  timing_topology_trusted{false};
};

struct GpuBuildPlan {
    std::vector<GpuFrameSlicePlan> slices{};
    std::vector<GpuAxisFramePlan>  axis_frames{};
    std::vector<std::uint64_t>     track_first_slice{};
    std::vector<std::uint64_t>     track_slice_count{};
    std::uint64_t                  timeline_scope_count{0};
    std::uint64_t                  tree_value_count{0};
};

class CancellationProbe final {
public:
    explicit CancellationProbe(const TimelineIndexBuildControl& _control) noexcept :
        control_(_control),
        interval_(std::max<std::uint64_t>(std::uint64_t{1}, _control.cancellation_check_interval)),
        remaining_budget_(_control.max_work_items_before_cancel) {}

    [[nodiscard]] bool Requested(std::uint64_t _work_items = 1) noexcept {
        if (_work_items > remaining_budget_) {
            remaining_budget_ = 0;
            return true;
        }
        remaining_budget_ -= _work_items;
        if (!control_.stop_token.stop_possible()) {
            return false;
        }
        if (_work_items >= interval_ - pending_work_) {
            pending_work_ = 0;
            return control_.stop_token.stop_requested();
        }
        pending_work_ += _work_items;
        return false;
    }

    [[nodiscard]] bool RequestedNow() const noexcept {
        return remaining_budget_ == 0 || control_.stop_token.stop_requested();
    }

    [[nodiscard]] std::size_t SortRunSize() const noexcept {
        constexpr std::uint64_t kMaximumSortRunSize = 4096;
        return static_cast<std::size_t>(std::min(interval_, kMaximumSortRunSize));
    }

private:
    const TimelineIndexBuildControl& control_;
    std::uint64_t                    interval_{1};
    std::uint64_t                    pending_work_{0};
    std::uint64_t                    remaining_budget_{std::numeric_limits<std::uint64_t>::max()};
};

[[nodiscard]] bool MarkCancelled(
    CancellationProbe&        _cancellation,
    TimelineIndexBuildResult& _result,
    std::uint64_t             _work_items = 1
) noexcept;

[[nodiscard]] bool
MarkCancelledNow(const CancellationProbe& _cancellation, TimelineIndexBuildResult& _result) noexcept;

template<typename T, typename Compare>
[[nodiscard]] bool CancellableSort(
    std::vector<T>&           _values,
    std::size_t               _first,
    std::size_t               _count,
    Compare                   _compare,
    CancellationProbe&        _cancellation,
    TimelineIndexBuildResult& _result
) {
    if (_first > _values.size() || _count > _values.size() - _first) {
        _result.status = TimelineIndexBuildStatus::ProtocolViolation;
        return false;
    }
    if (_count < 2) {
        return !MarkCancelledNow(_cancellation, _result);
    }

    const std::size_t run_size = std::min(_count, _cancellation.SortRunSize());
    for (std::size_t run_begin = 0; run_begin < _count;) {
        const std::size_t run_count = std::min(run_size, _count - run_begin);
        if (MarkCancelled(_cancellation, _result, run_count)) {
            return false;
        }
        const auto begin = _values.begin() + static_cast<std::ptrdiff_t>(_first + run_begin);
        std::sort(begin, begin + static_cast<std::ptrdiff_t>(run_count), _compare);
        run_begin += run_count;
    }

    if (MarkCancelledNow(_cancellation, _result) || run_size == _count) {
        return _result.status != TimelineIndexBuildStatus::Cancelled;
    }
    std::vector<T> scratch;
    scratch.reserve(_count);
    for (std::size_t width = run_size; width < _count;) {
        scratch.clear();
        std::size_t block_begin = 0;
        while (block_begin < _count) {
            const std::size_t middle    = block_begin + std::min(width, _count - block_begin);
            const std::size_t block_end = middle + std::min(width, _count - middle);
            std::size_t       left      = block_begin;
            std::size_t       right     = middle;
            while (left < middle || right < block_end) {
                if (MarkCancelled(_cancellation, _result)) {
                    return false;
                }
                if (right == block_end ||
                    (left < middle && !_compare(_values[_first + right], _values[_first + left]))) {
                    scratch.push_back(_values[_first + left]);
                    ++left;
                } else {
                    scratch.push_back(_values[_first + right]);
                    ++right;
                }
            }
            block_begin = block_end;
        }
        if (scratch.size() != _count) {
            _result.status = TimelineIndexBuildStatus::ProtocolViolation;
            return false;
        }
        for (std::size_t index = 0; index < _count; ++index) {
            if (MarkCancelled(_cancellation, _result)) {
                return false;
            }
            _values[_first + index] = std::move(scratch[index]);
        }
        width = width > _count - width ? _count : width * 2;
    }
    return !MarkCancelledNow(_cancellation, _result);
}

[[nodiscard]] bool AddOverflow(std::uint64_t _left, std::uint64_t _right, std::uint64_t& _result) noexcept {
    if (_right > std::numeric_limits<std::uint64_t>::max() - _left) {
        return true;
    }
    _result = _left + _right;
    return false;
}

[[nodiscard]] bool
MultiplyOverflow(std::uint64_t _left, std::uint64_t _right, std::uint64_t& _result) noexcept {
    if (_left != 0 && _right > std::numeric_limits<std::uint64_t>::max() / _left) {
        return true;
    }
    _result = _left * _right;
    return false;
}

[[nodiscard]] bool NextPowerOfTwo(std::uint64_t _value, std::uint64_t& _result) noexcept {
    if (_value == 0) {
        _result = 1;
        return true;
    }
    std::uint64_t power = 1;
    while (power < _value) {
        if (power > std::numeric_limits<std::uint64_t>::max() / 2) {
            return false;
        }
        power *= 2;
    }
    _result = power;
    return true;
}

[[nodiscard]] std::uint64_t TimestampMask(std::uint32_t _valid_bits) noexcept {
    return _valid_bits == 64 ? std::numeric_limits<std::uint64_t>::max() :
                               (std::uint64_t{1} << _valid_bits) - 1;
}

[[nodiscard]] std::uint64_t
TimestampDelta(std::uint64_t _begin_tick, std::uint64_t _end_tick, std::uint32_t _valid_bits) noexcept {
    return (_end_tick - _begin_tick) & TimestampMask(_valid_bits);
}

[[nodiscard]] bool NearlyEqual(double _left, double _right) noexcept {
    if (!std::isfinite(_left) || !std::isfinite(_right)) {
        return false;
    }
    const double tolerance = std::max(1.0e-6, std::max(std::abs(_left), std::abs(_right)) * 1.0e-9);
    return std::abs(_left - _right) <= tolerance;
}

[[nodiscard]] std::uint64_t QueryEnd(const CpuTimelineScopeRef& _scope) noexcept {
    if (_scope.end_ns > _scope.begin_ns) {
        return _scope.end_ns;
    }
    return _scope.begin_ns == std::numeric_limits<std::uint64_t>::max() ? _scope.begin_ns :
                                                                          _scope.begin_ns + 1;
}

[[nodiscard]] std::uint64_t QueryEnd(const GpuTimelineScopeRef& _scope) noexcept {
    if (_scope.end_tick_offset > _scope.begin_tick_offset) {
        return _scope.end_tick_offset;
    }
    return _scope.begin_tick_offset == std::numeric_limits<std::uint64_t>::max() ?
               _scope.begin_tick_offset :
               _scope.begin_tick_offset + 1;
}

[[nodiscard]] bool
Overlaps(const CpuTimelineScopeRef& _scope, std::uint64_t _view_begin, std::uint64_t _view_end) noexcept {
    if (_scope.end_ns > _scope.begin_ns) {
        return _scope.end_ns > _view_begin && _scope.begin_ns < _view_end;
    }
    return _scope.begin_ns >= _view_begin && _scope.begin_ns < _view_end;
}

[[nodiscard]] bool
Overlaps(const GpuTimelineScopeRef& _scope, std::uint64_t _view_begin, std::uint64_t _view_end) noexcept {
    if (_scope.end_tick_offset > _scope.begin_tick_offset) {
        return _scope.end_tick_offset > _view_begin && _scope.begin_tick_offset < _view_end;
    }
    return _scope.begin_tick_offset >= _view_begin && _scope.begin_tick_offset < _view_end;
}

template<typename ScopeRef, typename Output, typename Project>
[[nodiscard]] TimelineOverlapQueryResult QueryTimelineOverlaps(
    std::span<const ScopeRef>      _scopes,
    std::uint64_t                  _first_scope,
    std::uint64_t                  _scope_count,
    const TimelineIntervalTree&    _tree,
    std::span<const std::uint64_t> _tree_max_end,
    std::uint64_t                  _view_begin,
    std::uint64_t                  _view_end,
    std::span<Output>              _output,
    Project                        _project
) noexcept {
    TimelineOverlapQueryResult query{};
    if (_view_end <= _view_begin || _first_scope > _scopes.size() ||
        _scope_count > _scopes.size() - _first_scope) {
        return query;
    }

    query.valid = true;
    if (_scope_count == 0) {
        return query;
    }
    if (_tree.leaf_count == 0 || _tree.tree_offset > _tree_max_end.size() ||
        _tree.leaf_count > (_tree_max_end.size() - _tree.tree_offset) / 2) {
        query.valid = false;
        return query;
    }

    struct StackNode {
        std::uint64_t node{0};
        std::uint64_t begin{0};
        std::uint64_t end{0};
    };
    std::array<StackNode, 128> stack{};
    std::size_t                stack_size = 0;
    stack[stack_size++]                   = {
                          .node  = 1,
                          .begin = 0,
                          .end   = _tree.leaf_count,
    };

    while (stack_size != 0) {
        const StackNode current = stack[--stack_size];
        if (current.begin >= _scope_count) {
            continue;
        }
        const std::uint64_t tree_value = _tree_max_end[_tree.tree_offset + current.node];
        if (tree_value <= _view_begin) {
            continue;
        }

        const ScopeRef& first = _scopes[_first_scope + current.begin];
        if constexpr (std::is_same_v<ScopeRef, CpuTimelineScopeRef>) {
            if (first.begin_ns >= _view_end) {
                continue;
            }
        } else {
            if (first.begin_tick_offset >= _view_end) {
                continue;
            }
        }

        if (current.end - current.begin == 1) {
            if (!Overlaps(first, _view_begin, _view_end)) {
                continue;
            }
            if (query.written < _output.size()) {
                _output[query.written++] = _project(first);
                continue;
            }
            query.truncated = true;
            break;
        }

        const std::uint64_t middle = current.begin + (current.end - current.begin) / 2;
        if (stack_size + 2 > stack.size()) {
            query.valid = false;
            return query;
        }
        // Push right first so the left/begin-time half is visited first.
        stack[stack_size++] = {
            .node  = current.node * 2 + 1,
            .begin = middle,
            .end   = current.end,
        };
        stack[stack_size++] = {
            .node  = current.node * 2,
            .begin = current.begin,
            .end   = middle,
        };
    }
    return query;
}

void AddQualityFlag(ProfileTimelineQuality& _quality, TimelineQualityFlag _flag) noexcept {
    _quality.flags |= TimelineQualityBit(_flag);
}

[[nodiscard]] bool MarkCancelled(
    CancellationProbe&        _cancellation,
    TimelineIndexBuildResult& _result,
    std::uint64_t             _work_items
) noexcept {
    if (!_cancellation.Requested(_work_items)) {
        return false;
    }
    _result.status = TimelineIndexBuildStatus::Cancelled;
    return true;
}

[[nodiscard]] bool
MarkCancelledNow(const CancellationProbe& _cancellation, TimelineIndexBuildResult& _result) noexcept {
    if (!_cancellation.RequestedNow()) {
        return false;
    }
    _result.status = TimelineIndexBuildStatus::Cancelled;
    return true;
}

[[nodiscard]] bool ComputeGpuOffsets(
    const GpuScopeRecord&     _scope,
    const GpuTimestampDomain& _domain,
    const GpuAxisFramePlan&   _axis_frame,
    std::uint64_t&            _begin_offset,
    std::uint64_t&            _end_offset
) noexcept {
    const std::uint64_t mask     = TimestampMask(_domain.valid_bits);
    _begin_offset                = (_scope.begin_tick - _axis_frame.origin_tick) & mask;
    const std::uint64_t duration = TimestampDelta(_scope.begin_tick, _scope.end_tick, _domain.valid_bits);
    if (_begin_offset > _axis_frame.extent_ticks || duration > _axis_frame.extent_ticks - _begin_offset) {
        return false;
    }
    _end_offset = _begin_offset + duration;
    return true;
}

[[nodiscard]] bool PrepareGpuBuildPlan(
    std::span<const GpuFrameRecord>     _gpu_frames,
    std::span<const GpuScopeRecord>     _gpu_scopes,
    std::span<const GpuTrack>           _gpu_tracks,
    std::span<const GpuTimestampDomain> _gpu_domains,
    const TimelineIndexLimits&          _limits,
    CancellationProbe&                  _cancellation,
    TimelineIndexBuildResult&           _result,
    GpuBuildPlan&                       _plan
) {
    std::uint64_t planned_slice_count = 0;
    std::uint64_t ready_scope_count   = 0;
    for (std::size_t track_index = 0; track_index < _gpu_tracks.size(); ++track_index) {
        if (MarkCancelled(_cancellation, _result)) {
            return false;
        }
        const GpuTrack& track = _gpu_tracks[track_index];
        if (track.domain_index >= _gpu_domains.size() || track.first_scope > _gpu_scopes.size() ||
            track.scope_count > _gpu_scopes.size() - track.first_scope) {
            _result.status = TimelineIndexBuildStatus::ProtocolViolation;
            return false;
        }

        const std::size_t source_begin         = static_cast<std::size_t>(track.first_scope);
        const std::size_t source_end           = source_begin + static_cast<std::size_t>(track.scope_count);
        bool              has_frame            = false;
        std::uint64_t     previous_frame_id    = 0;
        std::uint64_t     previous_frame_index = kInvalidSessionIndex;
        for (std::size_t scope_index = source_begin; scope_index < source_end; ++scope_index) {
            if (MarkCancelled(_cancellation, _result)) {
                return false;
            }
            const GpuScopeRecord& scope = _gpu_scopes[scope_index];
            if (scope.track_index != track_index || scope.domain_index != track.domain_index) {
                _result.status = TimelineIndexBuildStatus::ProtocolViolation;
                return false;
            }
            if (scope.status == ProfileGpuScopeStatus::Ready &&
                AddOverflow(ready_scope_count, std::uint64_t{1}, ready_scope_count)) {
                _result.status = TimelineIndexBuildStatus::ResourceExhausted;
                return false;
            }
            if (!has_frame || scope.frame_id != previous_frame_id) {
                if (has_frame && previous_frame_id >= scope.frame_id) {
                    _result.status = TimelineIndexBuildStatus::ProtocolViolation;
                    return false;
                }
                if (planned_slice_count >= _limits.max_gpu_frame_slices) {
                    _result.status     = TimelineIndexBuildStatus::LimitExceeded;
                    _result.limit_kind = TimelineIndexLimitKind::GpuFrameSlices;
                    return false;
                }
                ++planned_slice_count;
                has_frame            = true;
                previous_frame_id    = scope.frame_id;
                previous_frame_index = scope.frame_index;
                if (scope.frame_index != kInvalidSessionIndex &&
                    (scope.frame_index >= _gpu_frames.size() ||
                     _gpu_frames[scope.frame_index].frame_id != scope.frame_id)) {
                    _result.status = TimelineIndexBuildStatus::ProtocolViolation;
                    return false;
                }
            } else if (scope.frame_index != previous_frame_index) {
                _result.status = TimelineIndexBuildStatus::ProtocolViolation;
                return false;
            }
        }
    }

    const auto add_bytes = [](std::uint64_t& _total, std::uint64_t _count, std::uint64_t _size) {
        std::uint64_t bytes = 0;
        return !MultiplyOverflow(_count, _size, bytes) && !AddOverflow(_total, bytes, _total);
    };
    std::uint64_t initial_transient_bytes = 0;
    if (!add_bytes(initial_transient_bytes, _gpu_tracks.size(), sizeof(std::uint64_t)) ||
        !add_bytes(initial_transient_bytes, _gpu_tracks.size(), sizeof(std::uint64_t)) ||
        !add_bytes(initial_transient_bytes, planned_slice_count, sizeof(GpuFrameSlicePlan)) ||
        !add_bytes(initial_transient_bytes, planned_slice_count, sizeof(std::size_t)) ||
        !add_bytes(initial_transient_bytes, planned_slice_count, sizeof(std::size_t))) {
        _result.status = TimelineIndexBuildStatus::ResourceExhausted;
        return false;
    }
    if (initial_transient_bytes > _limits.max_transient_build_bytes) {
        _result.status     = TimelineIndexBuildStatus::LimitExceeded;
        _result.limit_kind = TimelineIndexLimitKind::TransientBuildBytes;
        return false;
    }

    _plan.track_first_slice.resize(_gpu_tracks.size(), 0);
    _plan.track_slice_count.resize(_gpu_tracks.size(), 0);
    _plan.slices.reserve(static_cast<std::size_t>(planned_slice_count));

    for (std::size_t track_index = 0; track_index < _gpu_tracks.size(); ++track_index) {
        if (MarkCancelled(_cancellation, _result)) {
            return false;
        }
        const GpuTrack& track = _gpu_tracks[track_index];
        if (track.domain_index >= _gpu_domains.size() || track.first_scope > _gpu_scopes.size() ||
            track.scope_count > _gpu_scopes.size() - track.first_scope) {
            _result.status = TimelineIndexBuildStatus::ProtocolViolation;
            return false;
        }

        _plan.track_first_slice[track_index] = _plan.slices.size();
        const std::size_t source_begin       = static_cast<std::size_t>(track.first_scope);
        const std::size_t source_end         = source_begin + static_cast<std::size_t>(track.scope_count);
        std::size_t       slice_begin        = source_begin;
        while (slice_begin < source_end) {
            if (MarkCancelled(_cancellation, _result)) {
                return false;
            }
            std::size_t slice_end = slice_begin + 1;
            while (slice_end < source_end &&
                   _gpu_scopes[slice_end].frame_id == _gpu_scopes[slice_begin].frame_id) {
                if (MarkCancelled(_cancellation, _result)) {
                    return false;
                }
                ++slice_end;
            }
            if (slice_begin != source_begin &&
                _gpu_scopes[slice_begin - 1].frame_id >= _gpu_scopes[slice_begin].frame_id) {
                _result.status = TimelineIndexBuildStatus::ProtocolViolation;
                return false;
            }

            const GpuScopeRecord& first = _gpu_scopes[slice_begin];
            GpuFrameSlicePlan     slice{
                    .track_index        = static_cast<std::uint32_t>(track_index),
                    .domain_index       = track.domain_index,
                    .frame_id           = first.frame_id,
                    .source_frame_index = first.frame_index,
                    .first_scope        = slice_begin,
                    .scope_count        = slice_end - slice_begin,
            };
            for (std::size_t scope_index = slice_begin; scope_index < slice_end; ++scope_index) {
                if (MarkCancelled(_cancellation, _result)) {
                    return false;
                }
                const GpuScopeRecord& scope = _gpu_scopes[scope_index];
                if (scope.track_index != track_index || scope.domain_index != track.domain_index ||
                    scope.frame_id != slice.frame_id || scope.frame_index != slice.source_frame_index) {
                    _result.status = TimelineIndexBuildStatus::ProtocolViolation;
                    return false;
                }
                if (scope.status == ProfileGpuScopeStatus::Ready) {
                    ++slice.ready_scope_count;
                } else {
                    ++slice.error_scope_count;
                }
            }
            if (slice.source_frame_index != kInvalidSessionIndex) {
                if (slice.source_frame_index >= _gpu_frames.size() ||
                    _gpu_frames[slice.source_frame_index].frame_id != slice.frame_id) {
                    _result.status = TimelineIndexBuildStatus::ProtocolViolation;
                    return false;
                }
            }
            if (_plan.slices.size() >= _limits.max_gpu_frame_slices) {
                _result.status     = TimelineIndexBuildStatus::LimitExceeded;
                _result.limit_kind = TimelineIndexLimitKind::GpuFrameSlices;
                return false;
            }
            _plan.slices.push_back(slice);
            slice_begin = slice_end;
        }
        _plan.track_slice_count[track_index] = _plan.slices.size() - _plan.track_first_slice[track_index];
    }

    if (MarkCancelled(_cancellation, _result)) {
        return false;
    }
    std::vector<std::size_t> slice_order;
    slice_order.reserve(_plan.slices.size());
    for (std::size_t index = 0; index < _plan.slices.size(); ++index) {
        if (MarkCancelled(_cancellation, _result)) {
            return false;
        }
        slice_order.push_back(index);
    }
    if (!CancellableSort(
            slice_order,
            0,
            slice_order.size(),
            [&](std::size_t _left, std::size_t _right) {
                const GpuFrameSlicePlan& left  = _plan.slices[_left];
                const GpuFrameSlicePlan& right = _plan.slices[_right];
                return std::tie(left.domain_index, left.frame_id, left.track_index) <
                       std::tie(right.domain_index, right.frame_id, right.track_index);
            },
            _cancellation,
            _result
        )) {
        return false;
    }

    std::uint64_t axis_frame_count = 0;
    for (std::size_t index = 0; index < slice_order.size(); ++index) {
        if (MarkCancelled(_cancellation, _result)) {
            return false;
        }
        if (index == 0) {
            axis_frame_count = 1;
            continue;
        }
        const GpuFrameSlicePlan& previous = _plan.slices[slice_order[index - 1]];
        const GpuFrameSlicePlan& current  = _plan.slices[slice_order[index]];
        if (previous.domain_index != current.domain_index || previous.frame_id != current.frame_id) {
            ++axis_frame_count;
        }
    }

    std::uint64_t persistent_plan_bytes = 0;
    std::uint64_t endpoint_peak_bytes   = 0;
    std::uint64_t timeline_sort_bytes   = 0;
    if (!add_bytes(persistent_plan_bytes, _gpu_tracks.size(), sizeof(std::uint64_t)) ||
        !add_bytes(persistent_plan_bytes, _gpu_tracks.size(), sizeof(std::uint64_t)) ||
        !add_bytes(persistent_plan_bytes, planned_slice_count, sizeof(GpuFrameSlicePlan)) ||
        !add_bytes(persistent_plan_bytes, axis_frame_count, sizeof(GpuAxisFramePlan)) ||
        !add_bytes(endpoint_peak_bytes, planned_slice_count, sizeof(std::size_t)) ||
        !add_bytes(endpoint_peak_bytes, ready_scope_count, sizeof(std::uint64_t)) ||
        !add_bytes(endpoint_peak_bytes, ready_scope_count, sizeof(std::uint64_t)) ||
        !add_bytes(endpoint_peak_bytes, ready_scope_count, sizeof(std::uint64_t)) ||
        !add_bytes(endpoint_peak_bytes, ready_scope_count, sizeof(std::uint64_t)) ||
        !add_bytes(timeline_sort_bytes, ready_scope_count, sizeof(GpuTimelineScopeRef))) {
        _result.status = TimelineIndexBuildStatus::ResourceExhausted;
        return false;
    }
    std::uint64_t peak_transient_bytes = 0;
    if (AddOverflow(
            persistent_plan_bytes, std::max(endpoint_peak_bytes, timeline_sort_bytes), peak_transient_bytes
        )) {
        _result.status = TimelineIndexBuildStatus::ResourceExhausted;
        return false;
    }
    if (peak_transient_bytes > _limits.max_transient_build_bytes) {
        _result.status     = TimelineIndexBuildStatus::LimitExceeded;
        _result.limit_kind = TimelineIndexLimitKind::TransientBuildBytes;
        return false;
    }
    _plan.axis_frames.reserve(static_cast<std::size_t>(axis_frame_count));

    std::vector<std::uint64_t> endpoints;
    std::size_t                group_begin = 0;
    while (group_begin < slice_order.size()) {
        std::size_t              group_end   = group_begin + 1;
        const GpuFrameSlicePlan& first_slice = _plan.slices[slice_order[group_begin]];
        while (group_end < slice_order.size()) {
            if (MarkCancelled(_cancellation, _result)) {
                return false;
            }
            const GpuFrameSlicePlan& candidate = _plan.slices[slice_order[group_end]];
            if (candidate.domain_index != first_slice.domain_index ||
                candidate.frame_id != first_slice.frame_id) {
                break;
            }
            ++group_end;
        }

        if (MarkCancelled(_cancellation, _result)) {
            return false;
        }
        GpuAxisFramePlan axis_frame{
            .domain_index       = first_slice.domain_index,
            .frame_id           = first_slice.frame_id,
            .source_frame_index = first_slice.source_frame_index,
        };
        if (axis_frame.source_frame_index != kInvalidSessionIndex) {
            const GpuFrameRecord& frame         = _gpu_frames[axis_frame.source_frame_index];
            axis_frame.frame_status             = frame.status;
            axis_frame.has_frame_record         = true;
            axis_frame.materialization_complete = frame.materialization_complete;
            axis_frame.timing_topology_trusted  = frame.timing_topology_trusted;
        }

        endpoints.clear();
        std::uint64_t group_ready_scope_count = 0;
        for (std::size_t order_index = group_begin; order_index < group_end; ++order_index) {
            if (MarkCancelled(_cancellation, _result) ||
                AddOverflow(
                    group_ready_scope_count,
                    _plan.slices[slice_order[order_index]].ready_scope_count,
                    group_ready_scope_count
                )) {
                if (_result.status != TimelineIndexBuildStatus::Cancelled) {
                    _result.status = TimelineIndexBuildStatus::ResourceExhausted;
                }
                return false;
            }
        }
        std::uint64_t endpoint_count = 0;
        if (MultiplyOverflow(group_ready_scope_count, std::uint64_t{2}, endpoint_count) ||
            endpoint_count > std::numeric_limits<std::size_t>::max()) {
            _result.status = TimelineIndexBuildStatus::ResourceExhausted;
            return false;
        }
        endpoints.reserve(static_cast<std::size_t>(endpoint_count));
        bool                      timing_layout_valid = true;
        const GpuTimestampDomain& domain              = _gpu_domains[axis_frame.domain_index];
        for (std::size_t order_index = group_begin; order_index < group_end; ++order_index) {
            GpuFrameSlicePlan& slice = _plan.slices[slice_order[order_index]];
            if (slice.source_frame_index != axis_frame.source_frame_index) {
                _result.status = TimelineIndexBuildStatus::ProtocolViolation;
                return false;
            }
            axis_frame.ready_scope_count += slice.ready_scope_count;
            axis_frame.error_scope_count += slice.error_scope_count;
            for (std::uint64_t scope_offset = 0; scope_offset < slice.scope_count; ++scope_offset) {
                if (MarkCancelled(_cancellation, _result)) {
                    return false;
                }
                const GpuScopeRecord& scope = _gpu_scopes[slice.first_scope + scope_offset];
                if (scope.status != ProfileGpuScopeStatus::Ready) {
                    continue;
                }
                if (!domain.has_ready_timestamps || domain.valid_bits == 0 || domain.valid_bits > 64 ||
                    scope.valid_bits != domain.valid_bits ||
                    !NearlyEqual(scope.tick_period_ns, domain.tick_period_ns)) {
                    timing_layout_valid = false;
                    continue;
                }
                const std::uint64_t mask = TimestampMask(domain.valid_bits);
                endpoints.push_back(scope.begin_tick & mask);
                endpoints.push_back(scope.end_tick & mask);
            }
        }

        if (timing_layout_valid && !endpoints.empty()) {
            if (!CancellableSort(
                    endpoints,
                    0,
                    endpoints.size(),
                    [](std::uint64_t _left, std::uint64_t _right) {
                        return _left < _right;
                    },
                    _cancellation,
                    _result
                )) {
                return false;
            }
            std::size_t unique_count = 1;
            for (std::size_t index = 1; index < endpoints.size(); ++index) {
                if (MarkCancelled(_cancellation, _result)) {
                    return false;
                }
                if (endpoints[index] != endpoints[unique_count - 1]) {
                    endpoints[unique_count] = endpoints[index];
                    ++unique_count;
                }
            }
            endpoints.resize(unique_count);

            if (endpoints.size() == 1) {
                axis_frame.origin_tick  = endpoints.front();
                axis_frame.extent_ticks = 0;
            } else {
                std::uint64_t largest_gap = 0;
                std::uint64_t origin_tick = endpoints.front();
                for (std::size_t index = 1; index < endpoints.size(); ++index) {
                    if (MarkCancelled(_cancellation, _result)) {
                        return false;
                    }
                    const std::uint64_t gap = endpoints[index] - endpoints[index - 1];
                    if (gap > largest_gap) {
                        largest_gap = gap;
                        origin_tick = endpoints[index];
                    }
                }
                const std::uint64_t wrap_gap =
                    (endpoints.front() - endpoints.back()) & TimestampMask(domain.valid_bits);
                if (wrap_gap > largest_gap) {
                    largest_gap = wrap_gap;
                    origin_tick = endpoints.front();
                }
                axis_frame.origin_tick = origin_tick;
                if (domain.valid_bits == 64) {
                    axis_frame.extent_ticks = std::uint64_t{0} - largest_gap;
                } else {
                    const std::uint64_t modulus = std::uint64_t{1} << domain.valid_bits;
                    axis_frame.extent_ticks     = modulus - largest_gap;
                }
            }

            const std::uint64_t half_range = domain.valid_bits == 64 ?
                                                 (std::uint64_t{1} << 63) :
                                                 (std::uint64_t{1} << (domain.valid_bits - 1));
            timing_layout_valid            = axis_frame.extent_ticks < half_range;
            if (timing_layout_valid) {
                for (std::size_t order_index = group_begin; order_index < group_end && timing_layout_valid;
                     ++order_index) {
                    const GpuFrameSlicePlan& slice = _plan.slices[slice_order[order_index]];
                    for (std::uint64_t scope_offset = 0; scope_offset < slice.scope_count; ++scope_offset) {
                        if (MarkCancelled(_cancellation, _result)) {
                            return false;
                        }
                        const GpuScopeRecord& scope = _gpu_scopes[slice.first_scope + scope_offset];
                        if (scope.status != ProfileGpuScopeStatus::Ready) {
                            continue;
                        }
                        std::uint64_t begin_offset = 0;
                        std::uint64_t end_offset   = 0;
                        timing_layout_valid =
                            ComputeGpuOffsets(scope, domain, axis_frame, begin_offset, end_offset);
                        if (!timing_layout_valid) {
                            break;
                        }
                    }
                }
            }
        } else {
            timing_layout_valid = false;
        }

        axis_frame.timing_available          = timing_layout_valid && axis_frame.ready_scope_count != 0;
        const std::uint64_t axis_frame_index = _plan.axis_frames.size();
        for (std::size_t order_index = group_begin; order_index < group_end; ++order_index) {
            if (MarkCancelled(_cancellation, _result)) {
                return false;
            }
            GpuFrameSlicePlan& slice = _plan.slices[slice_order[order_index]];
            slice.axis_frame_index   = axis_frame_index;
            if (!axis_frame.timing_available) {
                continue;
            }
            slice.tree_leaf_count = 0;
            if (!NextPowerOfTwo(slice.ready_scope_count, slice.tree_leaf_count)) {
                _result.status = TimelineIndexBuildStatus::ResourceExhausted;
                return false;
            }
            std::uint64_t tree_values = 0;
            if (MultiplyOverflow(slice.tree_leaf_count, std::uint64_t{2}, tree_values) ||
                AddOverflow(_plan.tree_value_count, tree_values, _plan.tree_value_count) ||
                AddOverflow(
                    _plan.timeline_scope_count, slice.ready_scope_count, _plan.timeline_scope_count
                )) {
                _result.status = TimelineIndexBuildStatus::ResourceExhausted;
                return false;
            }
            if (_plan.timeline_scope_count > _limits.max_gpu_timeline_scopes) {
                _result.status     = TimelineIndexBuildStatus::LimitExceeded;
                _result.limit_kind = TimelineIndexLimitKind::GpuTimelineScopes;
                return false;
            }
        }
        _plan.axis_frames.push_back(axis_frame);
        group_begin = group_end;
    }
    if (_plan.axis_frames.size() != axis_frame_count) {
        _result.status = TimelineIndexBuildStatus::ProtocolViolation;
        return false;
    }

    if (_plan.timeline_scope_count > _limits.max_gpu_timeline_scopes) {
        _result.status     = TimelineIndexBuildStatus::LimitExceeded;
        _result.limit_kind = TimelineIndexLimitKind::GpuTimelineScopes;
        return false;
    }

    std::uint64_t tree_offset = 0;
    for (GpuFrameSlicePlan& slice : _plan.slices) {
        if (MarkCancelled(_cancellation, _result)) {
            return false;
        }
        slice.tree_offset         = tree_offset;
        std::uint64_t tree_values = 0;
        if (MultiplyOverflow(slice.tree_leaf_count, std::uint64_t{2}, tree_values) ||
            AddOverflow(tree_offset, tree_values, tree_offset)) {
            _result.status = TimelineIndexBuildStatus::ResourceExhausted;
            return false;
        }
    }
    if (tree_offset != _plan.tree_value_count) {
        _result.status = TimelineIndexBuildStatus::ProtocolViolation;
        return false;
    }
    return !MarkCancelledNow(_cancellation, _result);
}

} // namespace

struct ProfileTimelineIndex::Impl {
    TimelineIndexBuildResult result{
        .status = TimelineIndexBuildStatus::Ready,
    };
    ProfileTimelineQuality       quality{};
    const ProfileSessionSummary* session_identity{nullptr};
    std::uint64_t                session_generation{0};
    std::uint64_t                cpu_scope_count{0};
    std::uint64_t                gpu_frame_count{0};
    std::uint64_t                gpu_scope_count{0};
    std::uint32_t                cpu_track_count{0};
    std::uint32_t                gpu_track_count{0};
    std::uint32_t                gpu_domain_count{0};

    std::vector<TimelineAxis>          axes{};
    std::vector<CpuTimelineTrackIndex> cpu_tracks{};
    std::vector<CpuTimelineScopeRef>   cpu_scopes{};
    std::vector<TimelineIntervalTree>  cpu_trees{};
    std::vector<std::uint64_t>         cpu_tree_max_end{};
    std::vector<GpuTimelineFrameRef>   gpu_frames{};
    std::vector<GpuTimelineTrackIndex> gpu_tracks{};
    std::vector<GpuTimelineFrameSlice> gpu_frame_slices{};
    std::vector<GpuTimelineAxisFrame>  gpu_axis_frames{};
    std::vector<GpuTimelineScopeRef>   gpu_timeline_scopes{};
    std::vector<TimelineIntervalTree>  gpu_trees{};
    std::vector<std::uint64_t>         gpu_tree_max_end{};
};

ProfileTimelineIndex::~ProfileTimelineIndex() {
    delete impl_;
}

ProfileTimelineIndex::ProfileTimelineIndex(ProfileTimelineIndex&& _other) noexcept :
    impl_(std::exchange(_other.impl_, nullptr)) {}

ProfileTimelineIndex& ProfileTimelineIndex::operator=(ProfileTimelineIndex&& _other) noexcept {
    if (this != &_other) {
        delete impl_;
        impl_ = std::exchange(_other.impl_, nullptr);
    }
    return *this;
}

bool ProfileTimelineIndex::Valid() const noexcept {
    return impl_ != nullptr && impl_->result.status == TimelineIndexBuildStatus::Ready;
}

bool ProfileTimelineIndex::Matches(const ProfileSession& _session) const noexcept {
    if (!Valid() || !_session.Valid()) {
        return false;
    }
    const ProfileSessionSummary& summary = _session.Summary();
    return impl_->session_identity == &summary && impl_->session_generation == summary.generation &&
           impl_->cpu_scope_count == _session.CpuScopes().size() &&
           impl_->gpu_frame_count == _session.GpuFrames().size() &&
           impl_->gpu_scope_count == _session.GpuScopes().size() &&
           impl_->cpu_track_count == _session.CpuTracks().size() &&
           impl_->gpu_track_count == _session.GpuTracks().size() &&
           impl_->gpu_domain_count == _session.GpuDomains().size();
}

const TimelineIndexBuildResult& ProfileTimelineIndex::BuildResult() const noexcept {
    static const TimelineIndexBuildResult invalid{};
    return impl_ != nullptr ? impl_->result : invalid;
}

const ProfileTimelineQuality& ProfileTimelineIndex::Quality() const noexcept {
    static const ProfileTimelineQuality empty{};
    return impl_ != nullptr ? impl_->quality : empty;
}

std::span<const TimelineAxis> ProfileTimelineIndex::Axes() const noexcept {
    return Valid() ? std::span<const TimelineAxis>(impl_->axes) : std::span<const TimelineAxis>{};
}

std::span<const CpuTimelineTrackIndex> ProfileTimelineIndex::CpuTracks() const noexcept {
    return Valid() ? std::span<const CpuTimelineTrackIndex>(impl_->cpu_tracks) :
                     std::span<const CpuTimelineTrackIndex>{};
}

std::span<const CpuTimelineScopeRef> ProfileTimelineIndex::CpuScopes() const noexcept {
    return Valid() ? std::span<const CpuTimelineScopeRef>(impl_->cpu_scopes) :
                     std::span<const CpuTimelineScopeRef>{};
}

std::span<const GpuTimelineFrameRef> ProfileTimelineIndex::GpuFrames() const noexcept {
    return Valid() ? std::span<const GpuTimelineFrameRef>(impl_->gpu_frames) :
                     std::span<const GpuTimelineFrameRef>{};
}

std::span<const GpuTimelineTrackIndex> ProfileTimelineIndex::GpuTracks() const noexcept {
    return Valid() ? std::span<const GpuTimelineTrackIndex>(impl_->gpu_tracks) :
                     std::span<const GpuTimelineTrackIndex>{};
}

std::span<const GpuTimelineFrameSlice> ProfileTimelineIndex::GpuFrameSlices() const noexcept {
    return Valid() ? std::span<const GpuTimelineFrameSlice>(impl_->gpu_frame_slices) :
                     std::span<const GpuTimelineFrameSlice>{};
}

std::span<const GpuTimelineAxisFrame> ProfileTimelineIndex::GpuAxisFrames() const noexcept {
    return Valid() ? std::span<const GpuTimelineAxisFrame>(impl_->gpu_axis_frames) :
                     std::span<const GpuTimelineAxisFrame>{};
}

std::span<const GpuTimelineScopeRef> ProfileTimelineIndex::GpuTimelineScopes() const noexcept {
    return Valid() ? std::span<const GpuTimelineScopeRef>(impl_->gpu_timeline_scopes) :
                     std::span<const GpuTimelineScopeRef>{};
}

TimelineOverlapQueryResult ProfileTimelineIndex::QueryCpuOverlaps(
    std::uint32_t            _track_index,
    std::uint64_t            _view_begin_ns,
    std::uint64_t            _view_end_ns,
    std::span<std::uint64_t> _output
) const noexcept {
    if (!Valid() || _track_index >= impl_->cpu_tracks.size() || _view_end_ns <= _view_begin_ns) {
        return {};
    }

    const CpuTimelineTrackIndex& track = impl_->cpu_tracks[_track_index];
    const TimelineIntervalTree&  tree  = impl_->cpu_trees[_track_index];
    return QueryTimelineOverlaps(
        std::span<const CpuTimelineScopeRef>(impl_->cpu_scopes),
        track.first_scope,
        track.scope_count,
        tree,
        std::span<const std::uint64_t>(impl_->cpu_tree_max_end),
        _view_begin_ns,
        _view_end_ns,
        _output,
        [](const CpuTimelineScopeRef& _scope) noexcept {
            return _scope.source_scope_index;
        }
    );
}

TimelineOverlapQueryResult ProfileTimelineIndex::QueryCpuTimelineOverlaps(
    std::uint32_t                  _track_index,
    std::uint64_t                  _view_begin_ns,
    std::uint64_t                  _view_end_ns,
    std::span<CpuTimelineScopeRef> _output
) const noexcept {
    if (!Valid() || _track_index >= impl_->cpu_tracks.size() || _view_end_ns <= _view_begin_ns) {
        return {};
    }

    const CpuTimelineTrackIndex& track = impl_->cpu_tracks[_track_index];
    const TimelineIntervalTree&  tree  = impl_->cpu_trees[_track_index];
    return QueryTimelineOverlaps(
        std::span<const CpuTimelineScopeRef>(impl_->cpu_scopes),
        track.first_scope,
        track.scope_count,
        tree,
        std::span<const std::uint64_t>(impl_->cpu_tree_max_end),
        _view_begin_ns,
        _view_end_ns,
        _output,
        [](const CpuTimelineScopeRef& _scope) noexcept {
            return _scope;
        }
    );
}

const GpuTimelineFrameRef* ProfileTimelineIndex::FindGpuFrame(std::uint64_t _frame_id) const noexcept {
    if (!Valid()) {
        return nullptr;
    }
    const auto found = std::lower_bound(
        impl_->gpu_frames.begin(),
        impl_->gpu_frames.end(),
        _frame_id,
        [](const GpuTimelineFrameRef& _frame, std::uint64_t _id) {
            return _frame.frame_id < _id;
        }
    );
    return found != impl_->gpu_frames.end() && found->frame_id == _frame_id ? &*found : nullptr;
}

const GpuTimelineFrameSlice*
ProfileTimelineIndex::FindGpuFrameSlice(std::uint32_t _track_index, std::uint64_t _frame_id) const noexcept {
    if (!Valid() || _track_index >= impl_->gpu_tracks.size()) {
        return nullptr;
    }
    const GpuTimelineTrackIndex& track = impl_->gpu_tracks[_track_index];
    const auto begin = impl_->gpu_frame_slices.begin() + static_cast<std::ptrdiff_t>(track.first_frame_slice);
    const auto end   = begin + static_cast<std::ptrdiff_t>(track.frame_slice_count);
    const auto found =
        std::lower_bound(begin, end, _frame_id, [](const GpuTimelineFrameSlice& _slice, std::uint64_t _id) {
            return _slice.frame_id < _id;
        });
    return found != end && found->frame_id == _frame_id ? &*found : nullptr;
}

const GpuTimelineAxisFrame*
ProfileTimelineIndex::FindGpuAxisFrame(std::uint32_t _axis_index, std::uint64_t _frame_id) const noexcept {
    if (!Valid() || _axis_index >= impl_->axes.size()) {
        return nullptr;
    }
    const auto found = std::lower_bound(
        impl_->gpu_axis_frames.begin(),
        impl_->gpu_axis_frames.end(),
        std::pair{_axis_index, _frame_id},
        [](const GpuTimelineAxisFrame& _frame, const auto& _key) {
            return _frame.axis_index < _key.first ||
                   (_frame.axis_index == _key.first && _frame.frame_id < _key.second);
        }
    );
    return found != impl_->gpu_axis_frames.end() && found->axis_index == _axis_index &&
                   found->frame_id == _frame_id ?
               &*found :
               nullptr;
}

TimelineOverlapQueryResult ProfileTimelineIndex::QueryGpuOverlaps(
    std::uint32_t            _track_index,
    std::uint64_t            _frame_id,
    std::uint64_t            _view_begin_ticks,
    std::uint64_t            _view_end_ticks,
    std::span<std::uint64_t> _output
) const noexcept {
    if (!Valid() || _track_index >= impl_->gpu_tracks.size() || _view_end_ticks <= _view_begin_ticks) {
        return {};
    }
    const GpuTimelineFrameSlice* slice = FindGpuFrameSlice(_track_index, _frame_id);
    if (slice == nullptr || slice->axis_frame_index >= impl_->gpu_axis_frames.size() ||
        !impl_->gpu_axis_frames[slice->axis_frame_index].timing_available) {
        return {};
    }
    const std::size_t slice_index = static_cast<std::size_t>(slice - impl_->gpu_frame_slices.data());
    if (slice_index >= impl_->gpu_trees.size()) {
        return {};
    }
    const TimelineIntervalTree& tree = impl_->gpu_trees[slice_index];
    return QueryTimelineOverlaps(
        std::span<const GpuTimelineScopeRef>(impl_->gpu_timeline_scopes),
        slice->first_timeline_scope,
        slice->timeline_scope_count,
        tree,
        std::span<const std::uint64_t>(impl_->gpu_tree_max_end),
        _view_begin_ticks,
        _view_end_ticks,
        _output,
        [](const GpuTimelineScopeRef& _scope) noexcept {
            return _scope.source_scope_index;
        }
    );
}

TimelineOverlapQueryResult ProfileTimelineIndex::QueryGpuTimelineOverlaps(
    std::uint32_t                  _track_index,
    std::uint64_t                  _frame_id,
    std::uint64_t                  _view_begin_ticks,
    std::uint64_t                  _view_end_ticks,
    std::span<GpuTimelineScopeRef> _output
) const noexcept {
    if (!Valid() || _track_index >= impl_->gpu_tracks.size() || _view_end_ticks <= _view_begin_ticks) {
        return {};
    }
    const GpuTimelineFrameSlice* slice = FindGpuFrameSlice(_track_index, _frame_id);
    if (slice == nullptr || slice->axis_frame_index >= impl_->gpu_axis_frames.size() ||
        !impl_->gpu_axis_frames[slice->axis_frame_index].timing_available) {
        return {};
    }
    const std::size_t slice_index = static_cast<std::size_t>(slice - impl_->gpu_frame_slices.data());
    if (slice_index >= impl_->gpu_trees.size()) {
        return {};
    }
    const TimelineIntervalTree& tree = impl_->gpu_trees[slice_index];
    return QueryTimelineOverlaps(
        std::span<const GpuTimelineScopeRef>(impl_->gpu_timeline_scopes),
        slice->first_timeline_scope,
        slice->timeline_scope_count,
        tree,
        std::span<const std::uint64_t>(impl_->gpu_tree_max_end),
        _view_begin_ticks,
        _view_end_ticks,
        _output,
        [](const GpuTimelineScopeRef& _scope) noexcept {
            return _scope;
        }
    );
}

TimelineIndexBuildResult BuildProfileTimelineIndex(
    const ProfileSession&            _session,
    const SessionLoadResult&         _load_result,
    const TimelineIndexLimits&       _limits,
    ProfileTimelineIndex&            _output,
    const TimelineIndexBuildControl& _control
) noexcept {
    TimelineIndexBuildResult result{};
    if (!_session.Valid() || !_load_result.HasUsableSession()) {
        return result;
    }
    const ProfileSessionSummary& input_summary = _session.Summary();
    const bool                   result_matches_session =
        _load_result.packet_count == input_summary.packet_count &&
        ((_load_result.status == SessionLoadStatus::Complete) == input_summary.has_session_end);
    if (!result_matches_session) {
        return result;
    }

    CancellationProbe cancellation(_control);
    if (MarkCancelledNow(cancellation, result)) {
        return result;
    }

    try {
        const auto cpu_scopes  = _session.CpuScopes();
        const auto cpu_tracks  = _session.CpuTracks();
        const auto gpu_frames  = _session.GpuFrames();
        const auto gpu_scopes  = _session.GpuScopes();
        const auto gpu_tracks  = _session.GpuTracks();
        const auto gpu_domains = _session.GpuDomains();

        if (cpu_scopes.size() > _limits.max_cpu_scopes) {
            result.status     = TimelineIndexBuildStatus::LimitExceeded;
            result.limit_kind = TimelineIndexLimitKind::CpuScopes;
            return result;
        }

        std::uint64_t cpu_tree_value_count = 0;
        for (const CpuTrack& track : cpu_tracks) {
            if (MarkCancelled(cancellation, result)) {
                return result;
            }
            std::uint64_t leaf_count = 0;
            std::uint64_t tree_count = 0;
            if (!NextPowerOfTwo(track.scope_count, leaf_count) ||
                MultiplyOverflow(leaf_count, std::uint64_t{2}, tree_count) ||
                AddOverflow(cpu_tree_value_count, tree_count, cpu_tree_value_count)) {
                result.status = TimelineIndexBuildStatus::ResourceExhausted;
                return result;
            }
        }

        GpuBuildPlan gpu_plan;
        if (!PrepareGpuBuildPlan(
                gpu_frames, gpu_scopes, gpu_tracks, gpu_domains, _limits, cancellation, result, gpu_plan
            )) {
            return result;
        }

        const auto add_vector_bytes = [&](std::uint64_t _count, std::uint64_t _element_size) {
            std::uint64_t bytes = 0;
            return !MultiplyOverflow(_count, _element_size, bytes) &&
                   !AddOverflow(result.logical_bytes, bytes, result.logical_bytes);
        };
        if (!add_vector_bytes(1 + gpu_domains.size(), sizeof(TimelineAxis)) ||
            !add_vector_bytes(cpu_tracks.size(), sizeof(CpuTimelineTrackIndex)) ||
            !add_vector_bytes(cpu_scopes.size(), sizeof(CpuTimelineScopeRef)) ||
            !add_vector_bytes(cpu_tracks.size(), sizeof(TimelineIntervalTree)) ||
            !add_vector_bytes(cpu_tree_value_count, sizeof(std::uint64_t)) ||
            !add_vector_bytes(gpu_frames.size(), sizeof(GpuTimelineFrameRef)) ||
            !add_vector_bytes(gpu_tracks.size(), sizeof(GpuTimelineTrackIndex)) ||
            !add_vector_bytes(gpu_plan.slices.size(), sizeof(GpuTimelineFrameSlice)) ||
            !add_vector_bytes(gpu_plan.axis_frames.size(), sizeof(GpuTimelineAxisFrame)) ||
            !add_vector_bytes(gpu_plan.timeline_scope_count, sizeof(GpuTimelineScopeRef)) ||
            !add_vector_bytes(gpu_plan.slices.size(), sizeof(TimelineIntervalTree)) ||
            !add_vector_bytes(gpu_plan.tree_value_count, sizeof(std::uint64_t))) {
            result.status = TimelineIndexBuildStatus::ResourceExhausted;
            return result;
        }
        if (result.logical_bytes > _limits.max_logical_bytes) {
            result.status     = TimelineIndexBuildStatus::LimitExceeded;
            result.limit_kind = TimelineIndexLimitKind::LogicalBytes;
            return result;
        }
        if (MarkCancelledNow(cancellation, result)) {
            return result;
        }

        ProfileTimelineIndex candidate;
        candidate.impl_ = new (std::nothrow) ProfileTimelineIndex::Impl;
        if (candidate.impl_ == nullptr) {
            result.status = TimelineIndexBuildStatus::ResourceExhausted;
            return result;
        }
        ProfileTimelineIndex::Impl& impl = *candidate.impl_;
        impl.result                      = result;
        impl.result.status               = TimelineIndexBuildStatus::Ready;
        impl.session_identity            = &_session.Summary();
        impl.session_generation          = _session.Summary().generation;
        impl.cpu_scope_count             = cpu_scopes.size();
        impl.gpu_frame_count             = gpu_frames.size();
        impl.gpu_scope_count             = gpu_scopes.size();
        impl.cpu_track_count             = static_cast<std::uint32_t>(cpu_tracks.size());
        impl.gpu_track_count             = static_cast<std::uint32_t>(gpu_tracks.size());
        impl.gpu_domain_count            = static_cast<std::uint32_t>(gpu_domains.size());

        impl.axes.reserve(1 + gpu_domains.size());
        impl.cpu_tracks.reserve(cpu_tracks.size());
        impl.cpu_scopes.reserve(cpu_scopes.size());
        impl.cpu_trees.reserve(cpu_tracks.size());
        impl.cpu_tree_max_end.resize(static_cast<std::size_t>(cpu_tree_value_count), 0);
        impl.gpu_frames.reserve(gpu_frames.size());
        impl.gpu_tracks.reserve(gpu_tracks.size());
        impl.gpu_frame_slices.reserve(gpu_plan.slices.size());
        impl.gpu_axis_frames.reserve(gpu_plan.axis_frames.size());
        impl.gpu_timeline_scopes.reserve(static_cast<std::size_t>(gpu_plan.timeline_scope_count));
        impl.gpu_trees.reserve(gpu_plan.slices.size());
        impl.gpu_tree_max_end.resize(static_cast<std::size_t>(gpu_plan.tree_value_count), 0);

        const ProfileSessionSummary& summary = input_summary;
        impl.axes.push_back({
            .kind                            = TimelineAxisKind::CpuSteadyClock,
            .unit_period_ns                  = 1.0,
            .timing_available                = summary.has_cpu_range,
            .timing_capability_trusted       = summary.has_cpu_range,
            .calibrated_to_cpu               = true,
            .calibrated_to_other_gpu_domains = false,
        });
        for (std::size_t index = 0; index < gpu_domains.size(); ++index) {
            if (MarkCancelled(cancellation, result)) {
                return result;
            }
            const GpuTimestampDomain& domain = gpu_domains[index];
            impl.axes.push_back({
                .kind                            = TimelineAxisKind::GpuPhysicalTimestampDomain,
                .source_domain_index             = static_cast<std::uint32_t>(index),
                .native_queue_id                 = domain.native_queue_id,
                .family_id                       = domain.family_id,
                .logical_queue_mask              = domain.logical_queue_mask,
                .valid_bits                      = domain.valid_bits,
                .unit_period_ns                  = domain.tick_period_ns,
                .timing_available                = domain.has_ready_timestamps,
                .timing_capability_trusted       = domain.timing_capability_trusted,
                .calibrated_to_cpu               = false,
                .calibrated_to_other_gpu_domains = false,
            });
        }

        std::uint64_t tree_offset = 0;
        for (std::size_t track_index = 0; track_index < cpu_tracks.size(); ++track_index) {
            if (MarkCancelled(cancellation, result)) {
                return result;
            }
            const CpuTrack& track = cpu_tracks[track_index];
            if (track.first_scope > cpu_scopes.size() ||
                track.scope_count > cpu_scopes.size() - track.first_scope ||
                (track.scope_count != 0 && !summary.has_cpu_range)) {
                result.status = TimelineIndexBuildStatus::ProtocolViolation;
                return result;
            }

            std::uint64_t leaf_count = 0;
            if (!NextPowerOfTwo(track.scope_count, leaf_count)) {
                result.status = TimelineIndexBuildStatus::ResourceExhausted;
                return result;
            }
            impl.cpu_trees.push_back({
                .tree_offset = tree_offset,
                .leaf_count  = leaf_count,
            });
            CpuTimelineTrackIndex indexed_track{
                .source_track_index = static_cast<std::uint32_t>(track_index),
                .axis_index         = 0,
                .first_scope        = impl.cpu_scopes.size(),
                .scope_count        = track.scope_count,
            };

            std::uint64_t     previous_begin = 0;
            bool              has_scope      = false;
            const std::size_t source_begin   = static_cast<std::size_t>(track.first_scope);
            const std::size_t source_end     = source_begin + static_cast<std::size_t>(track.scope_count);
            for (std::size_t source_index = source_begin; source_index < source_end; ++source_index) {
                if (MarkCancelled(cancellation, result)) {
                    return result;
                }
                const CpuScopeRecord& scope = cpu_scopes[source_index];
                if (scope.track_index != track_index || scope.thread_id != track.thread_id ||
                    scope.begin_ns < summary.cpu_begin_ns || scope.end_ns < scope.begin_ns) {
                    result.status = TimelineIndexBuildStatus::ProtocolViolation;
                    return result;
                }
                const std::uint64_t begin_ns = scope.begin_ns - summary.cpu_begin_ns;
                const std::uint64_t end_ns   = scope.end_ns - summary.cpu_begin_ns;
                if (has_scope && begin_ns < previous_begin) {
                    result.status = TimelineIndexBuildStatus::ProtocolViolation;
                    return result;
                }
                previous_begin = begin_ns;
                has_scope      = true;
                impl.cpu_scopes.push_back({
                    .source_scope_index = source_index,
                    .begin_ns           = begin_ns,
                    .end_ns             = end_ns,
                });
                indexed_track.max_depth = std::max(indexed_track.max_depth, scope.depth);
                if (indexed_track.scope_count != 0) {
                    if (source_index == source_begin) {
                        indexed_track.begin_ns = begin_ns;
                        indexed_track.end_ns   = end_ns;
                    } else {
                        indexed_track.end_ns = std::max(indexed_track.end_ns, end_ns);
                    }
                }
            }
            impl.cpu_tracks.push_back(indexed_track);

            const std::uint64_t track_tree_offset = tree_offset;
            for (std::uint64_t local_index = 0; local_index < track.scope_count; ++local_index) {
                if (MarkCancelled(cancellation, result)) {
                    return result;
                }
                const CpuTimelineScopeRef& scope = impl.cpu_scopes[indexed_track.first_scope + local_index];
                impl.cpu_tree_max_end[track_tree_offset + leaf_count + local_index] = QueryEnd(scope);
            }
            for (std::uint64_t node = leaf_count; node > 1;) {
                if (MarkCancelled(cancellation, result)) {
                    return result;
                }
                --node;
                impl.cpu_tree_max_end[track_tree_offset + node] = std::max(
                    impl.cpu_tree_max_end[track_tree_offset + node * 2],
                    impl.cpu_tree_max_end[track_tree_offset + node * 2 + 1]
                );
            }
            tree_offset += leaf_count * 2;
        }
        if (tree_offset != cpu_tree_value_count) {
            result.status = TimelineIndexBuildStatus::ProtocolViolation;
            return result;
        }

        for (std::size_t index = 0; index < gpu_frames.size(); ++index) {
            if (MarkCancelled(cancellation, result)) {
                return result;
            }
            if (index != 0 && gpu_frames[index - 1].frame_id >= gpu_frames[index].frame_id) {
                result.status = TimelineIndexBuildStatus::ProtocolViolation;
                return result;
            }
            impl.gpu_frames.push_back({
                .frame_id           = gpu_frames[index].frame_id,
                .source_frame_index = index,
            });
        }

        for (const GpuAxisFramePlan& planned : gpu_plan.axis_frames) {
            if (MarkCancelled(cancellation, result) || planned.domain_index >= gpu_domains.size()) {
                if (result.status != TimelineIndexBuildStatus::Cancelled) {
                    result.status = TimelineIndexBuildStatus::ProtocolViolation;
                }
                return result;
            }
            const GpuTimestampDomain& domain = gpu_domains[planned.domain_index];
            impl.gpu_axis_frames.push_back({
                .frame_id                  = planned.frame_id,
                .source_frame_index        = planned.source_frame_index,
                .axis_index                = static_cast<std::uint32_t>(1 + planned.domain_index),
                .origin_tick               = planned.origin_tick,
                .extent_ticks              = planned.extent_ticks,
                .ready_scope_count         = planned.ready_scope_count,
                .error_scope_count         = planned.error_scope_count,
                .frame_status              = planned.frame_status,
                .has_frame_record          = planned.has_frame_record,
                .timing_available          = planned.timing_available,
                .timing_capability_trusted = domain.timing_capability_trusted,
                .materialization_complete  = planned.materialization_complete,
                .timing_topology_trusted   = planned.timing_topology_trusted,
            });
        }

        for (std::size_t track_index = 0; track_index < gpu_tracks.size(); ++track_index) {
            if (MarkCancelled(cancellation, result)) {
                return result;
            }
            const GpuTrack& track = gpu_tracks[track_index];
            if (track_index >= gpu_plan.track_first_slice.size() ||
                track_index >= gpu_plan.track_slice_count.size() ||
                track.domain_index >= gpu_domains.size()) {
                result.status = TimelineIndexBuildStatus::ProtocolViolation;
                return result;
            }
            GpuTimelineTrackIndex indexed_track{
                .source_track_index = static_cast<std::uint32_t>(track_index),
                .axis_index         = static_cast<std::uint32_t>(1 + track.domain_index),
                .first_frame_slice  = impl.gpu_frame_slices.size(),
                .frame_slice_count  = gpu_plan.track_slice_count[track_index],
            };
            const std::uint64_t plan_begin = gpu_plan.track_first_slice[track_index];
            const std::uint64_t plan_end   = plan_begin + gpu_plan.track_slice_count[track_index];
            if (plan_end > gpu_plan.slices.size()) {
                result.status = TimelineIndexBuildStatus::ProtocolViolation;
                return result;
            }

            for (std::uint64_t plan_index = plan_begin; plan_index < plan_end; ++plan_index) {
                if (MarkCancelled(cancellation, result)) {
                    return result;
                }
                const GpuFrameSlicePlan& planned = gpu_plan.slices[plan_index];
                if (planned.track_index != track_index || planned.domain_index != track.domain_index ||
                    planned.axis_frame_index >= impl.gpu_axis_frames.size()) {
                    result.status = TimelineIndexBuildStatus::ProtocolViolation;
                    return result;
                }
                const GpuTimelineAxisFrame& axis_frame = impl.gpu_axis_frames[planned.axis_frame_index];
                const GpuAxisFramePlan&     axis_plan  = gpu_plan.axis_frames[planned.axis_frame_index];
                GpuTimelineFrameSlice       slice{
                          .frame_id             = planned.frame_id,
                          .source_frame_index   = planned.source_frame_index,
                          .axis_frame_index     = planned.axis_frame_index,
                          .first_scope          = planned.first_scope,
                          .scope_count          = planned.scope_count,
                          .first_timeline_scope = impl.gpu_timeline_scopes.size(),
                          .error_scope_count    = planned.error_scope_count,
                };
                if (planned.source_frame_index != kInvalidSessionIndex) {
                    const GpuFrameRecord& frame    = gpu_frames[planned.source_frame_index];
                    slice.frame_status             = frame.status;
                    slice.has_frame_record         = true;
                    slice.materialization_complete = frame.materialization_complete;
                    slice.timing_topology_trusted  = frame.timing_topology_trusted;
                }

                if (axis_frame.timing_available) {
                    const GpuTimestampDomain& domain = gpu_domains[planned.domain_index];
                    for (std::uint64_t scope_offset = 0; scope_offset < planned.scope_count; ++scope_offset) {
                        if (MarkCancelled(cancellation, result)) {
                            return result;
                        }
                        const std::uint64_t   source_index = planned.first_scope + scope_offset;
                        const GpuScopeRecord& scope        = gpu_scopes[source_index];
                        if (scope.status != ProfileGpuScopeStatus::Ready) {
                            continue;
                        }
                        std::uint64_t begin_offset = 0;
                        std::uint64_t end_offset   = 0;
                        if (!ComputeGpuOffsets(scope, domain, axis_plan, begin_offset, end_offset)) {
                            result.status = TimelineIndexBuildStatus::ProtocolViolation;
                            return result;
                        }
                        impl.gpu_timeline_scopes.push_back({
                            .source_scope_index = source_index,
                            .begin_tick_offset  = begin_offset,
                            .end_tick_offset    = end_offset,
                        });
                    }
                    slice.timeline_scope_count = impl.gpu_timeline_scopes.size() - slice.first_timeline_scope;
                    if (slice.timeline_scope_count != planned.ready_scope_count) {
                        result.status = TimelineIndexBuildStatus::ProtocolViolation;
                        return result;
                    }
                    if (!CancellableSort(
                            impl.gpu_timeline_scopes,
                            static_cast<std::size_t>(slice.first_timeline_scope),
                            static_cast<std::size_t>(slice.timeline_scope_count),
                            [](const GpuTimelineScopeRef& _left, const GpuTimelineScopeRef& _right) {
                                if (_left.begin_tick_offset != _right.begin_tick_offset) {
                                    return _left.begin_tick_offset < _right.begin_tick_offset;
                                }
                                if (_left.end_tick_offset != _right.end_tick_offset) {
                                    return _left.end_tick_offset > _right.end_tick_offset;
                                }
                                return _left.source_scope_index < _right.source_scope_index;
                            },
                            cancellation,
                            result
                        )) {
                        return result;
                    }
                }

                impl.gpu_trees.push_back({
                    .tree_offset = planned.tree_offset,
                    .leaf_count  = planned.tree_leaf_count,
                });
                if (planned.tree_leaf_count != 0) {
                    for (std::uint64_t local_index = 0; local_index < slice.timeline_scope_count;
                         ++local_index) {
                        if (MarkCancelled(cancellation, result)) {
                            return result;
                        }
                        const GpuTimelineScopeRef& scope =
                            impl.gpu_timeline_scopes[slice.first_timeline_scope + local_index];
                        impl.gpu_tree_max_end[planned.tree_offset + planned.tree_leaf_count + local_index] =
                            QueryEnd(scope);
                    }
                    for (std::uint64_t node = planned.tree_leaf_count; node > 1;) {
                        if (MarkCancelled(cancellation, result)) {
                            return result;
                        }
                        --node;
                        impl.gpu_tree_max_end[planned.tree_offset + node] = std::max(
                            impl.gpu_tree_max_end[planned.tree_offset + node * 2],
                            impl.gpu_tree_max_end[planned.tree_offset + node * 2 + 1]
                        );
                    }
                }
                impl.gpu_frame_slices.push_back(slice);
            }
            if (impl.gpu_frame_slices.size() - indexed_track.first_frame_slice !=
                indexed_track.frame_slice_count) {
                result.status = TimelineIndexBuildStatus::ProtocolViolation;
                return result;
            }
            impl.gpu_tracks.push_back(indexed_track);
        }

        impl.quality.lost_record_count                 = summary.lost_record_count;
        impl.quality.unnotified_drop_count             = summary.unnotified_drop_count;
        impl.quality.orphan_cpu_scope_count            = summary.orphan_cpu_scope_count;
        impl.quality.orphan_gpu_scope_count            = summary.orphan_gpu_scope_count;
        impl.quality.degraded_complete_gpu_frame_count = summary.degraded_complete_gpu_frame_count;
        impl.quality.incomplete_gpu_frame_count        = summary.incomplete_gpu_frame_count;
        impl.quality.invalid_gpu_frame_count           = summary.invalid_gpu_frame_count;
        impl.quality.error_gpu_scope_count             = summary.error_gpu_scope_count;
        impl.quality.unknown_record_count              = summary.unknown_record_count;
        for (const GpuFrameRecord& frame : gpu_frames) {
            if (MarkCancelled(cancellation, result)) {
                return result;
            }
            if (frame.scope_count != 0 && !frame.timing_topology_trusted) {
                ++impl.quality.untrusted_gpu_frame_count;
            }
        }

        if (_load_result.status == SessionLoadStatus::ForensicTruncated) {
            AddQualityFlag(impl.quality, TimelineQualityFlag::ForensicTruncated);
        }
        if (impl.quality.lost_record_count != 0) {
            AddQualityFlag(impl.quality, TimelineQualityFlag::LostRecords);
        }
        if (impl.quality.unnotified_drop_count != 0) {
            AddQualityFlag(impl.quality, TimelineQualityFlag::UnnotifiedDrops);
        }
        if (impl.quality.orphan_cpu_scope_count != 0) {
            AddQualityFlag(impl.quality, TimelineQualityFlag::CpuOrphans);
        }
        if (impl.quality.orphan_gpu_scope_count != 0) {
            AddQualityFlag(impl.quality, TimelineQualityFlag::GpuOrphans);
        }
        if (impl.quality.degraded_complete_gpu_frame_count != 0) {
            AddQualityFlag(impl.quality, TimelineQualityFlag::DegradedGpuFrames);
        }
        if (impl.quality.incomplete_gpu_frame_count != 0) {
            AddQualityFlag(impl.quality, TimelineQualityFlag::IncompleteGpuFrames);
        }
        if (impl.quality.invalid_gpu_frame_count != 0) {
            AddQualityFlag(impl.quality, TimelineQualityFlag::InvalidGpuFrames);
        }
        if (impl.quality.error_gpu_scope_count != 0) {
            AddQualityFlag(impl.quality, TimelineQualityFlag::GpuScopeErrors);
        }
        if (impl.quality.untrusted_gpu_frame_count != 0) {
            AddQualityFlag(impl.quality, TimelineQualityFlag::UntrustedGpuTiming);
        }
        if (impl.quality.unknown_record_count != 0) {
            AddQualityFlag(impl.quality, TimelineQualityFlag::UnknownRecords);
        }

        if (MarkCancelledNow(cancellation, result)) {
            return result;
        }
        impl.result        = result;
        impl.result.status = TimelineIndexBuildStatus::Ready;
        _output            = std::move(candidate);
        result.status      = TimelineIndexBuildStatus::Ready;
        return result;
    } catch (const std::bad_alloc&) {
        result.status = TimelineIndexBuildStatus::ResourceExhausted;
        return result;
    } catch (...) {
        result.status = TimelineIndexBuildStatus::ResourceExhausted;
        return result;
    }
}

} // namespace Moer::ProfileDump
