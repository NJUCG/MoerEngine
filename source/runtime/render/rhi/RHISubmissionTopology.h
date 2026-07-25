#pragma once

#include "rhi/RHICommon.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Moer::Render {

// Translation is parallel by default. SerialControl is an explicit CPU-side
// scheduling barrier; it does not change the queue on which GPU work executes.
enum class ERHITranslateExecutionClass : uint8_t {
    Parallel      = 0,
    SerialControl = 1,
};

struct RHISubmitSegment {
    EQueueType queue{EQueueType::Ignore};
    size_t     begin{0};
    size_t     end{0};

    [[nodiscard]] constexpr bool IsEmpty() const noexcept {
        return begin == end;
    }
};

struct SourceSubmitKey {
    uint64_t op_seq{0};
    uint32_t submit_idx{0};

    friend constexpr bool operator==(const SourceSubmitKey&, const SourceSubmitKey&) = default;

    friend constexpr bool operator<(const SourceSubmitKey& _lhs, const SourceSubmitKey& _rhs) {
        return _lhs.op_seq < _rhs.op_seq ||
               (_lhs.op_seq == _rhs.op_seq && _lhs.submit_idx < _rhs.submit_idx);
    }
};

struct SubmissionKey {
    uint64_t op_seq{0};
    uint32_t submit_idx{0};

    friend constexpr bool operator==(const SubmissionKey&, const SubmissionKey&) = default;

    friend constexpr bool operator<(const SubmissionKey& _lhs, const SubmissionKey& _rhs) {
        return _lhs.op_seq < _rhs.op_seq ||
               (_lhs.op_seq == _rhs.op_seq && _lhs.submit_idx < _rhs.submit_idx);
    }
};

struct SourceSubmitKeyHash {
    size_t operator()(const SourceSubmitKey& _key) const noexcept {
        size_t hash = std::hash<uint64_t>{}(_key.op_seq);
        hash ^= std::hash<uint32_t>{}(_key.submit_idx) + 0x9e3779b9u + (hash << 6u) +
                (hash >> 2u);
        return hash;
    }
};

struct SubmissionKeyHash {
    size_t operator()(const SubmissionKey& _key) const noexcept {
        size_t hash = std::hash<uint64_t>{}(_key.op_seq);
        hash ^= std::hash<uint32_t>{}(_key.submit_idx) + 0x9e3779b9u + (hash << 6u) +
                (hash >> 2u);
        return hash;
    }
};

// This deliberately contains no RHI resource ownership. It is the immutable
// description consumed by the CPU topology planner before backend translation.
struct RHISourceSubmitDescription {
    EQueueType                       root_queue{EQueueType::Ignore};
    size_t                           command_count{0};
    std::span<const RHISubmitSegment> segments{};
    ERHITranslateExecutionClass      execution_class{
        ERHITranslateExecutionClass::Parallel
    };
    bool has_side_effects{false};
};

enum class ERHISubmissionTopologyError : uint8_t {
    None = 0,
    InvalidRootQueue,
    InvalidExecutionClass,
    InvalidSegmentQueue,
    InvalidSegmentRange,
    NonContiguousSegmentCoverage,
    IndexOverflow,
};

struct RHISubmissionSegmentPlan {
    SubmissionKey               key{};
    SourceSubmitKey             source_key{};
    uint32_t                    source_segment_index{0};
    RHISubmitSegment            segment{};
    ERHITranslateExecutionClass execution_class{
        ERHITranslateExecutionClass::Parallel
    };

    bool inherit_source_wait_events{false};
    bool inherit_source_signal_events_and_callbacks{false};
    bool inherit_source_runtime_payload{false};

    // Logical CPU translation prerequisites, in source order. GPU queue and
    // resource dependencies are layered on by the backend preprocessor.
    std::vector<SubmissionKey> dependencies{};
};

struct RHISourceSubmitPlan {
    SourceSubmitKey             source_key{};
    EQueueType                  root_queue{EQueueType::Ignore};
    size_t                      command_count{0};
    ERHITranslateExecutionClass execution_class{
        ERHITranslateExecutionClass::Parallel
    };
    bool   has_side_effects{false};
    size_t segment_plan_begin{0};
    size_t segment_plan_count{0};
};

