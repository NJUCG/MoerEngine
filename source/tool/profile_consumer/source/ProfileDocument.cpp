#include "profile_consumer/ProfileDocument.h"

#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace Moer::ProfileDump {

ProfileDocument::ProfileDocument(
    std::filesystem::path _source_path,
    std::uint64_t         _request_generation,
    SessionLoadResult     _load_result,
    ProfileSession        _session,
    ProfileTimelineIndex  _timeline_index
) noexcept :
    source_path(std::move(_source_path)),
    request_generation(_request_generation),
    load_result(_load_result),
    session(std::move(_session)),
    timeline_index(std::move(_timeline_index)) {}

bool ProfileDocument::Valid() const noexcept {
    return request_generation != kInvalidProfileDocumentGeneration && load_result.HasUsableSession() &&
           session.Valid() && timeline_index.Valid() && timeline_index.Matches(session);
}

struct ProfileDocumentLoader::Impl final {
    struct Request {
        std::filesystem::path      path{};
        ProfileDocumentLoadOptions options{};
        std::uint64_t              generation{kInvalidProfileDocumentGeneration};
    };

    mutable std::mutex            mutex{};
    std::mutex                    shutdown_mutex{};
    std::condition_variable       work_available{};
    std::optional<Request>        pending{};
    std::stop_source              active_stop{};
    bool                          active{false};
    bool                          accepting{true};
    std::uint64_t                 next_generation{kInvalidProfileDocumentGeneration};
    ProfileDocumentLoaderSnapshot snapshot{};
    std::jthread                  worker{};

    Impl() {
        snapshot.accepting_requests = true;
        try {
            worker = std::jthread([this](std::stop_token _worker_stop) noexcept {
                WorkerMain(_worker_stop);
            });
        } catch (...) {
            accepting                   = false;
            snapshot.accepting_requests = false;
            snapshot.phase              = ProfileDocumentLoadPhase::Failed;
            snapshot.diagnostic         = ProfileDocumentLoadDiagnostic::WorkerUnavailable;
        }
    }

    ~Impl() {
        Shutdown();
    }

    void UpdatePendingCountLocked() noexcept {
        snapshot.pending_request_count = pending.has_value() ? 1u : 0u;
        if (snapshot.pending_request_count > snapshot.maximum_pending_request_count_observed) {
            snapshot.maximum_pending_request_count_observed = snapshot.pending_request_count;
        }
    }

    [[nodiscard]] bool IsLatestLocked(std::uint64_t _generation) const noexcept {
        return accepting && snapshot.request_generation == _generation;
    }

    [[nodiscard]] static bool IsTerminalPhase(ProfileDocumentLoadPhase _phase) noexcept {
        return _phase == ProfileDocumentLoadPhase::Ready || _phase == ProfileDocumentLoadPhase::Failed ||
               _phase == ProfileDocumentLoadPhase::Cancelled || _phase == ProfileDocumentLoadPhase::Shutdown;
    }

    void CompleteActiveGenerationLocked(std::uint64_t _generation) noexcept {
        if (snapshot.active_generation != _generation) {
            return;
        }
        active                     = false;
        snapshot.active_request    = false;
        snapshot.active_generation = kInvalidProfileDocumentGeneration;
    }

    [[nodiscard]] bool CompleteCancellationIfRequestedLocked(
        std::stop_token                 _request_stop,
        const SessionLoadResult&        _session_result,
        const TimelineIndexBuildResult& _timeline_result
    ) noexcept {
        if (!snapshot.cancel_requested && !_request_stop.stop_requested()) {
            return false;
        }
        snapshot.phase                      = ProfileDocumentLoadPhase::Cancelled;
        snapshot.session_result             = _session_result;
        snapshot.timeline_result            = _timeline_result;
        snapshot.reported_input_bytes       = _session_result.input_bytes;
        snapshot.reported_input_bytes_final = true;
        snapshot.diagnostic                 = ProfileDocumentLoadDiagnostic::Cancelled;
        return true;
    }

