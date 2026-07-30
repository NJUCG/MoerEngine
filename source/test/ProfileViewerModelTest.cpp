#include "profile_viewer_ui/ProfileViewerModel.h"
#include "profile/ProfileDump.h"
#include "profile/ProfileDumpTemplates.h"
#include "profile_consumer/ProfileDocument.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using namespace Moer;
using namespace Moer::ProfileDump;
using namespace std::chrono_literals;

static_assert(std::is_same_v<
              decltype(std::declval<const ProfileViewerModel&>().FindGpuViewport({1, 1})),
              std::optional<ProfileViewerViewport>>);
static_assert(sizeof(ProfileViewerModel) < 64 * 1024);

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

void ExpectValidViewport(const ProfileViewerViewport& _viewport, std::string_view _message) {
    Expect(
        _viewport.valid && _viewport.domain_end != 0 && _viewport.view_begin < _viewport.view_end &&
            _viewport.view_end <= _viewport.domain_end,
        _message
    );
}

void TestOverflowSafeFractionMapping() {
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();

    Expect(ProfileViewerMapFraction(0, maximum, 0, 10) == 0, "fraction zero did not map to begin");
    Expect(
        ProfileViewerMapFraction(0, maximum, 10, 10) == maximum,
        "fraction endpoint did not map exactly to uint64 max"
    );
    Expect(
        ProfileViewerMapFraction(maximum - 10, maximum, 1, 2) == maximum - 5,
        "fraction mapping overflowed a high-offset domain"
    );
    Expect(
        ProfileViewerMapFraction(0, maximum, 1, 2) == maximum / 2,
        "fraction midpoint did not use integer floor semantics"
    );
    Expect(ProfileViewerMapFraction(11, 99, 1, 0) == 11, "invalid fraction denominator did not map to begin");
    Expect(ProfileViewerMapFraction(11, 99, 3, 2) == 11, "out-of-range fraction did not map to begin");
    Expect(ProfileViewerMapFraction(99, 11, 1, 2) == 99, "reversed interval did not map to begin");
}

void ExpectGpuFrameChoice(
    std::optional<ProfileViewerGpuFrameChoice> _choice,
    std::uint64_t                              _frame_id,
    bool                                       _has_frame_record,
    std::string_view                           _message
) {
    Expect(
        _choice && _choice->frame_id == _frame_id && _choice->has_frame_record == _has_frame_record, _message
    );
}

