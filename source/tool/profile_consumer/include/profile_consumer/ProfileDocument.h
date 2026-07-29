#ifndef MOER_ENGINE_PROFILE_DOCUMENT_H
#define MOER_ENGINE_PROFILE_DOCUMENT_H

#include "profile_consumer/ProfileSession.h"
#include "profile_consumer/ProfileTimelineIndex.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace Moer::ProfileDump {

inline constexpr std::uint64_t kInvalidProfileDocumentGeneration = 0;

// One immutable, self-contained publication unit. Field order is intentional:
// timeline_index is destroyed before session, whose immutable model it indexes.
struct ProfileDocument final {
    std::filesystem::path source_path{};
    std::uint64_t         request_generation{kInvalidProfileDocumentGeneration};
    SessionLoadResult     load_result{};
    ProfileSession        session{};
    ProfileTimelineIndex  timeline_index{};

    ProfileDocument(
        std::filesystem::path _source_path,
        std::uint64_t         _request_generation,
        SessionLoadResult     _load_result,
        ProfileSession        _session,
        ProfileTimelineIndex  _timeline_index
    ) noexcept;

    ProfileDocument(const ProfileDocument&)            = delete;
    ProfileDocument& operator=(const ProfileDocument&) = delete;
    ProfileDocument(ProfileDocument&&)                 = delete;
    ProfileDocument& operator=(ProfileDocument&&)      = delete;

    [[nodiscard]] bool Valid() const noexcept;
};

enum class ProfileDocumentLoadPhase : std::uint8_t {
    Idle = 0,
    Opening,
    Reading,
    // Reserved for a future reader progress hook. The current whole-file API
    // materializes during Reading, so the loader does not invent this phase.
    MaterializingSession,
    BuildingTimeline,
    Ready,
    Failed,
    Cancelled,
    Shutdown,
};

enum class ProfileDocumentLoadDiagnostic : std::uint8_t {
    None = 0,
    WorkerUnavailable,
    Cancelled,
    SessionLoadFailed,
    TimelineBuildFailed,
    DocumentAllocationFailed,
};

struct ProfileDocumentLoadOptions {
    SessionLoadOptions        session{};
    TimelineIndexLimits       timeline_limits{};
    SessionLoadControl        session_control{};
    TimelineIndexBuildControl timeline_control{};
};

struct ProfileDocumentLoaderSnapshot {
    // Every progress/result field in this snapshot belongs to this generation.
    // The last-good document may intentionally have an older generation after
    // the latest attempt fails, is cancelled, or is still in progress.
    std::uint64_t            request_generation{kInvalidProfileDocumentGeneration};
    ProfileDocumentLoadPhase phase{ProfileDocumentLoadPhase::Idle};
    std::filesystem::path    latest_attempt_path{};

    // observed_total_bytes is a best-effort filesystem metadata observation
    // made before the synchronous reader starts; it is not the reader's
    // internal file-size snapshot. The reader currently has no streaming
    // progress callback: while Reading, reported_input_bytes remains zero and
    // reported_input_bytes_final is false. Once terminal, this is the exact
    // SessionLoadResult::input_bytes field; for a file-size preflight rejection
    // it therefore reports the snapshotted size, not bytes physically read.
    // Consumers must render Reading as indeterminate rather than deriving a
    // percentage from the observation.
    bool          observed_total_bytes_available{false};
    std::uint64_t observed_total_bytes{0};
    std::uint64_t reported_input_bytes{0};
    bool          reported_input_bytes_final{false};

    // Worker/admission lifecycle only. Generation exhaustion is reported
    // separately and never cancels an already accepted maximum generation.
    bool          accepting_requests{false};
    bool          generation_exhausted{false};
    bool          cancel_requested{false};
    bool          active_request{false};
    std::uint64_t active_generation{kInvalidProfileDocumentGeneration};
    std::uint32_t pending_request_count{0};
    std::uint32_t maximum_pending_request_count_observed{0};
    std::uint64_t superseded_request_count{0};

    SessionLoadResult             session_result{};
    TimelineIndexBuildResult      timeline_result{};
    ProfileDocumentLoadDiagnostic diagnostic{ProfileDocumentLoadDiagnostic::None};

    std::uint64_t                          published_generation{kInvalidProfileDocumentGeneration};
    std::shared_ptr<const ProfileDocument> document{};
};

// A bounded latest-request-wins loader. It owns one persistent worker thread,
// at most one pending request, and one active request. Publication and all
// status fields are copied under the same mutex by Snapshot().
class ProfileDocumentLoader final {
public:
    ProfileDocumentLoader() noexcept;
    ~ProfileDocumentLoader();

    ProfileDocumentLoader(const ProfileDocumentLoader&)            = delete;
    ProfileDocumentLoader& operator=(const ProfileDocumentLoader&) = delete;
    ProfileDocumentLoader(ProfileDocumentLoader&&)                 = delete;
    ProfileDocumentLoader& operator=(ProfileDocumentLoader&&)      = delete;

    // Returns zero after admission has closed or generation space is exhausted.
    // A newer accepted request cooperatively cancels active work and replaces
    // the single pending slot.
    [[nodiscard]] std::uint64_t
    RequestLoad(const std::filesystem::path& _path, const ProfileDocumentLoadOptions& _options = {});

    // Requests cancellation of the latest non-terminal attempt. Cancellation
    // never clears or mutates the last successfully published document.
    [[nodiscard]] bool CancelLatest() noexcept;

    [[nodiscard]] ProfileDocumentLoaderSnapshot Snapshot() const;

    // Idempotent. Admission closes before pending/active work is cancelled.
    // The worker is joined without holding the state mutex.
    void Shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

} // namespace Moer::ProfileDump

#endif // MOER_ENGINE_PROFILE_DOCUMENT_H