struct RHISubmissionTopologyPlan {
    static constexpr size_t NoErrorIndex = std::numeric_limits<size_t>::max();

    ERHISubmissionTopologyError error{ERHISubmissionTopologyError::None};
    size_t error_source_index{NoErrorIndex};
    size_t error_segment_index{NoErrorIndex};

    std::vector<RHISubmissionSegmentPlan> segments{};
    std::vector<RHISourceSubmitPlan>      source_plans{};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return error == ERHISubmissionTopologyError::None;
    }
};

namespace RHISubmissionTopologyDetail {

[[nodiscard]] inline constexpr bool IsValidQueue(EQueueType _queue) noexcept {
    return _queue == EQueueType::Graphics || _queue == EQueueType::Compute ||
           _queue == EQueueType::Copy;
}

[[nodiscard]] inline constexpr bool
IsValidExecutionClass(ERHITranslateExecutionClass _execution_class) noexcept {
    return _execution_class == ERHITranslateExecutionClass::Parallel ||
           _execution_class == ERHITranslateExecutionClass::SerialControl;
}

inline void AppendUnique(
    std::vector<SubmissionKey>& _dependencies,
    const SubmissionKey&        _key
) {
    for (const SubmissionKey& dependency : _dependencies) {
        if (dependency == _key) {
            return;
        }
    }
    _dependencies.emplace_back(_key);
}

[[nodiscard]] inline RHISubmissionTopologyPlan Fail(
    ERHISubmissionTopologyError _error,
    size_t                      _source_index,
    size_t                      _segment_index = RHISubmissionTopologyPlan::NoErrorIndex
) {
    RHISubmissionTopologyPlan result{};
    result.error               = _error;
    result.error_source_index  = _source_index;
    result.error_segment_index = _segment_index;
    return result;
}

} // namespace RHISubmissionTopologyDetail