    void BeginRequestLocked(std::uint64_t _generation, std::filesystem::path _latest_attempt_path) noexcept {
        snapshot.request_generation             = _generation;
        snapshot.phase                          = ProfileDocumentLoadPhase::Opening;
        snapshot.latest_attempt_path            = std::move(_latest_attempt_path);
        snapshot.observed_total_bytes_available = false;
        snapshot.observed_total_bytes           = 0;
        snapshot.reported_input_bytes           = 0;
        snapshot.reported_input_bytes_final     = false;
        snapshot.cancel_requested               = false;
        snapshot.session_result                 = {};
        snapshot.timeline_result                = {};
        snapshot.diagnostic                     = ProfileDocumentLoadDiagnostic::None;
    }

    void PublishOpeningSnapshot(
        const Request& _request,
        bool           _total_available,
        std::uint64_t  _total_bytes
    ) noexcept {
        std::lock_guard lock(mutex);
        if (!IsLatestLocked(_request.generation)) {
            return;
        }
        snapshot.phase                          = ProfileDocumentLoadPhase::Opening;
        snapshot.observed_total_bytes_available = _total_available;
        snapshot.observed_total_bytes           = _total_available ? _total_bytes : 0;
    }

    void PublishReadingSnapshot(const Request& _request) noexcept {
        std::lock_guard lock(mutex);
        if (!IsLatestLocked(_request.generation)) {
            return;
        }
        snapshot.phase                      = ProfileDocumentLoadPhase::Reading;
        snapshot.reported_input_bytes       = 0;
        snapshot.reported_input_bytes_final = false;
    }

    void PublishSessionResult(const Request& _request, const SessionLoadResult& _result) noexcept {
        std::lock_guard lock(mutex);
        if (!IsLatestLocked(_request.generation)) {
            return;
        }
        snapshot.session_result             = _result;
        snapshot.reported_input_bytes       = _result.input_bytes;
        snapshot.reported_input_bytes_final = true;
        if (_result.HasUsableSession()) {
            snapshot.phase = ProfileDocumentLoadPhase::BuildingTimeline;
        }
    }

    void CompleteCancelled(
        const Request&                  _request,
        const SessionLoadResult&        _session_result,
        const TimelineIndexBuildResult& _timeline_result
    ) noexcept {
        std::lock_guard lock(mutex);
        if (!IsLatestLocked(_request.generation)) {
            return;
        }
        snapshot.phase                      = ProfileDocumentLoadPhase::Cancelled;
        snapshot.session_result             = _session_result;
        snapshot.timeline_result            = _timeline_result;
        snapshot.reported_input_bytes       = _session_result.input_bytes;
        snapshot.reported_input_bytes_final = true;
        snapshot.diagnostic                 = ProfileDocumentLoadDiagnostic::Cancelled;
        CompleteActiveGenerationLocked(_request.generation);
    }

    void CompleteFailed(
        const Request&                  _request,
        const SessionLoadResult&        _session_result,
        const TimelineIndexBuildResult& _timeline_result,
        std::stop_token                 _request_stop
    ) noexcept {
        std::lock_guard lock(mutex);
        if (!IsLatestLocked(_request.generation)) {
            return;
        }
        if (CompleteCancellationIfRequestedLocked(_request_stop, _session_result, _timeline_result)) {
            CompleteActiveGenerationLocked(_request.generation);
            return;
        }
        snapshot.phase                      = ProfileDocumentLoadPhase::Failed;
        snapshot.session_result             = _session_result;
        snapshot.timeline_result            = _timeline_result;
        snapshot.reported_input_bytes       = _session_result.input_bytes;
        snapshot.reported_input_bytes_final = true;
        snapshot.diagnostic                 = _session_result.HasUsableSession() ?
                                                  ProfileDocumentLoadDiagnostic::TimelineBuildFailed :
                                                  ProfileDocumentLoadDiagnostic::SessionLoadFailed;
        CompleteActiveGenerationLocked(_request.generation);
    }