void TestGpuFrameChoiceUnionNavigation() {
    const std::array<GpuTimelineFrameRef, 2>  recorded_frames{{
        {
             .frame_id           = 10,
             .source_frame_index = 0,
        },
        {
             .frame_id           = 30,
             .source_frame_index = 1,
        },
    }};
    const std::array<GpuTimelineAxisFrame, 4> axis_frames{{
        {
            .frame_id         = 20,
            .axis_index       = 1,
            .has_frame_record = false,
        },
        {
            .frame_id         = 30,
            .axis_index       = 1,
            .has_frame_record = true,
        },
        {
            .frame_id         = 5,
            .axis_index       = 2,
            .has_frame_record = false,
        },
        {
            .frame_id         = 20,
            .axis_index       = 2,
            .has_frame_record = false,
        },
    }};

    ExpectGpuFrameChoice(
        FindProfileViewerGpuFrameAtOrAfter(recorded_frames, axis_frames, 0),
        5,
        false,
        "GPU frame union did not expose its orphan first frame"
    );
    ExpectGpuFrameChoice(
        FindProfileViewerGpuFrameAtOrAfter(recorded_frames, axis_frames, 6),
        10,
        true,
        "GPU frame union did not advance to a source-only frame"
    );
    ExpectGpuFrameChoice(
        FindProfileViewerGpuFrameAtOrAfter(recorded_frames, axis_frames, 20),
        20,
        false,
        "GPU frame union did not collapse a duplicate orphan frame across axes"
    );
    ExpectGpuFrameChoice(
        FindProfileViewerGpuFrameAtOrAfter(recorded_frames, axis_frames, 30),
        30,
        true,
        "GPU frame union did not retain source-record identity"
    );
    Expect(
        !FindProfileViewerGpuFrameAtOrAfter(recorded_frames, axis_frames, 31),
        "GPU frame at-or-after search advanced beyond the union"
    );

    ExpectGpuFrameChoice(
        FindProfileViewerGpuFrameAfter(recorded_frames, axis_frames, 20),
        30,
        true,
        "GPU next-frame navigation did not cross source/axis sets"
    );
    Expect(
        !FindProfileViewerGpuFrameAfter(recorded_frames, axis_frames, 30),
        "GPU next-frame navigation advanced past the final frame"
    );
    ExpectGpuFrameChoice(
        FindProfileViewerGpuFrameBefore(recorded_frames, axis_frames, 20),
        10,
        true,
        "GPU previous-frame navigation did not cross source/axis sets"
    );
    Expect(
        !FindProfileViewerGpuFrameBefore(recorded_frames, axis_frames, 5),
        "GPU previous-frame navigation moved before the first frame"
    );
    ExpectGpuFrameChoice(
        FindProfileViewerGpuFrameAtOrBefore(recorded_frames, axis_frames, 19),
        10,
        true,
        "GPU at-or-before navigation did not clamp to the preceding frame"
    );
    ExpectGpuFrameChoice(
        FindProfileViewerGpuFrameAtOrBefore(recorded_frames, axis_frames, 20),
        20,
        false,
        "GPU at-or-before navigation skipped an exact orphan frame"
    );
    Expect(
        !FindProfileViewerGpuFrameAtOrBefore(recorded_frames, axis_frames, 4),
        "GPU at-or-before navigation moved before the union"
    );

    const std::array<GpuTimelineAxisFrame, 1> orphan_only{{
        {
            .frame_id         = 42,
            .axis_index       = 7,
            .has_frame_record = false,
        },
    }};
    ExpectGpuFrameChoice(
        FindProfileViewerGpuFrameAtOrAfter({}, orphan_only, 0),
        42,
        false,
        "axis-only orphan GPU frame was not selectable"
    );
    ExpectGpuFrameChoice(
        FindProfileViewerGpuFrameAtOrAfter(recorded_frames, {}, 11),
        30,
        true,
        "source-only GPU frames were dropped from the chooser"
    );

    const std::array<GpuTimelineAxisFrame, 2> axis_budget_boundary{{
        {
            .frame_id         = 60,
            .axis_index       = kProfileViewerGpuRenderableAxisMax,
            .has_frame_record = false,
        },
        {
            .frame_id         = 1,
            .axis_index       = kProfileViewerGpuRenderableAxisMax + 1,
            .has_frame_record = false,
        },
    }};
    ExpectGpuFrameChoice(
        FindProfileViewerGpuFrameAtOrAfter({}, axis_budget_boundary, 0),
        60,
        false,
        "GPU chooser did not retain the final viewer-renderable physical axis"
    );
    Expect(
        !FindProfileViewerGpuFrameAfter({}, axis_budget_boundary, 60),
        "GPU chooser exposed an orphan timeline beyond the viewer axis budget"
    );

    Expect(
        !FindProfileViewerGpuFrameAtOrAfter({}, {}, 0) && !FindProfileViewerGpuFrameAfter({}, {}, 0) &&
            !FindProfileViewerGpuFrameBefore({}, {}, 0) && !FindProfileViewerGpuFrameAtOrBefore({}, {}, 0),
        "empty GPU frame sources produced a navigation result"
    );

    constexpr std::uint64_t                  maximum = std::numeric_limits<std::uint64_t>::max();
    const std::array<GpuTimelineFrameRef, 1> maximum_frame{{
        {
            .frame_id           = maximum,
            .source_frame_index = 0,
        },
    }};
    ExpectGpuFrameChoice(
        FindProfileViewerGpuFrameAtOrBefore(maximum_frame, {}, maximum),
        maximum,
        true,
        "GPU frame navigation lost UINT64_MAX"
    );
    Expect(
        !FindProfileViewerGpuFrameAfter(maximum_frame, {}, maximum),
        "GPU next-frame navigation wrapped UINT64_MAX"
    );
}

