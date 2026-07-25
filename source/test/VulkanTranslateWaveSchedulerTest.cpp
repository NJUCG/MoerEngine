#include "rhi/vulkan/VulkanTranslateWaveScheduler.h"
#include "taskgraph/TaskGraph.h"

#include <array>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Moer::Render;
using namespace Moer::Render::VulkanTranslateWaveDetail;

static_assert(noexcept(std::declval<TaskGraph&>()
                           .QueueTask(nullptr, EThread::AnyThread_NormalPri, EThread::UNKNOWN_THREAD, true)));
static_assert(noexcept(std::declval<BaseGraphTask&>().QueueTask(EThread::UNKNOWN_THREAD, true)));
static_assert(noexcept(std::declval<BaseGraphTask&>().ConditionalQueueTask(EThread::UNKNOWN_THREAD, true)));
static_assert(noexcept(std::declval<BaseGraphTask&>().PrerequestsComplete(EThread::UNKNOWN_THREAD, 0, true)));

namespace {

constexpr uint64_t BatchSequence = 71;

[[nodiscard]] constexpr SubmissionKey Key(uint32_t _index) noexcept {
    return SubmissionKey{BatchSequence, _index};
}

void Expect(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

void ExpectWave(
    const std::vector<SubmissionKey>&    _actual,
    std::initializer_list<SubmissionKey> _expected,
    const char*                          _message
) {
    if (_actual.size() != _expected.size()) {
        throw std::runtime_error(_message);
    }
    size_t index = 0;
    for (const SubmissionKey& expected : _expected) {
        if (!(_actual[index++] == expected)) {
            throw std::runtime_error(_message);
        }
    }
}

void IndependentNativeQueuesShareAStableWave() {
    const std::array nodes{
        TranslateWaveNode{.key = Key(0), .native_queue_id = 3, .async_translate = true},
        TranslateWaveNode{.key = Key(1), .native_queue_id = 8, .async_translate = true},
    };

    TranslateWaveScheduler scheduler;
    Expect(scheduler.Build(nodes), "independent roots failed to build");
    ExpectWave(
        scheduler.NextWave(), {Key(0), Key(1)}, "independent native queues did not share one stable wave"
    );
    ExpectWave(scheduler.NextWave(), {}, "already scheduled roots were returned twice");
    Expect(
        scheduler.MarkTranslated(Key(1)) && scheduler.MarkTranslated(Key(0)),
        "independent roots could not complete translation out of order"
    );
    Expect(
        scheduler.MarkReleased(Key(0)) && scheduler.MarkReleased(Key(1)) && scheduler.IsComplete(),
        "independent roots did not release exactly once"
    );
}

void GraphicsComputeCopyShareTheFirstReadyWave() {
    const std::array nodes{
        // Graphics front.
        TranslateWaveNode{.key = Key(0), .native_queue_id = 40, .async_translate = true},
        // Graphics tail shares the same physical lane as the front.
        TranslateWaveNode{.key = Key(1), .native_queue_id = 40, .async_translate = true},
        // Independent Compute and Copy lanes.
        TranslateWaveNode{.key = Key(2), .native_queue_id = 41, .async_translate = true},
        TranslateWaveNode{.key = Key(3), .native_queue_id = 42, .async_translate = true},
    };

    TranslateWaveScheduler scheduler;
    Expect(scheduler.Build(nodes), "Graphics/Compute/Copy topology failed to build");
    ExpectWave(
        scheduler.NextWave(),
        {Key(0), Key(2), Key(3)},
        "first ready wave did not expose Graphics, Compute, and Copy"
    );
    Expect(
        scheduler.MarkTranslated(Key(3)) && scheduler.MarkTranslated(Key(2)) &&
            scheduler.MarkTranslated(Key(0)),
        "independent Graphics/Compute/Copy lanes could not translate out of order"
    );
    ExpectWave(
        scheduler.NextWave(),
        {},
        "same-native Graphics tail started before its packet predecessor released"
    );
    Expect(
        scheduler.MarkReleased(Key(0)),
        "Graphics front packet did not release its native lane"
    );
    ExpectWave(
        scheduler.NextWave(),
        {Key(1)},
        "Graphics front release did not unlock its same-native tail"
    );
    Expect(
        scheduler.MarkTranslated(Key(1)) &&
            scheduler.MarkReleased(Key(1)) &&
            scheduler.MarkReleased(Key(2)) &&
            scheduler.MarkReleased(Key(3)) &&
            scheduler.IsComplete(),
        "Graphics/Compute/Copy packets did not retire exactly once"
    );
    Expect(
        !scheduler.MarkReleased(Key(0)) &&
            !scheduler.MarkReleased(Key(2)) &&
            !scheduler.MarkReleased(Key(3)),
        "Graphics/Compute/Copy packet release was accepted more than once"
    );
}

void SameNativeQueueWaitsForReleaseWithoutBlockingAnotherLane() {
    const std::array nodes{
        TranslateWaveNode{.key = Key(0), .native_queue_id = 4, .async_translate = true},
        // A logical Compute alias of the same physical queue.
        TranslateWaveNode{.key = Key(1), .native_queue_id = 4, .async_translate = true},
        TranslateWaveNode{.key = Key(2), .native_queue_id = 9, .async_translate = true},
    };

    TranslateWaveScheduler scheduler;
    Expect(scheduler.Build(nodes), "same-native topology failed to build");
    ExpectWave(
        scheduler.NextWave(), {Key(0), Key(2)}, "blocked same-native successor hid a ready independent lane"
    );
    Expect(
        scheduler.MarkTranslated(Key(0)) && scheduler.MarkTranslated(Key(2)),
        "first native-lane ready set did not finish translation"
    );
    ExpectWave(scheduler.NextWave(), {}, "same-native successor started before packet release");
    Expect(scheduler.MarkReleased(Key(0)), "same-native predecessor packet was not released");
    ExpectWave(
        scheduler.NextWave(),
        {Key(1)},
        "released alias did not unlock its successor while the later lane was held"
    );
    Expect(
        scheduler.MarkTranslated(Key(1)) && scheduler.MarkReleased(Key(1)) &&
            scheduler.MarkReleased(Key(2)) && scheduler.IsComplete(),
        "held later-lane packet did not preserve ordered-release ownership"
    );
}

void GraphicsCopyAliasWaitsForReleaseWithoutBlockingCompute() {
    const std::array nodes{
        // Graphics and Copy are distinct logical queues on native lane 50.
        TranslateWaveNode{.key = Key(0), .native_queue_id = 50, .async_translate = true},
        TranslateWaveNode{.key = Key(1), .native_queue_id = 50, .async_translate = true},
        TranslateWaveNode{.key = Key(2), .native_queue_id = 51, .async_translate = true},
    };

    TranslateWaveScheduler scheduler;
    Expect(scheduler.Build(nodes), "Graphics/Copy alias topology failed to build");
    ExpectWave(
        scheduler.NextWave(),
        {Key(0), Key(2)},
        "Graphics/Copy alias hid its independent Compute lane"
    );
    Expect(
        scheduler.MarkTranslated(Key(0)) && scheduler.MarkTranslated(Key(2)),
        "Graphics/Compute first wave did not finish translation"
    );
    ExpectWave(
        scheduler.NextWave(),
        {},
        "Copy entered Translate before its aliased Graphics packet released"
    );
    Expect(
        scheduler.MarkReleased(Key(0)),
        "Graphics packet did not release its aliased Copy lane"
    );
    ExpectWave(
        scheduler.NextWave(),
        {Key(1)},
        "Graphics release did not unlock its aliased Copy successor"
    );
    Expect(
        scheduler.MarkTranslated(Key(1)) &&
            scheduler.MarkReleased(Key(1)) &&
            scheduler.MarkReleased(Key(2)) &&
            scheduler.IsComplete(),
        "Graphics/Copy alias packets did not retire exactly once"
    );
}

void ComputeCopyAliasWaitsForReleaseWithoutBlockingGraphics() {
    const std::array nodes{
        // Compute and Copy are distinct logical queues on native lane 60.
        TranslateWaveNode{.key = Key(0), .native_queue_id = 60, .async_translate = true},
        TranslateWaveNode{.key = Key(1), .native_queue_id = 60, .async_translate = true},
        TranslateWaveNode{.key = Key(2), .native_queue_id = 61, .async_translate = true},
    };

    TranslateWaveScheduler scheduler;
    Expect(scheduler.Build(nodes), "Compute/Copy alias topology failed to build");
    ExpectWave(
        scheduler.NextWave(),
        {Key(0), Key(2)},
        "Compute/Copy alias hid its independent Graphics lane"
    );
    Expect(
        scheduler.MarkTranslated(Key(0)) && scheduler.MarkTranslated(Key(2)),
        "Compute/Graphics first wave did not finish translation"
    );
    ExpectWave(
        scheduler.NextWave(),
        {},
        "Copy entered Translate before its aliased Compute packet released"
    );
    Expect(
        scheduler.MarkReleased(Key(0)),
        "Compute packet did not release its aliased Copy lane"
    );
    ExpectWave(
        scheduler.NextWave(),
        {Key(1)},
        "Compute release did not unlock its aliased Copy successor"
    );
    Expect(
        scheduler.MarkTranslated(Key(1)) &&
            scheduler.MarkReleased(Key(1)) &&
            scheduler.MarkReleased(Key(2)) &&
            scheduler.IsComplete(),
        "Compute/Copy alias packets did not retire exactly once"
    );
}

void ExplicitDependencyUnlocksAtTranslated() {
    constexpr std::array dependency{Key(0)};
    const std::array     nodes{
        TranslateWaveNode{.key = Key(0), .native_queue_id = 1, .async_translate = true},
        TranslateWaveNode{
                .key             = Key(1),
                .native_queue_id = 2,
                .async_translate = true,
                .dependencies    = dependency,
        },
    };

    TranslateWaveScheduler scheduler;
    Expect(scheduler.Build(nodes), "explicit dependency failed to build");
    ExpectWave(scheduler.NextWave(), {Key(0)}, "dependent node entered its prerequisite wave");
    ExpectWave(scheduler.NextWave(), {}, "dependent node started before prerequisite translation");
    Expect(scheduler.MarkTranslated(Key(0)), "explicit prerequisite did not translate");
    ExpectWave(scheduler.NextWave(), {Key(1)}, "explicit dependency incorrectly waited for packet release");
}

void FanInAndFanOutRemainDeterministic() {
    constexpr std::array fan_in_dependencies{Key(0), Key(1)};
    constexpr std::array fan_out_dependency{Key(2)};
    const std::array     nodes{
        TranslateWaveNode{.key = Key(0), .native_queue_id = 10, .async_translate = true},
        TranslateWaveNode{.key = Key(1), .native_queue_id = 11, .async_translate = true},
        TranslateWaveNode{
                .key             = Key(2),
                .native_queue_id = 12,
                .async_translate = true,
                .dependencies    = fan_in_dependencies,
        },
        TranslateWaveNode{
                .key             = Key(3),
                .native_queue_id = 13,
                .async_translate = true,
                .dependencies    = fan_out_dependency,
        },
        TranslateWaveNode{
                .key             = Key(4),
                .native_queue_id = 14,
                .async_translate = true,
                .dependencies    = fan_out_dependency,
        },
    };

    TranslateWaveScheduler scheduler;
    Expect(scheduler.Build(nodes), "fan-in/fan-out topology failed to build");
    ExpectWave(scheduler.NextWave(), {Key(0), Key(1)}, "fan-in roots did not form the first wave");
    Expect(scheduler.MarkTranslated(Key(0)), "first fan-in prerequisite did not translate");
    ExpectWave(scheduler.NextWave(), {}, "fan-in node started with one incomplete prerequisite");
    Expect(scheduler.MarkTranslated(Key(1)), "second fan-in prerequisite did not translate");
    ExpectWave(scheduler.NextWave(), {Key(2)}, "fan-in node did not wait for both prerequisites");
    Expect(scheduler.MarkTranslated(Key(2)), "fan-out prerequisite did not translate");
    ExpectWave(
        scheduler.NextWave(), {Key(3), Key(4)}, "fan-out nodes did not retain stable order in one wave"
    );
}

void NonAsyncNodeIsAnExclusiveReleaseBoundary() {
    constexpr std::array control_dependency{Key(2)};
    const std::array     nodes{
        TranslateWaveNode{.key = Key(0), .native_queue_id = 20, .async_translate = true},
        TranslateWaveNode{.key = Key(1), .native_queue_id = 21, .async_translate = true},
        TranslateWaveNode{.key = Key(2), .native_queue_id = 22, .async_translate = false},
        TranslateWaveNode{
                .key             = Key(3),
                .native_queue_id = 23,
                .async_translate = true,
                .dependencies    = control_dependency,
        },
    };

    TranslateWaveScheduler scheduler;
    Expect(scheduler.Build(nodes), "hard-boundary topology failed to build");
    ExpectWave(scheduler.NextWave(), {Key(0), Key(1)}, "hard boundary joined an async wave");
    Expect(
        scheduler.MarkTranslated(Key(0)) && scheduler.MarkTranslated(Key(1)),
        "hard-boundary prefix did not translate"
    );
    ExpectWave(scheduler.NextWave(), {}, "hard boundary started before the earlier prefix was released");
    Expect(scheduler.MarkReleased(Key(0)), "first hard-boundary prefix packet did not release");
    ExpectWave(scheduler.NextWave(), {}, "hard boundary ignored an unreleased earlier packet");
    Expect(scheduler.MarkReleased(Key(1)), "second hard-boundary prefix packet did not release");
    ExpectWave(scheduler.NextWave(), {Key(2)}, "hard boundary was not emitted as an exclusive wave");
    Expect(scheduler.MarkTranslated(Key(2)), "hard boundary did not finish translation");
    ExpectWave(scheduler.NextWave(), {}, "hard-boundary dependent started before release");
    Expect(scheduler.MarkReleased(Key(2)), "hard-boundary packet did not release");
    ExpectWave(scheduler.NextWave(), {Key(3)}, "hard-boundary release did not unlock its suffix");
}

void UnknownAndFutureDependenciesFailClosed() {
    constexpr std::array unknown_dependency{SubmissionKey{BatchSequence, 99}};
    const std::array     unknown_nodes{
        TranslateWaveNode{
                .key             = Key(0),
                .native_queue_id = 1,
                .async_translate = true,
                .dependencies    = unknown_dependency,
        },
    };

    TranslateWaveScheduler unknown;
    Expect(
        !unknown.Build(unknown_nodes) &&
            unknown.GetBuildError() == ETranslateWaveBuildError::UnknownDependency &&
            unknown.GetErrorNodeIndex() == 0 && unknown.GetErrorDependencyIndex() == 0,
        "unknown dependency did not fail closed with stable diagnostics"
    );
    ExpectWave(unknown.NextWave(), {}, "invalid scheduler emitted work for an unknown dependency");

    constexpr std::array future_dependency{Key(1)};
    const std::array     future_nodes{
        TranslateWaveNode{
                .key             = Key(0),
                .native_queue_id = 1,
                .async_translate = true,
                .dependencies    = future_dependency,
        },
        TranslateWaveNode{.key = Key(1), .native_queue_id = 2, .async_translate = true},
    };

    TranslateWaveScheduler future;
    Expect(
        !future.Build(future_nodes) && future.GetBuildError() == ETranslateWaveBuildError::FutureDependency,
        "future dependency did not fail closed"
    );

    const std::array duplicate_nodes{
        TranslateWaveNode{.key = Key(0), .native_queue_id = 1, .async_translate = true},
        TranslateWaveNode{.key = Key(0), .native_queue_id = 2, .async_translate = true},
    };
    TranslateWaveScheduler duplicate;
    Expect(
        !duplicate.Build(duplicate_nodes) &&
            duplicate.GetBuildError() == ETranslateWaveBuildError::DuplicateKey &&
            duplicate.GetErrorNodeIndex() == 1,
        "duplicate key did not fail closed"
    );
}

void CancellationRetainsOnlyOutstandingGraphicsCopyPacketLeases() {
    const std::array nodes{
        // Graphics front and Copy tail alias native lane 31.
        TranslateWaveNode{.key = Key(0), .native_queue_id = 31, .async_translate = true},
        TranslateWaveNode{.key = Key(1), .native_queue_id = 31, .async_translate = true},
        // Compute remains independently ready.
        TranslateWaveNode{.key = Key(2), .native_queue_id = 32, .async_translate = true},
    };

    TranslateWaveScheduler scheduler;
    Expect(scheduler.Build(nodes), "Graphics/Copy cancellation topology failed to build");
    ExpectWave(
        scheduler.NextWave(),
        {Key(0), Key(2)},
        "Graphics/Copy cancellation fixture did not expose every ready native lane"
    );
    scheduler.Cancel();
    Expect(scheduler.IsCancelled() && !scheduler.IsComplete(), "cancellation lost an in-flight packet lease");
    ExpectWave(scheduler.NextWave(), {}, "cancelled scheduler emitted later work");
    Expect(
        scheduler.GetState(Key(1)) == ETranslateWaveNodeState::Cancelled &&
            scheduler.GetState(Key(2)) == ETranslateWaveNodeState::Translating,
        "cancellation did not terminalize the unscheduled suffix"
    );
    Expect(
        scheduler.MarkReleased(Key(0)) && scheduler.MarkReleased(Key(2)) && scheduler.IsComplete(),
        "cancelled in-flight packets could not be released exactly once"
    );
    Expect(
        !scheduler.MarkReleased(Key(0)) && !scheduler.MarkTranslated(Key(1)),
        "invalid terminal transitions were accepted"
    );
}

void EmptyBuildIsImmediatelyComplete() {
    TranslateWaveScheduler scheduler;
    Expect(
        scheduler.Build({}) && scheduler.IsValid() && scheduler.IsComplete(),
        "empty valid topology was not immediately complete"
    );
    ExpectWave(scheduler.NextWave(), {}, "empty valid topology emitted a wave");
}

} // namespace

int main() {
    try {
        IndependentNativeQueuesShareAStableWave();
        GraphicsComputeCopyShareTheFirstReadyWave();
        SameNativeQueueWaitsForReleaseWithoutBlockingAnotherLane();
        GraphicsCopyAliasWaitsForReleaseWithoutBlockingCompute();
        ComputeCopyAliasWaitsForReleaseWithoutBlockingGraphics();
        ExplicitDependencyUnlocksAtTranslated();
        FanInAndFanOutRemainDeterministic();
        NonAsyncNodeIsAnExclusiveReleaseBoundary();
        UnknownAndFutureDependenciesFailClosed();
        CancellationRetainsOnlyOutstandingGraphicsCopyPacketLeases();
        EmptyBuildIsImmediatelyComplete();
        std::cout << "Vulkan translate-wave scheduler tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Vulkan translate-wave scheduler test failed: " << error.what() << '\n';
        return 1;
    }
}
