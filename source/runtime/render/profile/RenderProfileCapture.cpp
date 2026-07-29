#include "profile/RenderProfileCapture.h"

#include "profile/ProfileDumpTemplates.h"
#include "rhi/RHICommand.h"

#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Moer::Render {

namespace {

enum class EmitDisposition : std::uint8_t {
    Accepted,
    Dropped,
    Fatal,
};

#if defined(MOER_RENDER_PROFILE_CAPTURE_TEST_HOOKS)
enum class InjectedStartValidationException : std::uint8_t {
    None = 0,
    BadAlloc,
    Other,
};

struct StartValidationExceptionForTesting final {};

std::atomic<InjectedStartValidationException> g_start_validation_exception{
    InjectedStartValidationException::None
};
std::atomic<std::atomic_uint32_t*> g_start_publish_pause_entered{nullptr};
std::atomic<std::atomic_bool*>     g_start_publish_pause_release{nullptr};

void ThrowInjectedStartValidationExceptionForTesting() {
    switch (g_start_validation_exception.exchange(
        InjectedStartValidationException::None,
        std::memory_order_acq_rel
    )) {
        case InjectedStartValidationException::BadAlloc:
            throw std::bad_alloc{};
        case InjectedStartValidationException::Other:
            throw StartValidationExceptionForTesting{};
        case InjectedStartValidationException::None:
            return;
    }
}

void PauseBeforeStartPublishForTesting() noexcept {
    std::atomic_uint32_t* entered =
        g_start_publish_pause_entered.exchange(nullptr, std::memory_order_acq_rel);
    if (entered == nullptr) {
        return;
    }
    entered->fetch_add(1, std::memory_order_release);
    std::atomic_bool* release = g_start_publish_pause_release.load(std::memory_order_acquire);
    while (release != nullptr && !release->load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}
#endif

bool IsManagedQueue(const RHIQueueBinding& _binding) noexcept {
    return _binding.available &&
           (_binding.queue == EQueueType::Graphics || _binding.queue == EQueueType::Compute ||
            _binding.queue == EQueueType::Copy);
}

RenderProfileSessionStartResult ValidateSessionStart(
    ProfileDump::SchemaHandle _gpu_frame_schema,
    ProfileDump::SchemaHandle _gpu_scope_schema
) noexcept {
    try {
#if defined(MOER_RENDER_PROFILE_CAPTURE_TEST_HOOKS)
        ThrowInjectedStartValidationExceptionForTesting();
#endif
        // GpuFrame()/GpuScope() use function-local static descriptors whose
        // first construction allocates strings and field arrays. Keep that
        // lazy initialization inside this noexcept translation boundary.
        const std::uint64_t expected_frame_hash =
            ProfileDump::ComputeSchemaHash(ProfileDump::Templates::GpuFrame());
        const std::uint64_t expected_scope_hash =
            ProfileDump::ComputeSchemaHash(ProfileDump::Templates::GpuScope());
        if (!_gpu_frame_schema || !_gpu_scope_schema ||
            _gpu_frame_schema.hash != expected_frame_hash ||
            _gpu_scope_schema.hash != expected_scope_hash) {
            return RenderProfileSessionStartResult::InvalidSchema;
        }

        const std::uint64_t generation_before =
            ProfileDump::GetRuntimeGeneration();
        const ProfileDump::RuntimeState runtime_state =
            ProfileDump::GetRuntimeState();
        const std::uint64_t generation_after =
            ProfileDump::GetRuntimeGeneration();
        if (runtime_state != ProfileDump::RuntimeState::Running ||
            generation_before == 0 ||
            generation_before != generation_after) {
            return RenderProfileSessionStartResult::RuntimeUnavailable;
        }
        if (_gpu_frame_schema.generation != generation_after ||
            _gpu_scope_schema.generation != generation_after) {
            return RenderProfileSessionStartResult::StaleGeneration;
        }
        return RenderProfileSessionStartResult::Started;
    } catch (const std::bad_alloc&) {
        return RenderProfileSessionStartResult::ResourceExhausted;
    } catch (...) {
        return RenderProfileSessionStartResult::InvalidConfiguration;
    }
}

} // namespace

namespace RenderProfileDetail {

enum class SessionTerminal : std::uint8_t {
    None = 0,
    Closed,
    ClosedWithLoss,
    Faulted,
    Aborted,
};

struct FrameMetadata {
    std::uint64_t frame_id{0};
    std::uint64_t source_bind_failures{0};
};

struct CaptureState {
    CaptureState(
        ProfileDump::SchemaHandle   _gpu_frame_schema,
        ProfileDump::SchemaHandle   _gpu_scope_schema,
        const GpuScopeStreamConfig& _stream_config
    ) :
        gpu_frame_schema(_gpu_frame_schema),
        gpu_scope_schema(_gpu_scope_schema),
        generation(_gpu_frame_schema.generation),
        stream(_stream_config),
        frame_metadata(_stream_config.max_resident_frames) {}