class ScopedFixtures final {
public:
    ScopedFixtures() {
        static std::uint64_t next_id = 0;
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            const auto now =
                static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            directory_ =
                std::filesystem::temp_directory_path() /
                ("moer-profile-viewer-model-" + std::to_string(now) + "-" + std::to_string(next_id++));
            std::error_code error;
            if (std::filesystem::create_directory(directory_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                break;
            }
        }
        throw std::runtime_error("profile viewer model test could not reserve a temp directory");
    }

    ~ScopedFixtures() {
        static_cast<void>(Moer::ProfileDump::Shutdown());
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    [[nodiscard]] std::filesystem::path Path(std::string_view _name) const {
        return directory_ / (std::string(_name) + ".mpd");
    }

private:
    std::filesystem::path directory_{};
};

void WriteCapture(const std::filesystem::path& _path, std::string_view _scope_name) {
    static_cast<void>(Moer::ProfileDump::Shutdown());

    RuntimeConfig config;
    config.output_path      = _path;
    config.replace_existing = true;
    Expect(Start(config) == StartResult::Started, "fixture capture did not start");

    const SchemaRegistration cpu = RegisterSchema(Templates::CpuScope());
    Expect(cpu.status == SchemaStatus::Registered, "fixture CPU schema did not register");

    for (std::uint32_t index = 0; index < 4; ++index) {
        const std::uint64_t                 begin = static_cast<std::uint64_t>(index) * 4;
        const std::array<FieldValueView, 5> values{
            std::uint64_t{17},
            _scope_name,
            begin,
            begin + 2,
            std::uint32_t{0},
        };
        Expect(Emit(cpu.handle, values) == EmitStatus::Accepted, "fixture CPU scope emission failed");
    }

    Expect(Moer::ProfileDump::Shutdown() == ShutdownResult::Completed, "fixture capture did not close");
}

[[nodiscard]] bool IsTerminal(ProfileDocumentLoadPhase _phase) noexcept {
    return _phase == ProfileDocumentLoadPhase::Ready || _phase == ProfileDocumentLoadPhase::Failed ||
           _phase == ProfileDocumentLoadPhase::Cancelled || _phase == ProfileDocumentLoadPhase::Shutdown;
}

ProfileDocumentLoaderSnapshot
WaitForGeneration(const ProfileDocumentLoader& _loader, std::uint64_t _generation) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        auto snapshot = _loader.Snapshot();
        Expect(
            snapshot.request_generation == _generation,
            "loader snapshot did not describe the requested generation"
        );
        if (IsTerminal(snapshot.phase)) {
            return snapshot;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("profile document load timed out");
        }
        std::this_thread::sleep_for(1ms);
    }
}

void ExpectReady(const ProfileDocumentLoaderSnapshot& _snapshot, std::uint64_t _generation) {
    Expect(_snapshot.phase == ProfileDocumentLoadPhase::Ready, "fixture document was not ready");
    Expect(_snapshot.published_generation == _generation, "fixture publication generation mismatch");
    Expect(_snapshot.document != nullptr, "ready fixture had no document");
    Expect(_snapshot.document->Valid(), "ready fixture document was invalid");
}

void ExpectViewerState(
    const ProfileViewerModel&     _model,
    const ProfileViewerViewport&  _cpu,
    ProfileViewerGpuViewportKey   _gpu_key,
    const ProfileViewerViewport&  _gpu,
    const ProfileViewerSelection& _selection,
    std::size_t                   _gpu_count,
    std::string_view              _message
) {
    const auto observed_gpu = _model.FindGpuViewport(_gpu_key);
    Expect(
        _model.CpuViewport() == _cpu && observed_gpu && *observed_gpu == _gpu &&
            _model.Selection() == _selection && _model.ActiveGpuViewportCount() == _gpu_count,
        _message
    );
}