    void CompleteUnexpectedFailure(
        const Request&                  _request,
        const SessionLoadResult&        _session_result,
        const TimelineIndexBuildResult& _timeline_result,
        std::stop_token                 _request_stop
    ) noexcept {
        std::lock_guard lock(mutex);
        if (!IsLatestLocked(_request.generation)) {
            return;
        }
        if (CompleteCancellationIfRequestedLocked(_request_stop, _session_result, _timeline_result)) {
            CompleteActiveGenerationLocked(_request.generation);
            return;
        }
        snapshot.phase                      = ProfileDocumentLoadPhase::Failed;
        snapshot.session_result             = _session_result;
        snapshot.timeline_result            = _timeline_result;
        snapshot.reported_input_bytes       = _session_result.input_bytes;
        snapshot.reported_input_bytes_final = true;
        snapshot.diagnostic                 = ProfileDocumentLoadDiagnostic::DocumentAllocationFailed;
        CompleteActiveGenerationLocked(_request.generation);
    }

    void CompleteReady(
        const Request&                         _request,
        const SessionLoadResult&               _session_result,
        const TimelineIndexBuildResult&        _timeline_result,
        std::shared_ptr<const ProfileDocument> _document,
        std::stop_token                        _request_stop
    ) noexcept {
        std::shared_ptr<const ProfileDocument> retired_document;
        {
            std::lock_guard lock(mutex);
            if (!IsLatestLocked(_request.generation)) {
                return;
            }
            if (CompleteCancellationIfRequestedLocked(_request_stop, _session_result, _timeline_result)) {
                CompleteActiveGenerationLocked(_request.generation);
                return;
            }
            snapshot.phase                      = ProfileDocumentLoadPhase::Ready;
            snapshot.session_result             = _session_result;
            snapshot.timeline_result            = _timeline_result;
            snapshot.reported_input_bytes       = _session_result.input_bytes;
            snapshot.reported_input_bytes_final = true;
            snapshot.cancel_requested           = false;
            snapshot.published_generation       = _request.generation;
            // Publication is O(1) under the state mutex. The previous owning
            // document is released after new coherent snapshots are visible.
            retired_document    = std::exchange(snapshot.document, std::move(_document));
            snapshot.diagnostic = ProfileDocumentLoadDiagnostic::None;
            CompleteActiveGenerationLocked(_request.generation);
        }
    }

    void
    ProcessRequest(const Request& _request, std::stop_source _request_stop, std::stop_token _worker_stop) {
        const auto forward_stop = [&_request_stop]() noexcept {
            static_cast<void>(_request_stop.request_stop());
        };
        std::stop_callback worker_callback(_worker_stop, forward_stop);
        std::stop_callback session_callback(_request.options.session_control.stop_token, forward_stop);
        std::stop_callback timeline_callback(_request.options.timeline_control.stop_token, forward_stop);

        if (_request_stop.stop_requested()) {
            SessionLoadResult cancelled;
            cancelled.status     = SessionLoadStatus::Cancelled;
            cancelled.error_code = SessionErrorCode::Cancelled;
            CompleteCancelled(_request, cancelled, {});
            return;
        }

        std::error_code      file_size_error;
        const std::uintmax_t file_size = std::filesystem::file_size(_request.path, file_size_error);
        const bool           total_available =
            !file_size_error &&
            file_size <= static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max());
        PublishOpeningSnapshot(
            _request, total_available, total_available ? static_cast<std::uint64_t>(file_size) : 0
        );

        PublishReadingSnapshot(_request);

        ProfileSession session;
        auto           session_control = _request.options.session_control;
        session_control.stop_token     = _request_stop.get_token();
        const SessionLoadResult session_result =
            LoadProfileSessionFile(_request.path, _request.options.session, session, session_control);
        PublishSessionResult(_request, session_result);

        if (session_result.status == SessionLoadStatus::Cancelled || _request_stop.stop_requested()) {
            auto cancelled_result = session_result;
            if (cancelled_result.status != SessionLoadStatus::Cancelled) {
                cancelled_result.status     = SessionLoadStatus::Cancelled;
                cancelled_result.error_code = SessionErrorCode::Cancelled;
            }
            CompleteCancelled(_request, cancelled_result, {});
            return;
        }
        if (!session_result.HasUsableSession()) {
            CompleteFailed(_request, session_result, {}, _request_stop.get_token());
            return;
        }