    [[nodiscard]] bool RuntimeWritableUnlocked() const noexcept {
        const std::uint64_t             generation_before = ProfileDump::GetRuntimeGeneration();
        const ProfileDump::RuntimeState runtime_state     = ProfileDump::GetRuntimeState();
        const std::uint64_t             generation_after  = ProfileDump::GetRuntimeGeneration();
        return generation != 0 && generation_before == generation && generation_after == generation &&
               runtime_state == ProfileDump::RuntimeState::Running;
    }

    [[nodiscard]] bool HasLossUnlocked() const noexcept {
        const GpuScopeStreamStats stream_stats = stream.GetStats();
        return frames_admission_rejected != 0 || sources_rejected != 0 ||
               frame_records_dropped != 0 || scope_records_dropped != 0 ||
               stream_stats.sources_dropped_capacity != 0 ||
               stream_stats.sources_dropped_duplicate_order != 0 ||
               stream_stats.sources_dropped_after_seal != 0 ||
               stream_stats.scopes_dropped_resident_full != 0 ||
               stream_stats.scopes_dropped_frame_full != 0 ||
               stream_stats.scopes_dropped_name_too_large != 0 ||
               stream_stats.scopes_dropped_invalid_hierarchy != 0 ||
               stream_stats.scopes_dropped_suppressed_subtree != 0 ||
               stream_stats.scopes_dropped_after_seal != 0 ||
               stream_stats.scopes_dropped_resource_exhaustion != 0;
    }

    void ClearMetadataUnlocked() noexcept {
        for (FrameMetadata& metadata : frame_metadata) {
            metadata = {};
        }
    }

    [[nodiscard]] bool MetadataEmptyUnlocked() const noexcept {
        for (const FrameMetadata& metadata : frame_metadata) {
            if (metadata.frame_id != 0) {
                return false;
            }
        }
        return true;
    }

    void CloseUnlocked(SessionTerminal _terminal) noexcept {
        if (closed) {
            return;
        }
        accepting = false;
        closed    = true;
        terminal  = _terminal;
        if (_terminal == SessionTerminal::Faulted) {
            ++terminal_faults;
        }
        const GpuScopeStreamStats before_close = stream.GetStats();
        shutdown_abandoned_frames += before_close.resident_frames;
        stream.Close();
        ClearMetadataUnlocked();
    }

    [[nodiscard]] bool ReserveMetadataUnlocked(std::uint64_t _frame_id) noexcept {
        for (FrameMetadata& metadata : frame_metadata) {
            if (metadata.frame_id == 0) {
                metadata.frame_id = _frame_id;
                return true;
            }
        }
        return false;
    }

    void ReleaseMetadataUnlocked(std::uint64_t _frame_id) noexcept {
        for (FrameMetadata& metadata : frame_metadata) {
            if (metadata.frame_id == _frame_id) {
                metadata = {};
                return;
            }
        }
    }

    [[nodiscard]] FrameMetadata TakeMetadataUnlocked(std::uint64_t _frame_id) noexcept {
        for (FrameMetadata& metadata : frame_metadata) {
            if (metadata.frame_id == _frame_id) {
                const FrameMetadata result = metadata;
                metadata                   = {};
                return result;
            }
        }
        return {};
    }