void TestPublicationAndIndependentNavigation(
    ProfileDocumentLoader&       _loader,
    const std::filesystem::path& _path_a,
    const std::filesystem::path& _path_b
) {
    ProfileViewerModel model;
    Expect(!model.FitCpu(10), "unpublished model accepted CPU navigation");

    const std::uint64_t generation_a = _loader.RequestLoad(_path_a);
    Expect(generation_a != 0, "first fixture load was rejected");
    const auto ready_a = WaitForGeneration(_loader, generation_a);
    ExpectReady(ready_a, generation_a);
    Expect(
        model.InspectSnapshot(ready_a) == EProfileViewerPublicationUpdate::Changed &&
            model.PublishedGeneration() == kInvalidProfileDocumentGeneration,
        "publication inspection mutated the unpublished model"
    );
    Expect(
        model.ObserveSnapshot(ready_a) == EProfileViewerPublicationUpdate::Changed,
        "first publication did not reset the model"
    );
    Expect(model.PublishedGeneration() == generation_a, "model generation did not publish");

    constexpr ProfileViewerGpuViewportKey gpu_a{1, 42};
    constexpr ProfileViewerGpuViewportKey gpu_b{1, 43};
    constexpr ProfileViewerGpuViewportKey gpu_c{2, 42};
    Expect(model.FitCpu(1'000), "CPU fit failed");
    Expect(model.ZoomCpu(1, 2, 1, 2), "CPU zoom failed");
    Expect(model.PanCpu(100), "CPU pan failed");
    Expect(model.FitGpu(gpu_a, 100), "first GPU fit failed");
    Expect(model.ZoomGpu(gpu_a, 1, 2, 1, 2), "first GPU zoom failed");
    Expect(model.FitGpu(gpu_b, 200), "second GPU fit failed");
    Expect(model.FocusGpu(gpu_b, 20, 40, 5), "second GPU focus failed");
    Expect(model.FitGpu(gpu_c, 300), "third GPU fit failed");

    const auto cpu_before_gpu_pan = model.CpuViewport();
    const auto gpu_a_before       = model.FindGpuViewport(gpu_a);
    const auto gpu_c_before       = model.FindGpuViewport(gpu_c);
    Expect(gpu_a_before && gpu_c_before, "GPU key lookup failed");
    Expect(model.PanGpu(gpu_b, 10), "GPU pan failed");
    Expect(model.CpuViewport() == cpu_before_gpu_pan, "GPU pan changed the CPU viewport");
    Expect(model.FindGpuViewport(gpu_a) == gpu_a_before, "GPU frame key leaked into another frame");
    Expect(model.FindGpuViewport(gpu_c) == gpu_c_before, "GPU axis key leaked into another axis");

    Expect(model.SelectCpu(0, 1, 999, 999), "CPU point-scope selection failed");
    const auto cpu_point_selection = model.Selection();
    Expect(!model.SelectCpu(0, 1, 1'000, 1'000), "point scope at the exclusive CPU domain end was accepted");
    Expect(model.Selection() == cpu_point_selection, "rejected exclusive-end CPU point mutated selection");
    Expect(model.SelectCpu(0, 1, 400, 410), "CPU selection failed");
    const ProfileViewerSelection selection_a = model.Selection();
    Expect(selection_a.Valid(), "CPU selection was not valid");
    const auto cpu_a       = model.CpuViewport();
    const auto gpu_a_state = *model.FindGpuViewport(gpu_a);
    const auto gpu_count_a = model.ActiveGpuViewportCount();

    auto loading_b                = ready_a;
    loading_b.request_generation  = generation_a + 1;
    loading_b.phase               = ProfileDocumentLoadPhase::Reading;
    loading_b.latest_attempt_path = _path_b;
    Expect(
        model.ObserveSnapshot(loading_b) == EProfileViewerPublicationUpdate::Unchanged,
        "latest request generation reset last-good publication state"
    );
    ExpectViewerState(
        model,
        cpu_a,
        gpu_a,
        gpu_a_state,
        selection_a,
        gpu_count_a,
        "in-progress attempt changed last-good view state"
    );

    auto failed_b  = loading_b;
    failed_b.phase = ProfileDocumentLoadPhase::Failed;
    Expect(
        model.ObserveSnapshot(failed_b) == EProfileViewerPublicationUpdate::Unchanged,
        "failed latest request reset last-good publication state"
    );
    auto cancelled_b  = loading_b;
    cancelled_b.phase = ProfileDocumentLoadPhase::Cancelled;
    Expect(
        model.ObserveSnapshot(cancelled_b) == EProfileViewerPublicationUpdate::Unchanged,
        "cancelled latest request reset last-good publication state"
    );
    ExpectViewerState(
        model,
        cpu_a,
        gpu_a,
        gpu_a_state,
        selection_a,
        gpu_count_a,
        "failed or cancelled attempt changed last-good view state"
    );

    auto malformed                 = ready_a;
    malformed.published_generation = generation_a + 1;
    Expect(
        model.ObserveSnapshot(malformed) == EProfileViewerPublicationUpdate::Invalid,
        "mismatched publication and document generation was accepted"
    );
    ExpectViewerState(
        model,
        cpu_a,
        gpu_a,
        gpu_a_state,
        selection_a,
        gpu_count_a,
        "invalid snapshot changed current view state"
    );
    Expect(
        model.PublishedGeneration() == generation_a,
        "invalid snapshot changed the last-good publication generation"
    );

    auto missing_document = ready_a;
    missing_document.document.reset();
    Expect(
        model.ObserveSnapshot(missing_document) == EProfileViewerPublicationUpdate::Invalid,
        "non-empty publication without a document was accepted"
    );
    Expect(
        model.PublishedGeneration() == generation_a && model.Selection() == selection_a,
        "missing-document snapshot cleared last-good publication state"
    );

    const std::uint64_t generation_b = _loader.RequestLoad(_path_b);
    Expect(generation_b == generation_a + 1, "second fixture generation was not monotonic");
    const auto ready_b = WaitForGeneration(_loader, generation_b);
    ExpectReady(ready_b, generation_b);
    Expect(
        model.InspectSnapshot(ready_b) == EProfileViewerPublicationUpdate::Changed &&
            model.PublishedGeneration() == generation_a && model.Selection() == selection_a,
        "publication inspection changed last-good navigation state"
    );
    Expect(
        model.ObserveSnapshot(ready_b) == EProfileViewerPublicationUpdate::Changed,
        "new publication did not reset the model"
    );
    Expect(!model.CpuViewport().valid, "new publication retained CPU viewport state");
    Expect(model.ActiveGpuViewportCount() == 0, "new publication retained GPU viewport state");
    Expect(!model.Selection().Valid(), "new publication retained selection state");
    Expect(
        model.ObserveSnapshot(ready_b) == EProfileViewerPublicationUpdate::Unchanged,
        "same publication reset the model twice"
    );

    Expect(model.FitCpu(std::numeric_limits<std::uint64_t>::max()), "maximum CPU domain fit failed");
    Expect(model.ZoomCpu(1, 2, std::numeric_limits<std::uint32_t>::max(), 1), "saturating zoom failed");
    ExpectValidViewport(model.CpuViewport(), "saturating zoom broke CPU viewport invariants");
    Expect(model.ZoomCpu(1, 2, 1, std::numeric_limits<std::uint32_t>::max()), "maximum-ratio zoom-in failed");
    ExpectValidViewport(model.CpuViewport(), "maximum-ratio zoom-in broke CPU viewport invariants");
    Expect(
        model.FocusCpu(
            std::numeric_limits<std::uint64_t>::max(), 0, std::numeric_limits<std::uint64_t>::max()
        ),
        "overflow-edge focus failed"
    );
    Expect(
        model.CpuViewport().view_begin == 0 &&
            model.CpuViewport().view_end == std::numeric_limits<std::uint64_t>::max(),
        "maximum focus margin did not clamp to the domain"
    );
    Expect(model.FocusCpu(10, 20, 0), "small focus failed");
    Expect(model.PanCpu(std::numeric_limits<std::int64_t>::min()), "minimum pan failed");
    Expect(model.CpuViewport().view_begin == 0, "minimum pan did not clamp left");
    Expect(model.PanCpu(std::numeric_limits<std::int64_t>::max()), "maximum pan failed");
    ExpectValidViewport(model.CpuViewport(), "maximum pan broke CPU viewport invariants");

    const auto before_invalid_zoom = model.CpuViewport();
    Expect(!model.ZoomCpu(1, 0, 1, 1), "zero anchor denominator was accepted");
    Expect(!model.ZoomCpu(2, 1, 1, 1), "out-of-range anchor was accepted");
    Expect(!model.ZoomCpu(1, 2, 0, 1), "zero zoom numerator was accepted");
    Expect(model.CpuViewport() == before_invalid_zoom, "invalid zoom mutated viewport state");

    constexpr ProfileViewerGpuViewportKey same_frame_axis_a{1, 77};
    constexpr ProfileViewerGpuViewportKey same_frame_axis_b{2, 77};
    constexpr ProfileViewerGpuViewportKey same_axis_frame_b{1, 78};
    Expect(model.FitGpu(same_frame_axis_a, 101), "GPU identity fit A failed");
    Expect(model.FitGpu(same_frame_axis_b, 202), "GPU identity fit B failed");
    Expect(model.FitGpu(same_axis_frame_b, 303), "GPU identity fit C failed");
    Expect(
        model.FindGpuViewport(same_frame_axis_a)->domain_end == 101 &&
            model.FindGpuViewport(same_frame_axis_b)->domain_end == 202 &&
            model.FindGpuViewport(same_axis_frame_b)->domain_end == 303,
        "GPU viewports were not keyed by both axis and frame"
    );

    Expect(model.SelectGpu(same_frame_axis_a, 4, 9, 10, 20), "GPU selection failed");
    Expect(model.Selection().Valid(), "GPU selection was invalid");
    const auto selected_gpu = model.Selection();
    Expect(model.SelectGpu(same_frame_axis_a, 4, 9, 100, 100), "GPU point-scope selection failed");
    const auto selected_gpu_point = model.Selection();
    Expect(
        !model.SelectGpu(same_frame_axis_a, 4, 9, 101, 101),
        "point scope at the exclusive GPU domain end was accepted"
    );
    Expect(model.Selection() == selected_gpu_point, "rejected exclusive-end GPU point mutated selection");
    Expect(model.SelectGpu(same_frame_axis_a, 4, 9, 10, 20), "GPU reselection failed");
    Expect(
        !model.SelectGpu(same_frame_axis_a, ProfileDump::kInvalidSessionIndex, 9, 10, 20),
        "invalid GPU track selection was accepted"
    );
    Expect(model.Selection() == selected_gpu, "rejected selection mutated current selection");

    model.ClearSelection();
    Expect(!model.Selection().Valid(), "selection clear failed");

    const auto cpu_before_capacity = model.CpuViewport();
    for (std::uint64_t frame = 1'000; frame < 1'000 + kProfileViewerGpuViewportCapacity; ++frame) {
        Expect(model.FitGpu({3, frame}, frame + 1), "fixed GPU viewport table fill failed");
    }
    for (std::uint64_t frame = 10'000; frame < 14'096; ++frame) {
        Expect(model.FitGpu({4, frame}, frame + 1), "fixed GPU viewport table eviction failed");
    }
    Expect(
        model.ActiveGpuViewportCount() == kProfileViewerGpuViewportCapacity,
        "GPU viewport state grew beyond its fixed capacity"
    );
    Expect(model.FindGpuViewport({4, 14'095}).has_value(), "newest evicted-table key was not retained");
    Expect(model.CpuViewport() == cpu_before_capacity, "GPU table eviction changed CPU state");

    ProfileDocumentLoaderSnapshot empty;
    Expect(
        model.ObserveSnapshot(empty) == EProfileViewerPublicationUpdate::Changed,
        "empty publication did not reset a published model"
    );
    Expect(model.PublishedGeneration() == 0, "empty publication did not clear generation");
    Expect(!model.CpuViewport().valid, "empty publication retained CPU state");
    Expect(model.ActiveGpuViewportCount() == 0, "empty publication retained GPU state");

    auto malformed_empty     = empty;
    malformed_empty.document = ready_b.document;
    Expect(
        model.ObserveSnapshot(malformed_empty) == EProfileViewerPublicationUpdate::Invalid,
        "empty generation with a document was accepted"
    );
}

} // namespace

int main() {
    try {
        TestOverflowSafeFractionMapping();
        TestGpuFrameChoiceUnionNavigation();

        ScopedFixtures fixtures;
        const auto     path_a = fixtures.Path("a");
        const auto     path_b = fixtures.Path("b");
        WriteCapture(path_a, "ViewerA");
        WriteCapture(path_b, "ViewerB");

        ProfileDocumentLoader loader;
        TestPublicationAndIndependentNavigation(loader, path_a, path_b);
        loader.Shutdown();

        std::cout << "ProfileViewerModel contract passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ProfileViewerModel contract failed: " << error.what() << '\n';
        return 1;
    }
}