// All source descriptions belong to the operation identified by _op_seq_base.
// Source keys use the input index; translated submission keys use one stable,
// monotonically increasing index across every retained segment.
[[nodiscard]] inline RHISubmissionTopologyPlan BuildRHISubmissionTopology(
    std::span<const RHISourceSubmitDescription> _descriptions,
    uint64_t                                    _op_seq_base
) {
    using namespace RHISubmissionTopologyDetail;

    if (_descriptions.size() > std::numeric_limits<uint32_t>::max()) {
        return Fail(ERHISubmissionTopologyError::IndexOverflow, 0);
    }

    RHISubmissionTopologyPlan result{};
    result.source_plans.reserve(_descriptions.size());

    std::vector<SubmissionKey> parallel_frontier{};
    std::optional<SubmissionKey> last_serial_control{};

    for (size_t source_index = 0; source_index < _descriptions.size(); ++source_index) {
        const RHISourceSubmitDescription& description = _descriptions[source_index];
        if (!IsValidQueue(description.root_queue)) {
            return Fail(ERHISubmissionTopologyError::InvalidRootQueue, source_index);
        }
        if (!IsValidExecutionClass(description.execution_class)) {
            return Fail(ERHISubmissionTopologyError::InvalidExecutionClass, source_index);
        }

        const RHISubmitSegment synthesized_segment{
            .queue = description.root_queue,
            .begin = 0,
            .end   = description.command_count,
        };
        const std::span<const RHISubmitSegment> source_segments = description.segments.empty()
            ? std::span<const RHISubmitSegment>(&synthesized_segment, 1)
            : description.segments;

        if (source_segments.size() > std::numeric_limits<uint32_t>::max()) {
            return Fail(ERHISubmissionTopologyError::IndexOverflow, source_index);
        }

        size_t expected_begin = 0;
        for (size_t segment_index = 0; segment_index < source_segments.size(); ++segment_index) {
            const RHISubmitSegment& segment = source_segments[segment_index];
            if (!IsValidQueue(segment.queue)) {
                return Fail(
                    ERHISubmissionTopologyError::InvalidSegmentQueue,
                    source_index,
                    segment_index
                );
            }
            if (segment.begin > segment.end || segment.end > description.command_count ||
                (description.command_count != 0 && segment.IsEmpty()) ||
                (description.command_count == 0 && source_segments.size() != 1)) {
                return Fail(
                    ERHISubmissionTopologyError::InvalidSegmentRange,
                    source_index,
                    segment_index
                );
            }
            if (segment.begin != expected_begin) {
                return Fail(
                    ERHISubmissionTopologyError::NonContiguousSegmentCoverage,
                    source_index,
                    segment_index
                );
            }
            expected_begin = segment.end;
        }
        if (expected_begin != description.command_count) {
            return Fail(
                ERHISubmissionTopologyError::NonContiguousSegmentCoverage,
                source_index,
                source_segments.size()
            );
        }

        RHISourceSubmitPlan source_plan{
            .source_key = SourceSubmitKey{
                _op_seq_base,
                static_cast<uint32_t>(source_index),
            },
            .root_queue       = description.root_queue,
            .command_count    = description.command_count,
            .execution_class = description.execution_class,
            .has_side_effects = description.has_side_effects,
            .segment_plan_begin = result.segments.size(),
            .segment_plan_count = 0,
        };

        const bool retain_empty_submit =
            description.command_count == 0 && description.has_side_effects;
        const bool retain_segments = description.command_count != 0 || retain_empty_submit;
        if (retain_segments) {
            if (result.segments.size() > std::numeric_limits<uint32_t>::max() -
                    source_segments.size()) {
                return Fail(ERHISubmissionTopologyError::IndexOverflow, source_index);
            }

            for (size_t segment_index = 0; segment_index < source_segments.size(); ++segment_index) {
                const SubmissionKey key{
                    _op_seq_base,
                    static_cast<uint32_t>(result.segments.size()),
                };
                RHISubmissionSegmentPlan segment_plan{
                    .key                  = key,
                    .source_key           = source_plan.source_key,
                    .source_segment_index = static_cast<uint32_t>(segment_index),
                    .segment              = source_segments[segment_index],
                    .execution_class      = description.execution_class,
                };

                if (description.execution_class ==
                    ERHITranslateExecutionClass::SerialControl) {
                    // Older keys are appended first so dependency order remains
                    // deterministic and monotonic for an identical input stream.
                    if (last_serial_control.has_value()) {
                        AppendUnique(segment_plan.dependencies, *last_serial_control);
                    }
                    for (const SubmissionKey& frontier_key : parallel_frontier) {
                        AppendUnique(segment_plan.dependencies, frontier_key);
                    }
                    parallel_frontier.clear();
                    last_serial_control = key;
                } else {
                    if (last_serial_control.has_value()) {
                        AppendUnique(segment_plan.dependencies, *last_serial_control);
                    }
                    parallel_frontier.emplace_back(key);
                }

                result.segments.emplace_back(std::move(segment_plan));
            }

            source_plan.segment_plan_count = result.segments.size() - source_plan.segment_plan_begin;
            RHISubmissionSegmentPlan& first = result.segments[source_plan.segment_plan_begin];
            RHISubmissionSegmentPlan& last  = result.segments.back();
            first.inherit_source_wait_events = true;
            last.inherit_source_signal_events_and_callbacks = true;
            // Copy translation does not own the source-level runtime payload
            // (profiling/control state and other non-command metadata). Keep
            // signals/callbacks on the true tail, but route runtime ownership
            // to the last retained non-Copy segment. A pure Copy source has no
            // runtime-payload owner.
            for (size_t segment_plan_index = result.segments.size();
                 segment_plan_index > source_plan.segment_plan_begin;
                 --segment_plan_index) {
                RHISubmissionSegmentPlan& candidate = result.segments[segment_plan_index - 1];
                if (candidate.segment.queue != EQueueType::Copy) {
                    candidate.inherit_source_runtime_payload = true;
                    break;
                }
            }
        }

        result.source_plans.emplace_back(std::move(source_plan));
    }

    return result;
}

} // namespace Moer::Render