    void MarkSourceFailureUnlocked(std::uint64_t _frame_id) noexcept {
        ++sources_rejected;
        for (FrameMetadata& metadata : frame_metadata) {
            if (metadata.frame_id == _frame_id) {
                ++metadata.source_bind_failures;
                return;
            }
        }
    }

    [[nodiscard]] EmitDisposition
    HandleEmitUnlocked(ProfileDump::EmitStatus _status, bool _frame_record) noexcept {
        switch (_status) {
            case ProfileDump::EmitStatus::Accepted:
                if (_frame_record) {
                    ++frames_emitted;
                } else {
                    ++scopes_emitted;
                }
                return EmitDisposition::Accepted;
            case ProfileDump::EmitStatus::QueueFull:
                if (_frame_record) {
                    ++frame_records_dropped;
                } else {
                    ++scope_records_dropped;
                }
                return EmitDisposition::Dropped;
            case ProfileDump::EmitStatus::Disabled:
            case ProfileDump::EmitStatus::InvalidHandle:
            case ProfileDump::EmitStatus::ValueCountMismatch:
            case ProfileDump::EmitStatus::ValueTypeMismatch:
            case ProfileDump::EmitStatus::StringTooLarge:
            case ProfileDump::EmitStatus::RecordTooLarge:
            case ProfileDump::EmitStatus::SinkFault:
                CloseUnlocked(SessionTerminal::Faulted);
                return EmitDisposition::Fatal;
        }
        CloseUnlocked(SessionTerminal::Faulted);
        return EmitDisposition::Fatal;
    }

    [[nodiscard]] EmitDisposition EmitFrameRecordUnlocked(
        std::uint64_t         _frame_id,
        GpuFrameCaptureStatus _status,
        bool                  _valid,
        std::uint64_t         _admitted_scope_count,
        std::uint64_t         _dropped_scope_count,
        std::uint64_t         _error_scope_count,
        std::string_view      _reason
    ) noexcept {
        const std::array<ProfileDump::FieldValueView, 7> values{
            _frame_id,
            static_cast<std::uint32_t>(_status),
            _valid,
            _admitted_scope_count,
            _dropped_scope_count,
            _error_scope_count,
            _reason,
        };
        return HandleEmitUnlocked(ProfileDump::Emit(gpu_frame_schema, values), true);
    }

    [[nodiscard]] EmitDisposition
    EmitScopeRecordUnlocked(std::uint64_t _frame_id, const GpuScopeNode& _scope) noexcept {
        const std::array<ProfileDump::FieldValueView, 18> values{
            _frame_id,
            _scope.scope_id,
            _scope.parent_scope_id,
            _scope.source_order,
            _scope.local_order,
            static_cast<std::uint32_t>(_scope.queue_binding.queue),
            _scope.queue_binding.native_queue_id,
            _scope.queue_binding.family_id,
            std::string_view(_scope.name),
            static_cast<std::uint32_t>(_scope.status),
            _scope.begin_tick,
            _scope.end_tick,
            _scope.valid_bits,
            _scope.tick_period_ns,
            _scope.total_duration_ns,
            _scope.exclusive_duration_ns,
            _scope.depth,
            std::string_view(_scope.error_reason),
        };
        return HandleEmitUnlocked(ProfileDump::Emit(gpu_scope_schema, values), false);
    }