        ProfileTimelineIndex timeline_index;
        auto                 timeline_control          = _request.options.timeline_control;
        timeline_control.stop_token                    = _request_stop.get_token();
        const TimelineIndexBuildResult timeline_result = BuildProfileTimelineIndex(
            session, session_result, _request.options.timeline_limits, timeline_index, timeline_control
        );

        if (timeline_result.status == TimelineIndexBuildStatus::Cancelled || _request_stop.stop_requested()) {
            CompleteCancelled(_request, session_result, timeline_result);
            return;
        }
        if (!timeline_result.Succeeded()) {
            CompleteFailed(_request, session_result, timeline_result, _request_stop.get_token());
            return;
        }

        try {
            auto document = std::make_shared<const ProfileDocument>(
                _request.path,
                _request.generation,
                session_result,
                std::move(session),
                std::move(timeline_index)
            );
            CompleteReady(
                _request, session_result, timeline_result, std::move(document), _request_stop.get_token()
            );
        } catch (...) {
            CompleteUnexpectedFailure(_request, session_result, timeline_result, _request_stop.get_token());
        }
    }

    void WorkerMainImpl(std::stop_token _worker_stop) {
        for (;;) {
            Request          request;
            std::stop_source request_stop;
            {
                std::unique_lock lock(mutex);
                work_available.wait(lock, [this, _worker_stop]() noexcept {
                    return _worker_stop.stop_requested() || !accepting || pending.has_value();
                });
                if (_worker_stop.stop_requested() || !accepting) {
                    active                     = false;
                    snapshot.active_request    = false;
                    snapshot.active_generation = kInvalidProfileDocumentGeneration;
                    return;
                }

                request = std::move(*pending);
                pending.reset();
                UpdatePendingCountLocked();

                active_stop                = request_stop;
                active                     = true;
                snapshot.active_request    = true;
                snapshot.active_generation = request.generation;
                if (snapshot.request_generation == request.generation) {
                    snapshot.phase = ProfileDocumentLoadPhase::Opening;
                }
            }

            ProcessRequest(request, request_stop, _worker_stop);

            {
                std::lock_guard lock(mutex);
                if (snapshot.active_generation == request.generation) {
                    active                     = false;
                    snapshot.active_request    = false;
                    snapshot.active_generation = kInvalidProfileDocumentGeneration;
                }
            }
        }
    }

    void WorkerMain(std::stop_token _worker_stop) noexcept {
        try {
            WorkerMainImpl(_worker_stop);
        } catch (...) {
            // A worker-control allocation or wait failure must not escape the
            // jthread entry point. Preserve the last-good document and close
            // admission because this worker can no longer service requests.
            try {
                std::lock_guard lock(mutex);
                if (!accepting) {
                    return;
                }
                accepting                           = false;
                snapshot.accepting_requests         = false;
                snapshot.phase                      = ProfileDocumentLoadPhase::Failed;
                snapshot.session_result             = {};
                snapshot.session_result.status      = SessionLoadStatus::ResourceExhausted;
                snapshot.session_result.error_code  = SessionErrorCode::ResourceAllocationFailed;
                snapshot.timeline_result            = {};
                snapshot.reported_input_bytes       = 0;
                snapshot.reported_input_bytes_final = true;
                snapshot.cancel_requested           = false;
                snapshot.diagnostic                 = ProfileDocumentLoadDiagnostic::WorkerUnavailable;
                pending.reset();
                UpdatePendingCountLocked();
                static_cast<void>(active_stop.request_stop());
                active                     = false;
                snapshot.active_request    = false;
                snapshot.active_generation = kInvalidProfileDocumentGeneration;
            } catch (...) {
            }
        }
    }

    void Shutdown() noexcept {
        std::lock_guard shutdown_lock(shutdown_mutex);
        {
            std::lock_guard lock(mutex);
            accepting                   = false;
            snapshot.accepting_requests = false;
            snapshot.phase              = ProfileDocumentLoadPhase::Shutdown;
            pending.reset();
            UpdatePendingCountLocked();
            static_cast<void>(active_stop.request_stop());
            if (worker.joinable()) {
                worker.request_stop();
            }
        }
        work_available.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        {
            std::lock_guard lock(mutex);
            active                     = false;
            snapshot.active_request    = false;
            snapshot.active_generation = kInvalidProfileDocumentGeneration;
            snapshot.phase             = ProfileDocumentLoadPhase::Shutdown;
        }
    }
};

