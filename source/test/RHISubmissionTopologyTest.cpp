#include "rhi/RHISubmissionTopology.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Moer::Render;

namespace {

void Expect(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

RHISourceSubmitDescription Describe(
    EQueueType                              _root_queue,
    size_t                                  _command_count,
    std::span<const RHISubmitSegment>       _segments = {},
    ERHITranslateExecutionClass             _execution_class =
        ERHITranslateExecutionClass::Parallel,
    bool                                    _has_side_effects = false
) {
    return {
        .root_queue      = _root_queue,
        .command_count   = _command_count,
        .segments        = _segments,
        .execution_class = _execution_class,
        .has_side_effects = _has_side_effects,
    };
}

void StableInputOrderAndKeysArePreserved() {
    constexpr std::array first_segments{
        RHISubmitSegment{EQueueType::Graphics, 0, 2},
        RHISubmitSegment{EQueueType::Compute, 2, 4},
    };
    const std::array descriptions{
        Describe(EQueueType::Graphics, 4, first_segments),
        Describe(EQueueType::Copy, 1),
    };

    const RHISubmissionTopologyPlan first = BuildRHISubmissionTopology(descriptions, 42);
    const RHISubmissionTopologyPlan second = BuildRHISubmissionTopology(descriptions, 42);
    Expect(first.IsValid() && second.IsValid(), "stable topology input failed to build");
    Expect(first.source_plans.size() == 2 && first.segments.size() == 3,
           "topology lost a source or segment");

    for (size_t index = 0; index < first.segments.size(); ++index) {
        const RHISubmissionSegmentPlan& lhs = first.segments[index];
        const RHISubmissionSegmentPlan& rhs = second.segments[index];
        Expect(lhs.key == rhs.key && lhs.source_key == rhs.source_key &&
                   lhs.segment.queue == rhs.segment.queue &&
                   lhs.segment.begin == rhs.segment.begin && lhs.segment.end == rhs.segment.end,
               "identical topology input produced unstable output");
        Expect(lhs.key == SubmissionKey{42, static_cast<uint32_t>(index)},
               "submission keys were not monotonic and unique");
        if (index != 0) {
            Expect(first.segments[index - 1].key < lhs.key,
                   "submission keys did not increase in source order");
        }
    }

    Expect(first.segments[0].source_key == SourceSubmitKey{42, 0} &&
               first.segments[1].source_key == SourceSubmitKey{42, 0} &&
               first.segments[2].source_key == SourceSubmitKey{42, 1},
           "source-submit identity was not preserved through flattening");
    Expect(first.source_plans[0].segment_plan_begin == 0 &&
               first.source_plans[0].segment_plan_count == 2 &&
               first.source_plans[1].segment_plan_begin == 2 &&
               first.source_plans[1].segment_plan_count == 1,
           "source plan did not retain its stable flat segment range");
}

void CoverageAndBoundaryMetadataAreExplicit() {
    constexpr std::array segments{
        RHISubmitSegment{EQueueType::Graphics, 0, 2},
        RHISubmitSegment{EQueueType::Compute, 2, 5},
    };
    const std::array descriptions{
        Describe(EQueueType::Graphics, 5, segments),
    };

    const RHISubmissionTopologyPlan plan = BuildRHISubmissionTopology(descriptions, 7);
    Expect(plan.IsValid() && plan.segments.size() == 2, "covered source did not build");
    const RHISubmissionSegmentPlan& first = plan.segments.front();
    const RHISubmissionSegmentPlan& last  = plan.segments.back();
    Expect(first.segment.queue == EQueueType::Graphics && first.segment.begin == 0 &&
               first.segment.end == 2 && last.segment.queue == EQueueType::Compute &&
               last.segment.begin == 2 && last.segment.end == 5,
           "explicit segment coverage changed during planning");
    Expect(first.inherit_source_wait_events &&
               !first.inherit_source_signal_events_and_callbacks &&
               !first.inherit_source_runtime_payload,
           "source wait metadata was not isolated to the first segment");
    Expect(!last.inherit_source_wait_events &&
               last.inherit_source_signal_events_and_callbacks &&
               last.inherit_source_runtime_payload,
           "source completion metadata was not isolated to the last segment");

    const std::array synthesized_descriptions{
        Describe(EQueueType::Copy, 3),
    };
    const RHISubmissionTopologyPlan synthesized =
        BuildRHISubmissionTopology(synthesized_descriptions, 8);
    Expect(synthesized.IsValid() && synthesized.segments.size() == 1,
           "missing segment list was not synthesized");
    Expect(synthesized.segments[0].segment.queue == EQueueType::Copy &&
               synthesized.segments[0].segment.begin == 0 &&
               synthesized.segments[0].segment.end == 3,
           "synthesized segment did not cover the complete source submit");
    Expect(synthesized.segments[0].inherit_source_wait_events &&
               synthesized.segments[0].inherit_source_signal_events_and_callbacks &&
               !synthesized.segments[0].inherit_source_runtime_payload,
           "pure Copy source unexpectedly inherited runtime payload");
}

void RuntimePayloadSkipsCopySegments() {
    constexpr std::array mixed_segments{
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Copy, 1, 2},
        RHISubmitSegment{EQueueType::Graphics, 2, 3},
    };
    const std::array mixed_descriptions{
        Describe(EQueueType::Graphics, 3, mixed_segments),
    };
    const RHISubmissionTopologyPlan mixed =
        BuildRHISubmissionTopology(mixed_descriptions, 9);
    Expect(mixed.IsValid() && mixed.segments.size() == 3,
           "mixed Graphics/Copy/Graphics source did not build");
    Expect(mixed.segments[0].inherit_source_wait_events &&
               !mixed.segments[0].inherit_source_runtime_payload &&
               !mixed.segments[1].inherit_source_wait_events &&
               !mixed.segments[1].inherit_source_runtime_payload &&
               !mixed.segments[2].inherit_source_wait_events &&
               mixed.segments[2].inherit_source_signal_events_and_callbacks &&
               mixed.segments[2].inherit_source_runtime_payload,
           "mixed source boundary metadata selected the wrong runtime owner");

    constexpr std::array copy_tail_segments{
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Copy, 1, 2},
    };
    const std::array copy_tail_descriptions{
        Describe(EQueueType::Graphics, 2, copy_tail_segments),
    };
    const RHISubmissionTopologyPlan copy_tail =
        BuildRHISubmissionTopology(copy_tail_descriptions, 10);
    Expect(copy_tail.IsValid() && copy_tail.segments.size() == 2,
           "Graphics/Copy source did not build");
    Expect(copy_tail.segments[0].inherit_source_wait_events &&
               copy_tail.segments[0].inherit_source_runtime_payload &&
               !copy_tail.segments[0].inherit_source_signal_events_and_callbacks &&
               !copy_tail.segments[1].inherit_source_runtime_payload &&
               copy_tail.segments[1].inherit_source_signal_events_and_callbacks,
           "Copy tail incorrectly displaced the non-Copy runtime owner");