    [[nodiscard]] bool EmitScopeTreeUnlocked(std::uint64_t _frame_id, const GpuScopeNode& _scope) noexcept {
        if (EmitScopeRecordUnlocked(_frame_id, _scope) == EmitDisposition::Fatal) {
            return false;
        }
        for (const GpuScopeNode& child : _scope.children) {
            if (!EmitScopeTreeUnlocked(_frame_id, child)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool EmitResolvedFrameUnlocked(const ResolvedGpuScopeFrame& _frame) noexcept {
        const FrameMetadata metadata          = TakeMetadataUnlocked(_frame.frame_id);
        const bool          bridge_incomplete = metadata.source_bind_failures != 0;
        const bool          valid             = _frame.valid && !bridge_incomplete;

        GpuFrameCaptureStatus capture_status = GpuFrameCaptureStatus::Complete;
        std::string_view      reason{};
        if (!valid) {
            ++frames_invalid;
            const bool has_errors = _frame.error_scope_count != 0;
            const bool has_drops  = _frame.dropped_scope_count != 0 || bridge_incomplete;
            if (has_errors || (!has_drops && !_frame.valid)) {
                capture_status = GpuFrameCaptureStatus::Invalid;
            } else {
                capture_status = GpuFrameCaptureStatus::Incomplete;
            }

            if (has_errors && has_drops) {
                reason = "GPU frame capture contains failed and dropped scopes";
            } else if (has_errors) {
                reason = "one or more GPU scopes failed";
            } else if (bridge_incomplete && _frame.dropped_scope_count != 0) {
                reason = "GPU recording sources and scopes were dropped";
            } else if (bridge_incomplete) {
                reason = "one or more GPU recording sources were rejected";
            } else if (_frame.dropped_scope_count != 0) {
                reason = "one or more GPU scopes were dropped";
            } else {
                reason = "GPU scope topology validation failed";
            }
        }

        if (EmitFrameRecordUnlocked(
                _frame.frame_id,
                capture_status,
                valid,
                _frame.admitted_scope_count,
                _frame.dropped_scope_count,
                _frame.error_scope_count,
                reason
            ) == EmitDisposition::Fatal) {
            return false;
        }

        for (const auto& queue_roots : _frame.queue_roots) {
            for (const GpuScopeNode& root : queue_roots) {
                if (!EmitScopeTreeUnlocked(_frame.frame_id, root)) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] std::size_t DrainUnlocked() noexcept {
        if (closed || !RuntimeWritableUnlocked()) {
            if (!closed) {
                CloseUnlocked(SessionTerminal::Faulted);
            }
            return 0;
        }

        std::size_t drained = 0;
        try {
            ResolvedGpuScopeFrame frame{};
            while (!closed && stream.TryPopFrame(frame)) {
                ++drained;
                if (!EmitResolvedFrameUnlocked(frame)) {
                    break;
                }
                frame = {};
            }
        } catch (...) {
            CloseUnlocked(SessionTerminal::Faulted);
        }
        return drained;
    }

    ProfileDump::SchemaHandle  gpu_frame_schema{};
    ProfileDump::SchemaHandle  gpu_scope_schema{};
    std::uint64_t              generation{0};
    GpuScopeStream             stream;
    std::vector<FrameMetadata> frame_metadata{};

    mutable std::mutex mutex{};
    std::uint64_t      next_frame_id{1};
    bool               accepting{true};
    bool               stop_requested{false};
    bool               closed{false};
    SessionTerminal    terminal{SessionTerminal::None};

    std::uint64_t frames_attempted{0};
    std::uint64_t frames_admitted{0};
    std::uint64_t frames_admission_rejected{0};
    std::uint64_t frames_sealed{0};
    std::uint64_t frames_emitted{0};
    std::uint64_t frames_invalid{0};
    std::uint64_t frame_records_dropped{0};
    std::uint64_t sources_bound{0};
    std::uint64_t sources_rejected{0};
    std::uint64_t scopes_emitted{0};
    std::uint64_t scope_records_dropped{0};
    std::uint64_t terminal_faults{0};
    std::uint64_t shutdown_abandoned_frames{0};
};

} // namespace RenderProfileDetail

RenderProfileFrameToken::RenderProfileFrameToken(
    std::shared_ptr<RenderProfileDetail::CaptureState> _state,
    GpuScopeFrameHandle                                _frame,
    std::uint64_t                                      _frame_id
) noexcept :
    state_(std::move(_state)),
    frame_(std::move(_frame)),
    frame_id_(_frame_id) {}

RenderProfileFrameToken::~RenderProfileFrameToken() {
    SealIfNeeded();
}

RenderProfileFrameToken::RenderProfileFrameToken(RenderProfileFrameToken&& _other) noexcept :
    state_(std::move(_other.state_)),
    frame_(std::move(_other.frame_)),
    frame_id_(_other.frame_id_),
    sealed_(_other.sealed_) {
    _other.frame_id_ = 0;
    _other.sealed_   = true;
}

RenderProfileFrameToken& RenderProfileFrameToken::operator=(RenderProfileFrameToken&& _other) noexcept {
    if (this == &_other) {
        return *this;
    }
    SealIfNeeded();
    state_           = std::move(_other.state_);
    frame_           = std::move(_other.frame_);
    frame_id_        = _other.frame_id_;
    sealed_          = _other.sealed_;
    _other.frame_id_ = 0;
    _other.sealed_   = true;
    return *this;
}

bool RenderProfileFrameToken::Valid() const noexcept {
    if (sealed_ || !state_ || !frame_.Valid()) {
        return false;
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->closed) {
        return false;
    }
    if (!state_->RuntimeWritableUnlocked()) {
        state_->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
        return false;
    }
    return frame_.Valid();
}

std::uint64_t RenderProfileFrameToken::FrameId() const noexcept {
    return frame_id_;
}

void RenderProfileFrameToken::SealIfNeeded() noexcept {
    if (sealed_) {
        return;
    }
    sealed_ = true;
    if (state_ && frame_.Valid()) {
        std::scoped_lock lock(state_->mutex);
        if (!state_->closed) {
            if (!state_->RuntimeWritableUnlocked()) {
                state_->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
            } else if (state_->stream.SealFrame(frame_)) {
                ++state_->frames_sealed;
            }
        }
    }
    frame_ = {};
    state_.reset();
}

RenderProfileCapture::RenderProfileCapture(
    ProfileDump::SchemaHandle   _gpu_frame_schema,
    ProfileDump::SchemaHandle   _gpu_scope_schema,
    const GpuScopeStreamConfig& _stream_config
) {
    const RenderProfileSessionStartResult result =
        StartSession(_gpu_frame_schema, _gpu_scope_schema, _stream_config);
    if (result == RenderProfileSessionStartResult::InvalidConfiguration) {
        throw std::invalid_argument("GpuScopeStreamConfig is invalid");
    }
    if (result == RenderProfileSessionStartResult::ResourceExhausted) {
        throw std::bad_alloc{};
    }
}

RenderProfileCapture::~RenderProfileCapture() {
    Abort();
}

RenderProfileSessionStartResult RenderProfileCapture::StartSession(
    ProfileDump::SchemaHandle   _gpu_frame_schema,
    ProfileDump::SchemaHandle   _gpu_scope_schema,
    const GpuScopeStreamConfig& _stream_config
) noexcept {
    RenderProfileSessionStartResult validation = ValidateSessionStart(_gpu_frame_schema, _gpu_scope_schema);
    if (validation != RenderProfileSessionStartResult::Started) {
        return validation;
    }

    std::shared_ptr<RenderProfileDetail::CaptureState> replacement;
    try {
        replacement = std::make_shared<RenderProfileDetail::CaptureState>(
            _gpu_frame_schema, _gpu_scope_schema, _stream_config
        );
        if (!replacement->stream.Valid()) {
            return RenderProfileSessionStartResult::ResourceExhausted;
        }
    } catch (const std::invalid_argument&) {
        return RenderProfileSessionStartResult::InvalidConfiguration;
    } catch (...) {
        return RenderProfileSessionStartResult::ResourceExhausted;
    }

    auto observed = state_.load(std::memory_order_acquire);
    for (;;) {
        // A request can be delayed while ProfileDump advances and another
        // caller publishes the new generation. Revalidate before inspecting
        // or retiring the currently published state so a stale request can
        // never poison a newer live session.
        validation = ValidateSessionStart(_gpu_frame_schema, _gpu_scope_schema);
        if (validation != RenderProfileSessionStartResult::Started) {
            return validation;
        }

        if (observed) {
            std::scoped_lock lock(observed->mutex);
            if (observed->generation == _gpu_frame_schema.generation) {
                return observed->closed ? RenderProfileSessionStartResult::GenerationAlreadyUsed :
                                          RenderProfileSessionStartResult::AlreadyActive;
            }
        }

        // The ProfileDump runtime may have stopped or restarted while the
        // replacement state was being allocated. Never publish a facade
        // state whose schema handles no longer name the live generation.
        validation = ValidateSessionStart(_gpu_frame_schema, _gpu_scope_schema);
        if (validation != RenderProfileSessionStartResult::Started) {
            return validation;
        }

#if defined(MOER_RENDER_PROFILE_CAPTURE_TEST_HOOKS)
        PauseBeforeStartPublishForTesting();
#endif

        const auto retired = observed;
        if (state_.compare_exchange_weak(
                observed, replacement, std::memory_order_acq_rel, std::memory_order_acquire
            )) {
            // This is the publication linearization check. If ProfileDump
            // changed after the pre-CAS validation, fail only this candidate;
            // never mutate a state another caller may already have published.
            validation = ValidateSessionStart(_gpu_frame_schema, _gpu_scope_schema);
            if (validation != RenderProfileSessionStartResult::Started) {
                std::scoped_lock lock(replacement->mutex);
                replacement->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
                return validation;
            }
            if (retired) {
                std::scoped_lock lock(retired->mutex);
                if (!retired->closed && !retired->RuntimeWritableUnlocked()) {
                    retired->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
                }
            }
            return RenderProfileSessionStartResult::Started;
        }
    }
}

RenderProfileSessionStopResult RenderProfileCapture::RequestStop() noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    if (!state) {
        return RenderProfileSessionStopResult::Inactive;
    }

    std::scoped_lock lock(state->mutex);
    if (state->closed) {
        return RenderProfileSessionStopResult::Inactive;
    }
    if (!state->RuntimeWritableUnlocked()) {
        state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
        return RenderProfileSessionStopResult::Inactive;
    }
    if (state->stop_requested) {
        return RenderProfileSessionStopResult::AlreadyStopping;
    }
    state->stop_requested = true;
    state->accepting      = false;
    return RenderProfileSessionStopResult::StopRequested;
}

RenderProfileSessionFinishResult RenderProfileCapture::TryFinishSession() noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    if (!state) {
        return RenderProfileSessionFinishResult::Inactive;
    }

    std::scoped_lock lock(state->mutex);
    if (state->closed) {
        switch (state->terminal) {
            case RenderProfileDetail::SessionTerminal::Closed:
                return RenderProfileSessionFinishResult::Closed;
            case RenderProfileDetail::SessionTerminal::ClosedWithLoss:
                return RenderProfileSessionFinishResult::ClosedWithLoss;
            case RenderProfileDetail::SessionTerminal::Aborted:
                return RenderProfileSessionFinishResult::Aborted;
            case RenderProfileDetail::SessionTerminal::Faulted:
                return RenderProfileSessionFinishResult::Faulted;
            case RenderProfileDetail::SessionTerminal::None:
                break;
        }
        return RenderProfileSessionFinishResult::Faulted;
    }

    // Make the finish attempt self-contained if an owner omitted the
    // separate RequestStop call. Already-admitted tokens remain operational.
    state->stop_requested = true;
    state->accepting      = false;
    if (!state->RuntimeWritableUnlocked()) {
        state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
        return RenderProfileSessionFinishResult::Faulted;
    }

    static_cast<void>(state->DrainUnlocked());
    if (state->closed) {
        return RenderProfileSessionFinishResult::Faulted;
    }

    const GpuScopeStreamStats stream_stats = state->stream.GetStats();
    if (stream_stats.resident_frames != 0) {
        return RenderProfileSessionFinishResult::Pending;
    }
    if (stream_stats.resident_pending_frames != 0 || stream_stats.resident_ready_frames != 0 ||
        stream_stats.resident_scopes != 0 || !state->MetadataEmptyUnlocked()) {
        state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
        return RenderProfileSessionFinishResult::Faulted;
    }

    if (state->HasLossUnlocked()) {
        state->CloseUnlocked(RenderProfileDetail::SessionTerminal::ClosedWithLoss);
        return RenderProfileSessionFinishResult::ClosedWithLoss;
    }
    state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Closed);
    return RenderProfileSessionFinishResult::Closed;
}

bool RenderProfileCapture::Valid() const noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    if (!state) {
        return false;
    }
    std::scoped_lock lock(state->mutex);
    if (state->closed) {
        return false;
    }
    if (!state->RuntimeWritableUnlocked()) {
        state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
        return false;
    }
    return state->accepting;
}

RenderProfileFrameToken RenderProfileCapture::BeginFrame() noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    if (!state) {
        return {};
    }

