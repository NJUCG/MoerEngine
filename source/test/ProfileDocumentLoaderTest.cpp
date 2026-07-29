#include "profile/ProfileDump.h"
#include "profile/ProfileDumpTemplates.h"
#include "profile_consumer/ProfileDocument.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace Moer::ProfileDump;
using namespace std::chrono_literals;

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

class ScopedLoaderFixtures final {
public:
    ScopedLoaderFixtures() {
        static std::uint64_t next_id = 0;
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            const auto now =
                static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            directory_ =
                std::filesystem::temp_directory_path() /
                ("moer-profile-document-loader-" + std::to_string(now) + "-" + std::to_string(next_id++));
            std::error_code error;
            if (std::filesystem::create_directory(directory_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                break;
            }
        }
        throw std::runtime_error("profile document loader test could not reserve a temp directory");
    }

    ~ScopedLoaderFixtures() {
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

void WriteCapture(
    const std::filesystem::path& _path,
    std::string_view             _scope_name,
    std::uint32_t                _scope_count
) {
    static_cast<void>(Moer::ProfileDump::Shutdown());

    RuntimeConfig config;
    config.output_path      = _path;
    config.replace_existing = true;
    // The fixture is intentionally lossless. Optimized producers can outrun
    // the writer while creating the two 131k-scope captures, so size the
    // bounded queue for the complete synthetic burst instead of depending on
    // scheduler timing.
    config.queue_max_chunks  = 4096;
    config.queue_max_records = 262144;
    Expect(Start(config) == StartResult::Started, "fixture capture did not start");

    const SchemaRegistration cpu = RegisterSchema(Templates::CpuScope());
    Expect(cpu.status == SchemaStatus::Registered, "fixture CPU schema did not register");

    for (std::uint32_t index = 0; index < _scope_count; ++index) {
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

ProfileDocumentLoaderSnapshot WaitForGeneration(
    const ProfileDocumentLoader& _loader,
    std::uint64_t                _generation,
    std::chrono::milliseconds    _timeout = 10s
) {
    const auto deadline = std::chrono::steady_clock::now() + _timeout;
    for (;;) {
        auto snapshot = _loader.Snapshot();
        Expect(
            snapshot.request_generation == _generation,
            "loader progress/result generation was not paired with the latest request"
        );
        if (IsTerminal(snapshot.phase)) {
            Expect(
                !snapshot.active_request || snapshot.active_generation != _generation,
                "terminal latest generation remained active in the same snapshot"
            );
            return snapshot;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("profile document loader timed out");
        }
        std::this_thread::sleep_for(1ms);
    }
}

ProfileDocumentLoaderSnapshot WaitUntilActive(
    const ProfileDocumentLoader& _loader,
    std::uint64_t                _generation,
    std::chrono::milliseconds    _timeout = 10s
) {
    const auto deadline = std::chrono::steady_clock::now() + _timeout;
    for (;;) {
        auto snapshot = _loader.Snapshot();
        Expect(
            snapshot.request_generation == _generation, "active wait observed a different latest generation"
        );
        if (snapshot.active_request && snapshot.active_generation == _generation) {
            return snapshot;
        }
        if (IsTerminal(snapshot.phase)) {
            throw std::runtime_error("request completed before active state was observed");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("profile document loader did not activate the request");
        }
        std::this_thread::yield();
    }
}

void ExpectReadyDocument(
    const ProfileDocumentLoaderSnapshot& _snapshot,
    std::uint64_t                        _generation,
    const std::filesystem::path&         _path
) {
    Expect(_snapshot.phase == ProfileDocumentLoadPhase::Ready, "document load was not ready");
    Expect(_snapshot.request_generation == _generation, "ready generation mismatch");
    Expect(_snapshot.published_generation == _generation, "published generation mismatch");
    Expect(_snapshot.latest_attempt_path == _path, "latest attempt path mismatch");
    Expect(_snapshot.reported_input_bytes_final, "ready reader byte count was not final");
    Expect(
        _snapshot.observed_total_bytes_available,
        "regular fixture did not expose a best-effort total-byte observation"
    );
    Expect(
        _snapshot.observed_total_bytes == std::filesystem::file_size(_path),
        "best-effort total-byte observation did not match the stable fixture"
    );
    Expect(
        _snapshot.reported_input_bytes == _snapshot.session_result.input_bytes,
        "final reported input byte count did not match the reader outcome"
    );
    Expect(_snapshot.document != nullptr, "ready snapshot had no document");
    Expect(_snapshot.document->request_generation == _generation, "document generation mismatch");
    Expect(_snapshot.document->source_path == _path, "document source path mismatch");
    Expect(_snapshot.document->Valid(), "published document was internally inconsistent");
    Expect(
        _snapshot.document->timeline_index.Matches(_snapshot.document->session),
        "published index did not match its owning session"
    );
}

void TestSuccessReplacementAndFailurePreservation(
    const std::filesystem::path& _a,
    const std::filesystem::path& _b,
    const std::filesystem::path& _missing
) {
    ProfileDocumentLoader loader;
    const std::uint64_t   generation_a = loader.RequestLoad(_a);
    Expect(generation_a != 0, "first load request was rejected");
    auto ready_a = WaitForGeneration(loader, generation_a);
    ExpectReadyDocument(ready_a, generation_a, _a);
    const auto document_a = ready_a.document;

    const std::uint64_t generation_b = loader.RequestLoad(_b);
    Expect(generation_b == generation_a + 1, "request generation was not monotonic");
    auto ready_b = WaitForGeneration(loader, generation_b);
    ExpectReadyDocument(ready_b, generation_b, _b);
    Expect(ready_b.document != document_a, "successful replacement reused the old document");
    const auto document_b = ready_b.document;

    const std::uint64_t failed_generation = loader.RequestLoad(_missing);
    Expect(failed_generation == generation_b + 1, "failed request generation mismatch");
    const auto failed = WaitForGeneration(loader, failed_generation);
    Expect(failed.phase == ProfileDocumentLoadPhase::Failed, "missing file was not reported failed");
    Expect(
        failed.diagnostic == ProfileDocumentLoadDiagnostic::SessionLoadFailed,
        "missing file failure diagnostic mismatch"
    );
    Expect(failed.latest_attempt_path == _missing, "failed attempt path mismatch");
    Expect(failed.published_generation == generation_b, "failure changed published generation");
    Expect(failed.document == document_b, "failure discarded the last-good document");
    Expect(failed.document->Valid(), "last-good document became invalid after failure");
}

void TestCancellationPreservesLastGood(
    const std::filesystem::path& _good,
    const std::filesystem::path& _large
) {
    ProfileDocumentLoader loader;
    const std::uint64_t   good_generation = loader.RequestLoad(_good);
    const auto            ready           = WaitForGeneration(loader, good_generation);
    ExpectReadyDocument(ready, good_generation, _good);
    const auto last_good = ready.document;

    ProfileDocumentLoadOptions index_cancel_options;
    index_cancel_options.timeline_control.cancellation_check_interval  = 1;
    index_cancel_options.timeline_control.max_work_items_before_cancel = 1;

    const std::uint64_t index_cancel_generation = loader.RequestLoad(_large, index_cancel_options);
    Expect(index_cancel_generation != 0, "index cancellation request was rejected");
    const auto index_cancelled = WaitForGeneration(loader, index_cancel_generation);
    Expect(
        index_cancelled.phase == ProfileDocumentLoadPhase::Cancelled,
        "index work-budget cancellation did not reach Cancelled"
    );
    Expect(
        index_cancelled.session_result.HasUsableSession(),
        "index cancellation did not complete session materialization first"
    );
    Expect(
        index_cancelled.timeline_result.status == TimelineIndexBuildStatus::Cancelled,
        "index cancellation did not report a cancelled timeline build"
    );
    Expect(index_cancelled.document == last_good, "index cancellation discarded the last-good document");

    ProfileDocumentLoadOptions active_cancel_options;
    active_cancel_options.session_control.cancellation_check_interval  = 1;
    active_cancel_options.timeline_control.cancellation_check_interval = 1;
    const std::uint64_t active_cancel_generation = loader.RequestLoad(_large, active_cancel_options);
    Expect(active_cancel_generation != 0, "active cancellation request was rejected");
    static_cast<void>(WaitUntilActive(loader, active_cancel_generation));
    Expect(loader.CancelLatest(), "active request did not accept CancelLatest");
    const auto active_cancelled = WaitForGeneration(loader, active_cancel_generation);
    Expect(
        active_cancelled.phase == ProfileDocumentLoadPhase::Cancelled,
        "active CancelLatest did not reach Cancelled"
    );
    Expect(active_cancelled.document == last_good, "active CancelLatest discarded the last-good document");

    ProfileDocumentLoadOptions options;
    options.session_control.cancellation_check_interval  = 1;
    options.session_control.max_work_items_before_cancel = 1;

    const std::uint64_t cancelled_generation = loader.RequestLoad(_large, options);
    Expect(cancelled_generation != 0, "cancellation fixture request was rejected");
    static_cast<void>(loader.CancelLatest());
    const auto cancelled = WaitForGeneration(loader, cancelled_generation);
    Expect(
        cancelled.phase == ProfileDocumentLoadPhase::Cancelled, "cancelled attempt did not reach Cancelled"
    );
    Expect(cancelled.diagnostic == ProfileDocumentLoadDiagnostic::Cancelled, "cancel diagnostic mismatch");
    Expect(cancelled.document == last_good, "cancellation discarded the last-good document");
    Expect(cancelled.published_generation == good_generation, "cancellation changed published generation");

    std::stop_source external_stop;
    external_stop.request_stop();
    ProfileDocumentLoadOptions external_stop_options;
    external_stop_options.session_control.stop_token = external_stop.get_token();

    const std::uint64_t external_stop_generation = loader.RequestLoad(_large, external_stop_options);
    Expect(external_stop_generation != 0, "external-stop request was rejected");
    const auto external_stop_cancelled = WaitForGeneration(loader, external_stop_generation);
    Expect(
        external_stop_cancelled.phase == ProfileDocumentLoadPhase::Cancelled,
        "external session stop_token was not forwarded"
    );
    Expect(
        external_stop_cancelled.session_result.status == SessionLoadStatus::Cancelled,
        "external session stop_token did not cancel the reader"
    );
    Expect(
        external_stop_cancelled.document == last_good,
        "external stop_token cancellation discarded the last-good document"
    );
}

void TestTimelineFailurePreservesLastGood(
    const std::filesystem::path& _good,
    const std::filesystem::path& _candidate
) {
    ProfileDocumentLoader loader;
    const std::uint64_t   good_generation = loader.RequestLoad(_good);
    const auto            ready           = WaitForGeneration(loader, good_generation);
    const auto            last_good       = ready.document;

    ProfileDocumentLoadOptions options;
    options.timeline_limits.max_cpu_scopes = 0;
    const std::uint64_t failed_generation  = loader.RequestLoad(_candidate, options);
    const auto          failed             = WaitForGeneration(loader, failed_generation);
    Expect(failed.phase == ProfileDocumentLoadPhase::Failed, "timeline limit did not fail load");
    Expect(
        failed.diagnostic == ProfileDocumentLoadDiagnostic::TimelineBuildFailed,
        "timeline failure diagnostic mismatch"
    );
    Expect(failed.session_result.HasUsableSession(), "timeline failure lost usable session outcome");
    Expect(
        failed.timeline_result.status == TimelineIndexBuildStatus::LimitExceeded,
        "timeline failure did not report its limit outcome"
    );
    Expect(failed.document == last_good, "timeline failure discarded the last-good document");
    Expect(failed.published_generation == good_generation, "timeline failure changed published generation");

    ProfileDocumentLoadOptions preflight_options;
    preflight_options.session.limits.max_input_bytes = 1;
    const std::uint64_t preflight_generation         = loader.RequestLoad(_candidate, preflight_options);
    const auto          preflight                    = WaitForGeneration(loader, preflight_generation);
    Expect(preflight.phase == ProfileDocumentLoadPhase::Failed, "input preflight did not fail load");
    Expect(
        preflight.session_result.status == SessionLoadStatus::LimitExceeded &&
            preflight.session_result.limit_kind == SessionLimitKind::InputBytes,
        "input preflight did not preserve the reader limit outcome"
    );
    Expect(
        preflight.reported_input_bytes == std::filesystem::file_size(_candidate) &&
            preflight.reported_input_bytes == preflight.session_result.input_bytes,
        "preflight file size was mislabeled or lost in the loader snapshot"
    );
    Expect(preflight.document == last_good, "input preflight discarded the last-good document");
}

void TestCancelReadyArbitration(const std::filesystem::path& _capture) {
    ProfileDocumentLoader loader;
    const std::uint64_t   initial_generation = loader.RequestLoad(_capture);
    const auto            initial            = WaitForGeneration(loader, initial_generation);
    ExpectReadyDocument(initial, initial_generation, _capture);
    auto last_good = initial.document;

    for (std::uint32_t iteration = 0; iteration < 256; ++iteration) {
        const std::uint64_t generation = loader.RequestLoad(_capture);
        Expect(generation != 0, "cancel/ready arbitration request was rejected");
        const bool cancel_won = loader.CancelLatest();
        const auto terminal   = WaitForGeneration(loader, generation);

        if (cancel_won) {
            Expect(
                terminal.phase == ProfileDocumentLoadPhase::Cancelled,
                "successful CancelLatest raced into Ready"
            );
            Expect(
                terminal.published_generation == last_good->request_generation,
                "successful cancellation published a new generation"
            );
            Expect(terminal.document == last_good, "successful cancellation replaced last-good");
        } else {
            Expect(
                terminal.phase == ProfileDocumentLoadPhase::Ready,
                "lost CancelLatest race did not observe Ready"
            );
            ExpectReadyDocument(terminal, generation, _capture);
            last_good = terminal.document;
        }
    }
}

void TestRapidNewestWinsAndBoundedPending(
    const std::filesystem::path& _a,
    const std::filesystem::path& _b,
    const std::filesystem::path& _c
) {
    ProfileDocumentLoader loader;

    ProfileDocumentLoadOptions slow_options;
    slow_options.session_control.cancellation_check_interval  = 1;
    slow_options.timeline_control.cancellation_check_interval = 1;

    const std::uint64_t generation_a = loader.RequestLoad(_a, slow_options);
    const auto          after_a      = WaitUntilActive(loader, generation_a);
    Expect(after_a.request_generation == generation_a, "A progress generation mismatch");
    Expect(after_a.latest_attempt_path == _a, "A progress path mismatch");

    const std::uint64_t generation_b = loader.RequestLoad(_b);
    const auto          after_b      = loader.Snapshot();
    Expect(after_b.request_generation == generation_b, "B progress generation mismatch");
    Expect(after_b.latest_attempt_path == _b, "B progress path mismatch");

    const std::uint64_t generation_c = loader.RequestLoad(_c);
    const auto          after_c      = loader.Snapshot();
    Expect(after_c.request_generation == generation_c, "C progress generation mismatch");
    Expect(after_c.latest_attempt_path == _c, "C progress path mismatch");

    const auto ready_c = WaitForGeneration(loader, generation_c);
    ExpectReadyDocument(ready_c, generation_c, _c);
    Expect(
        ready_c.maximum_pending_request_count_observed == 1, "loader retained more than one pending request"
    );
    Expect(ready_c.pending_request_count == 0, "pending request remained after latest completion");
    Expect(ready_c.superseded_request_count >= 2, "rapid supersession was not accounted");

    // Give any stale active result time to reach its final publication gate.
    std::this_thread::sleep_for(50ms);
    const auto stable_c = loader.Snapshot();
    Expect(stable_c.request_generation == generation_c, "stale result changed latest generation");
    Expect(stable_c.published_generation == generation_c, "stale result replaced published generation");
    Expect(stable_c.document == ready_c.document, "stale result replaced newest document");
}

void TestShutdownClosesAdmissionAndKeepsSnapshot(
    const std::filesystem::path& _good,
    const std::filesystem::path& _large
) {
    ProfileDocumentLoader loader;
    const std::uint64_t   generation = loader.RequestLoad(_good);
    const auto            ready      = WaitForGeneration(loader, generation);
    const auto            last_good  = ready.document;

    ProfileDocumentLoadOptions options;
    options.session_control.cancellation_check_interval  = 1;
    options.timeline_control.cancellation_check_interval = 1;
    const std::uint64_t in_flight                        = loader.RequestLoad(_large, options);
    Expect(in_flight != 0, "shutdown fixture request was rejected");
    static_cast<void>(WaitUntilActive(loader, in_flight));

    loader.Shutdown();
    loader.Shutdown();
    const auto shutdown = loader.Snapshot();
    Expect(shutdown.phase == ProfileDocumentLoadPhase::Shutdown, "loader did not report Shutdown");
    Expect(!shutdown.accepting_requests, "shutdown loader still accepted requests");
    Expect(!shutdown.active_request, "shutdown returned before active work joined");
    Expect(shutdown.pending_request_count == 0, "shutdown retained a pending request");
    Expect(loader.RequestLoad(_good) == 0, "shutdown loader accepted a new request");
    Expect(shutdown.document == last_good, "shutdown discarded or replaced last-good document");
    Expect(last_good->Valid(), "shared last-good snapshot was invalid after shutdown");
}

} // namespace

int main(int _argc, char** _argv) {
    try {
        if (_argc == 2) {
            const std::filesystem::path capture = _argv[1];
            ProfileDocumentLoader       loader;
            const std::uint64_t         generation = loader.RequestLoad(capture);
            Expect(generation != 0, "external capture request was rejected");
            const auto ready = WaitForGeneration(loader, generation, 120s);
            ExpectReadyDocument(ready, generation, capture);
            std::cout << "Profile document loader external capture passed (cpu_scopes="
                      << ready.document->session.CpuScopes().size()
                      << ", gpu_scopes=" << ready.document->session.GpuScopes().size() << ")\n";
            return 0;
        }
        Expect(_argc == 1, "usage: TestProfileDocumentLoader [capture.mpd]");

        ScopedLoaderFixtures fixtures;
        const auto           capture_a       = fixtures.Path("capture-a-large");
        const auto           capture_b_large = fixtures.Path("capture-b-large");
        const auto           capture_b       = fixtures.Path("capture-b");
        const auto           capture_c       = fixtures.Path("capture-c");
        const auto           missing         = fixtures.Path("missing");

        WriteCapture(capture_a, "capture-a", 131'072);
        WriteCapture(capture_b_large, "capture-b-large", 131'072);
        WriteCapture(capture_b, "capture-b", 32);
        WriteCapture(capture_c, "capture-c", 48);

        TestSuccessReplacementAndFailurePreservation(capture_b, capture_c, missing);
        TestCancellationPreservesLastGood(capture_b, capture_a);
        TestTimelineFailurePreservesLastGood(capture_b, capture_c);
        TestCancelReadyArbitration(capture_c);
        TestRapidNewestWinsAndBoundedPending(capture_a, capture_b_large, capture_c);
        TestShutdownClosesAdmissionAndKeepsSnapshot(capture_b, capture_a);

        std::cout << "Profile document loader contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Profile document loader contract test failed: " << error.what() << '\n';
        return 1;
    }
}