ProfileDocumentLoader::ProfileDocumentLoader() noexcept {
    try {
        impl_ = std::make_unique<Impl>();
    } catch (...) {
        impl_.reset();
    }
}

ProfileDocumentLoader::~ProfileDocumentLoader() {
    Shutdown();
}

std::uint64_t ProfileDocumentLoader::RequestLoad(
    const std::filesystem::path&      _path,
    const ProfileDocumentLoadOptions& _options
) {
    if (impl_ == nullptr) {
        return kInvalidProfileDocumentGeneration;
    }

    Impl::Request request{
        .path    = _path,
        .options = _options,
    };
    std::filesystem::path latest_attempt_path = _path;

    std::lock_guard lock(impl_->mutex);
    if (!impl_->accepting) {
        return kInvalidProfileDocumentGeneration;
    }
    if (impl_->next_generation == std::numeric_limits<std::uint64_t>::max()) {
        impl_->snapshot.generation_exhausted = true;
        return kInvalidProfileDocumentGeneration;
    }

    request.generation = impl_->next_generation + 1;
    if (impl_->active || impl_->pending.has_value()) {
        ++impl_->snapshot.superseded_request_count;
    }
    if (impl_->active) {
        static_cast<void>(impl_->active_stop.request_stop());
    }
    impl_->pending         = std::move(request);
    impl_->next_generation = impl_->pending->generation;
    impl_->BeginRequestLocked(impl_->pending->generation, std::move(latest_attempt_path));
    impl_->UpdatePendingCountLocked();
    const std::uint64_t generation = impl_->next_generation;
    impl_->work_available.notify_one();
    return generation;
}

bool ProfileDocumentLoader::CancelLatest() noexcept {
    if (impl_ == nullptr) {
        return false;
    }

    std::lock_guard lock(impl_->mutex);
    if (!impl_->accepting || impl_->snapshot.request_generation == kInvalidProfileDocumentGeneration) {
        return false;
    }

    const std::uint64_t generation = impl_->snapshot.request_generation;
    if (Impl::IsTerminalPhase(impl_->snapshot.phase)) {
        return false;
    }
    if (impl_->pending.has_value() && impl_->pending->generation == generation) {
        impl_->pending.reset();
        impl_->UpdatePendingCountLocked();
        impl_->snapshot.phase                      = ProfileDocumentLoadPhase::Cancelled;
        impl_->snapshot.cancel_requested           = true;
        impl_->snapshot.reported_input_bytes_final = true;
        impl_->snapshot.session_result.status      = SessionLoadStatus::Cancelled;
        impl_->snapshot.session_result.error_code  = SessionErrorCode::Cancelled;
        impl_->snapshot.diagnostic                 = ProfileDocumentLoadDiagnostic::Cancelled;
        return true;
    }
    if (impl_->active && impl_->snapshot.active_generation == generation) {
        impl_->snapshot.cancel_requested = true;
        static_cast<void>(impl_->active_stop.request_stop());
        return true;
    }
    return false;
}

ProfileDocumentLoaderSnapshot ProfileDocumentLoader::Snapshot() const {
    if (impl_ == nullptr) {
        ProfileDocumentLoaderSnapshot unavailable;
        unavailable.phase      = ProfileDocumentLoadPhase::Failed;
        unavailable.diagnostic = ProfileDocumentLoadDiagnostic::WorkerUnavailable;
        return unavailable;
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot;
}

void ProfileDocumentLoader::Shutdown() noexcept {
    if (impl_ != nullptr) {
        impl_->Shutdown();
    }
}

} // namespace Moer::ProfileDump