    std::scoped_lock lock(state->mutex);
    if (state->closed) {
        return {};
    }
    if (!state->RuntimeWritableUnlocked()) {
        if (!state->closed) {
            state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
        }
        return {};
    }
    if (!state->accepting) {
        return {};
    }

    const std::uint64_t frame_id = state->next_frame_id++;
    ++state->frames_attempted;
    if (frame_id == 0 || state->next_frame_id == 0) {
        state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
        return {};
    }

    GpuScopeFrameHandle frame = state->stream.BeginFrame(frame_id);
    if (!frame.Valid()) {
        ++state->frames_admission_rejected;
        // Rejected frames do not enter GpuScopeStream's ordered frame FIFO.
        // Emitting them here would allow frame N to appear before an older
        // pending frame N-1. Keep the rejection in bounded bridge statistics;
        // admitted frame records remain strictly materialized in FIFO order.
        return RenderProfileFrameToken(state, {}, frame_id);
    }

    if (!state->ReserveMetadataUnlocked(frame_id)) {
        static_cast<void>(state->stream.SealFrame(frame));
        state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
        return {};
    }
    ++state->frames_admitted;
    return RenderProfileFrameToken(state, std::move(frame), frame_id);
}

RenderProfileBindResult RenderProfileCapture::BindSource(
    RenderProfileFrameToken& _frame,
    CommandList&             _command_list,
    RHIQueueBinding          _queue_binding,
    std::uint64_t            _source_order
) noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    if (!state || _frame.state_.get() != state.get() || _frame.sealed_ || !_frame.frame_.Valid()) {
        return RenderProfileBindResult::InvalidFrame;
    }