    constexpr std::array pure_copy_segments{
        RHISubmitSegment{EQueueType::Copy, 0, 1},
        RHISubmitSegment{EQueueType::Copy, 1, 2},
    };
    const std::array pure_copy_descriptions{
        Describe(EQueueType::Copy, 2, pure_copy_segments),
    };
    const RHISubmissionTopologyPlan pure_copy =
        BuildRHISubmissionTopology(pure_copy_descriptions, 11);
    Expect(pure_copy.IsValid() && pure_copy.segments.size() == 2,
           "pure Copy source did not build");
    Expect(pure_copy.segments[0].inherit_source_wait_events &&
               !pure_copy.segments[0].inherit_source_runtime_payload &&
               !pure_copy.segments[1].inherit_source_runtime_payload &&
               pure_copy.segments[1].inherit_source_signal_events_and_callbacks,
           "pure Copy source unexpectedly selected a runtime owner");
}

void SerialControlCreatesStableFrontierBarriers() {
    constexpr std::array two_segments{
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Compute, 1, 2},
    };
    const std::array descriptions{
        Describe(EQueueType::Graphics, 1),                         // key 0, parallel
        Describe(EQueueType::Graphics, 2, two_segments),           // keys 1,2, parallel
        Describe(EQueueType::Graphics, 1, {},
                 ERHITranslateExecutionClass::SerialControl),      // key 3
        Describe(EQueueType::Compute, 1),                          // key 4
        Describe(EQueueType::Copy, 1),                             // key 5
        Describe(EQueueType::Graphics, 1, {},
                 ERHITranslateExecutionClass::SerialControl),      // key 6
        Describe(EQueueType::Graphics, 1),                         // key 7
    };

    const RHISubmissionTopologyPlan plan = BuildRHISubmissionTopology(descriptions, 11);
    Expect(plan.IsValid() && plan.segments.size() == 8, "serial-control topology failed");
    const auto expect_dependencies = [&](size_t _segment, std::initializer_list<uint32_t> _indices) {
        const std::vector<SubmissionKey>& dependencies = plan.segments[_segment].dependencies;
        Expect(dependencies.size() == _indices.size(), "dependency count was incorrect");
        size_t dependency_index = 0;
        for (uint32_t index : _indices) {
            Expect(dependencies[dependency_index++] == SubmissionKey{11, index},
                   "dependency order or identity was incorrect");
        }
    };

    expect_dependencies(0, {});
    expect_dependencies(1, {});
    expect_dependencies(2, {});
    expect_dependencies(3, {0, 1, 2});
    expect_dependencies(4, {3});
    expect_dependencies(5, {3});
    // The previous serial control is retained explicitly even though both
    // frontier entries already depend on it.
    expect_dependencies(6, {3, 4, 5});
    expect_dependencies(7, {6});
}