    std::scoped_lock lock(state->mutex);
    if (state->closed || !state->RuntimeWritableUnlocked()) {
        if (!state->closed) {
            state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
        }
        return RenderProfileBindResult::Inactive;
    }
    if (!IsManagedQueue(_queue_binding)) {
        state->MarkSourceFailureUnlocked(_frame.frame_id_);
        return RenderProfileBindResult::InvalidQueue;
    }

    GpuScopeRecorder recorder = _frame.frame_.CreateRecorder(_queue_binding, _source_order);
    if (!recorder.Valid()) {
        state->MarkSourceFailureUnlocked(_frame.frame_id_);
        return RenderProfileBindResult::SourceRejected;
    }

    try {
        _command_list.SetGpuScopeRecorder(std::move(recorder));
    } catch (...) {
        state->MarkSourceFailureUnlocked(_frame.frame_id_);
        return RenderProfileBindResult::CommandListRejected;
    }
    ++state->sources_bound;
    return RenderProfileBindResult::Bound;
}

bool RenderProfileCapture::Seal(RenderProfileFrameToken& _frame) noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    if (!state || _frame.state_.get() != state.get() || _frame.sealed_) {
        return false;
    }

    std::scoped_lock lock(state->mutex);
    bool             sealed = false;
    if (!state->closed && state->RuntimeWritableUnlocked()) {
        sealed = _frame.frame_.Valid() && state->stream.SealFrame(_frame.frame_);
    } else if (!state->closed) {
        state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
    }
    if (sealed) {
        ++state->frames_sealed;
    }
    _frame.sealed_ = true;
    _frame.frame_  = {};
    _frame.state_.reset();
    return sealed;
}

std::size_t RenderProfileCapture::DrainReadyFrames() noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    if (!state) {
        return 0;
    }
    std::scoped_lock lock(state->mutex);
    return state->DrainUnlocked();
}

void RenderProfileCapture::ShutdownAfterRhiDrain() noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    if (!state) {
        return;
    }
    std::scoped_lock lock(state->mutex);
    if (state->closed) {
        return;
    }
    state->stop_requested = true;
    state->accepting      = false;
    static_cast<void>(state->DrainUnlocked());
    if (!state->closed) {
        const GpuScopeStreamStats before_close = state->stream.GetStats();
        if (before_close.resident_frames != 0) {
            state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Aborted);
        } else if (before_close.resident_pending_frames != 0 || before_close.resident_ready_frames != 0 ||
                   before_close.resident_scopes != 0 || !state->MetadataEmptyUnlocked()) {
            state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
        } else {
            state->CloseUnlocked(
                state->HasLossUnlocked() ? RenderProfileDetail::SessionTerminal::ClosedWithLoss :
                                           RenderProfileDetail::SessionTerminal::Closed
            );
        }
    }
}

void RenderProfileCapture::Abort() noexcept {
    const auto state = state_.load(std::memory_order_acquire);
    if (!state) {
        return;
    }
    std::scoped_lock lock(state->mutex);
    state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Aborted);
}