void InvalidRangesAndQueuesFailClosed() {
    constexpr std::array gap_segments{
        RHISubmitSegment{EQueueType::Graphics, 0, 1},
        RHISubmitSegment{EQueueType::Graphics, 2, 3},
    };
    const std::array gap_description{
        Describe(EQueueType::Graphics, 3, gap_segments),
    };
    const RHISubmissionTopologyPlan gap = BuildRHISubmissionTopology(gap_description, 1);
    Expect(gap.error == ERHISubmissionTopologyError::NonContiguousSegmentCoverage &&
               gap.error_source_index == 0 && gap.error_segment_index == 1 &&
               gap.segments.empty() && gap.source_plans.empty(),
           "segment gap did not fail closed with a precise error");

    constexpr std::array bad_range_segments{
        RHISubmitSegment{EQueueType::Graphics, 0, 4},
    };
    const std::array bad_range_description{
        Describe(EQueueType::Graphics, 3, bad_range_segments),
    };
    const RHISubmissionTopologyPlan bad_range =
        BuildRHISubmissionTopology(bad_range_description, 1);
    Expect(bad_range.error == ERHISubmissionTopologyError::InvalidSegmentRange &&
               bad_range.segments.empty() && bad_range.source_plans.empty(),
           "out-of-bounds segment did not fail closed");

    constexpr std::array bad_queue_segments{
        RHISubmitSegment{EQueueType::Ignore, 0, 1},
    };
    const std::array bad_queue_description{
        Describe(EQueueType::Graphics, 1, bad_queue_segments),
    };
    const RHISubmissionTopologyPlan bad_queue =
        BuildRHISubmissionTopology(bad_queue_description, 1);
    Expect(bad_queue.error == ERHISubmissionTopologyError::InvalidSegmentQueue &&
               bad_queue.error_segment_index == 0 && bad_queue.segments.empty(),
           "invalid segment queue did not fail closed");

    const std::array bad_root_description{
        Describe(EQueueType::Num, 1),
    };
    const RHISubmissionTopologyPlan bad_root =
        BuildRHISubmissionTopology(bad_root_description, 1);
    Expect(bad_root.error == ERHISubmissionTopologyError::InvalidRootQueue &&
               bad_root.error_source_index == 0 && bad_root.segments.empty(),
           "invalid root queue did not fail closed");
}

void EmptySubmitsRequireSideEffects() {
    const std::array descriptions{
        Describe(EQueueType::Graphics, 0),
        Describe(EQueueType::Compute, 0, {},
                 ERHITranslateExecutionClass::SerialControl, true),
    };

    const RHISubmissionTopologyPlan plan = BuildRHISubmissionTopology(descriptions, 19);
    Expect(plan.IsValid() && plan.source_plans.size() == 2 && plan.segments.size() == 1,
           "empty source retention was incorrect");
    Expect(plan.source_plans[0].segment_plan_count == 0,
           "side-effect-free empty submit produced a segment");
    Expect(plan.source_plans[1].segment_plan_begin == 0 &&
               plan.source_plans[1].segment_plan_count == 1,
           "empty side-effect submit was not retained");
    const RHISubmissionSegmentPlan& retained = plan.segments[0];
    Expect(retained.source_key == SourceSubmitKey{19, 1} && retained.segment.IsEmpty(),
           "retained empty segment lost its source identity or range");
    Expect(retained.inherit_source_wait_events &&
               retained.inherit_source_signal_events_and_callbacks &&
               retained.inherit_source_runtime_payload,
           "empty side-effect segment lost source boundary metadata");
}

} // namespace

int main() {
    try {
        StableInputOrderAndKeysArePreserved();
        CoverageAndBoundaryMetadataAreExplicit();
        RuntimePayloadSkipsCopySegments();
        SerialControlCreatesStableFrontierBarriers();
        InvalidRangesAndQueuesFailClosed();
        EmptySubmitsRequireSideEffects();
        std::cout << "RHI submission topology contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RHI submission topology contract test failed: " << error.what() << '\n';
        return 1;
    }
}