RenderProfileCaptureStats RenderProfileCapture::GetStats() const noexcept {
    RenderProfileCaptureStats result{};
    const auto                state = state_.load(std::memory_order_acquire);
    if (!state) {
        return result;
    }

    std::scoped_lock lock(state->mutex);
    if (!state->closed && !state->RuntimeWritableUnlocked()) {
        state->CloseUnlocked(RenderProfileDetail::SessionTerminal::Faulted);
    }
    result.accepting                 = state->accepting;
    result.closed                    = state->closed;
    result.generation                = state->generation;
    result.frames_attempted          = state->frames_attempted;
    result.frames_admitted           = state->frames_admitted;
    result.frames_admission_rejected = state->frames_admission_rejected;
    result.frames_sealed             = state->frames_sealed;
    result.frames_emitted            = state->frames_emitted;
    result.frames_invalid            = state->frames_invalid;
    result.frame_records_dropped     = state->frame_records_dropped;
    result.sources_bound             = state->sources_bound;
    result.sources_rejected          = state->sources_rejected;
    result.scopes_emitted            = state->scopes_emitted;
    result.scope_records_dropped     = state->scope_records_dropped;
    result.terminal_faults           = state->terminal_faults;
    result.shutdown_abandoned_frames = state->shutdown_abandoned_frames;
    result.stream                    = state->stream.GetStats();
    return result;
}

#if defined(MOER_RENDER_PROFILE_CAPTURE_TEST_HOOKS)
namespace RenderProfileTesting {

void InjectNextStartValidationBadAlloc() noexcept {
    g_start_validation_exception.store(
        InjectedStartValidationException::BadAlloc,
        std::memory_order_release
    );
}

void InjectNextStartValidationException() noexcept {
    g_start_validation_exception.store(
        InjectedStartValidationException::Other,
        std::memory_order_release
    );
}

void ClearStartValidationException() noexcept {
    g_start_validation_exception.store(
        InjectedStartValidationException::None,
        std::memory_order_release
    );
}

void InstallStartPublishPause(std::atomic_uint32_t& _entered_count, std::atomic_bool& _release) noexcept {
    g_start_publish_pause_release.store(&_release, std::memory_order_release);
    g_start_publish_pause_entered.store(&_entered_count, std::memory_order_release);
}

void ClearStartPublishPause() noexcept {
    g_start_publish_pause_entered.store(nullptr, std::memory_order_release);
    g_start_publish_pause_release.store(nullptr, std::memory_order_release);
}

} // namespace RenderProfileTesting
#endif

} // namespace Moer::Render
