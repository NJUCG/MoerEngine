#include "profile/RenderProfileCapture.h"

#include "profile/ProfileDumpTemplates.h"
#include "rhi/RHICommand.h"

#include <array>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace Moer::Render {

namespace {

enum class EmitDisposition : std::uint8_t {
    Accepted,
    Dropped,
    Fatal,
};

bool IsManagedQueue(const RHIQueueBinding& _binding) noexcept {
    return _binding.available &&
           (_binding.queue == EQueueType::Graphics ||
            _binding.queue == EQueueType::Compute ||
            _binding.queue == EQueueType::Copy);
}

} // namespace

namespace RenderProfileDetail {

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
        return generation != 0 &&
               ProfileDump::GetRuntimeGeneration() == generation &&
               ProfileDump::GetRuntimeState() ==
                   ProfileDump::RuntimeState::Running;
    }

    void ClearMetadataUnlocked() noexcept {
        for (FrameMetadata& metadata : frame_metadata) {
            metadata = {};
        }
    }

    void CloseUnlocked(bool _terminal_fault) noexcept {
        if (closed) {
            return;
        }
        accepting = false;
        closed = true;
        if (_terminal_fault) {
            ++terminal_faults;
        }
        const GpuScopeStreamStats before_close = stream.GetStats();
        shutdown_abandoned_frames += before_close.resident_frames;
        stream.Close();
        ClearMetadataUnlocked();
    }

    [[nodiscard]] bool ReserveMetadataUnlocked(
        std::uint64_t _frame_id
    ) noexcept {
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

    [[nodiscard]] FrameMetadata TakeMetadataUnlocked(
        std::uint64_t _frame_id
    ) noexcept {
        for (FrameMetadata& metadata : frame_metadata) {
            if (metadata.frame_id == _frame_id) {
                const FrameMetadata result = metadata;
                metadata = {};
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

    [[nodiscard]] EmitDisposition HandleEmitUnlocked(
        ProfileDump::EmitStatus _status,
        bool                    _frame_record
    ) noexcept {
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
                CloseUnlocked(true);
                return EmitDisposition::Fatal;
        }
        CloseUnlocked(true);
        return EmitDisposition::Fatal;
    }

    [[nodiscard]] EmitDisposition EmitFrameRecordUnlocked(
        std::uint64_t            _frame_id,
        GpuFrameCaptureStatus    _status,
        bool                     _valid,
        std::uint64_t            _admitted_scope_count,
        std::uint64_t            _dropped_scope_count,
        std::uint64_t            _error_scope_count,
        std::string_view         _reason
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
        return HandleEmitUnlocked(
            ProfileDump::Emit(gpu_frame_schema, values),
            true
        );
    }

    [[nodiscard]] EmitDisposition EmitScopeRecordUnlocked(
        std::uint64_t       _frame_id,
        const GpuScopeNode& _scope
    ) noexcept {
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
        return HandleEmitUnlocked(
            ProfileDump::Emit(gpu_scope_schema, values),
            false
        );
    }

    [[nodiscard]] bool EmitScopeTreeUnlocked(
        std::uint64_t       _frame_id,
        const GpuScopeNode& _scope
    ) noexcept {
        if (EmitScopeRecordUnlocked(_frame_id, _scope) ==
            EmitDisposition::Fatal) {
            return false;
        }
        for (const GpuScopeNode& child : _scope.children) {
            if (!EmitScopeTreeUnlocked(_frame_id, child)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool EmitResolvedFrameUnlocked(
        const ResolvedGpuScopeFrame& _frame
    ) noexcept {
        const FrameMetadata metadata =
            TakeMetadataUnlocked(_frame.frame_id);
        const bool bridge_incomplete =
            metadata.source_bind_failures != 0;
        const bool valid = _frame.valid && !bridge_incomplete;

        GpuFrameCaptureStatus capture_status =
            GpuFrameCaptureStatus::Complete;
        std::string_view reason{};
        if (!valid) {
            ++frames_invalid;
            const bool has_errors = _frame.error_scope_count != 0;
            const bool has_drops =
                _frame.dropped_scope_count != 0 || bridge_incomplete;
            if (has_errors || (!has_drops && !_frame.valid)) {
                capture_status = GpuFrameCaptureStatus::Invalid;
            } else {
                capture_status = GpuFrameCaptureStatus::Incomplete;
            }

            if (has_errors && has_drops) {
                reason = "GPU frame capture contains failed and dropped scopes";
            } else if (has_errors) {
                reason = "one or more GPU scopes failed";
            } else if (bridge_incomplete &&
                       _frame.dropped_scope_count != 0) {
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
                CloseUnlocked(true);
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
            CloseUnlocked(true);
        }
        return drained;
    }

    ProfileDump::SchemaHandle gpu_frame_schema{};
    ProfileDump::SchemaHandle gpu_scope_schema{};
    std::uint64_t             generation{0};
    GpuScopeStream            stream;
    std::vector<FrameMetadata> frame_metadata{};

    mutable std::mutex mutex{};
    std::uint64_t next_frame_id{1};
    bool          accepting{true};
    bool          closed{false};

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

RenderProfileFrameToken::RenderProfileFrameToken(
    RenderProfileFrameToken&& _other
) noexcept :
    state_(std::move(_other.state_)),
    frame_(std::move(_other.frame_)),
    frame_id_(_other.frame_id_),
    sealed_(_other.sealed_) {
    _other.frame_id_ = 0;
    _other.sealed_ = true;
}

RenderProfileFrameToken& RenderProfileFrameToken::operator=(
    RenderProfileFrameToken&& _other
) noexcept {
    if (this == &_other) {
        return *this;
    }
    SealIfNeeded();
    state_ = std::move(_other.state_);
    frame_ = std::move(_other.frame_);
    frame_id_ = _other.frame_id_;
    sealed_ = _other.sealed_;
    _other.frame_id_ = 0;
    _other.sealed_ = true;
    return *this;
}

bool RenderProfileFrameToken::Valid() const noexcept {
    if (sealed_ || !state_ || !frame_.Valid()) {
        return false;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->accepting && !state_->closed &&
           state_->RuntimeWritableUnlocked() && frame_.Valid();
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
        if (!state_->closed && state_->stream.SealFrame(frame_)) {
            ++state_->frames_sealed;
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
    const std::uint64_t generation =
        ProfileDump::GetRuntimeGeneration();
    if (!_gpu_frame_schema || !_gpu_scope_schema ||
        generation == 0 ||
        ProfileDump::GetRuntimeState() !=
            ProfileDump::RuntimeState::Running ||
        _gpu_frame_schema.generation != generation ||
        _gpu_scope_schema.generation != generation ||
        _gpu_frame_schema.hash != ProfileDump::ComputeSchemaHash(
                                      ProfileDump::Templates::GpuFrame()
                                  ) ||
        _gpu_scope_schema.hash != ProfileDump::ComputeSchemaHash(
                                      ProfileDump::Templates::GpuScope()
                                  )) {
        return;
    }

    state_ = std::make_shared<RenderProfileDetail::CaptureState>(
        _gpu_frame_schema,
        _gpu_scope_schema,
        _stream_config
    );
}

RenderProfileCapture::~RenderProfileCapture() {
    Abort();
}

bool RenderProfileCapture::Valid() const noexcept {
    const auto state = state_;
    if (!state) {
        return false;
    }
    std::scoped_lock lock(state->mutex);
    return state->accepting && !state->closed &&
           state->RuntimeWritableUnlocked();
}

RenderProfileFrameToken RenderProfileCapture::BeginFrame() noexcept {
    const auto state = state_;
    if (!state) {
        return {};
    }

    std::scoped_lock lock(state->mutex);
    if (!state->accepting || state->closed ||
        !state->RuntimeWritableUnlocked()) {
        if (!state->closed) {
            state->CloseUnlocked(true);
        }
        return {};
    }

    const std::uint64_t frame_id = state->next_frame_id++;
    ++state->frames_attempted;
    if (frame_id == 0 ||
        state->next_frame_id == 0) {
        state->CloseUnlocked(true);
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
        state->CloseUnlocked(true);
        return {};
    }
    ++state->frames_admitted;
    return RenderProfileFrameToken(
        state, std::move(frame), frame_id
    );
}

RenderProfileBindResult RenderProfileCapture::BindSource(
    RenderProfileFrameToken& _frame,
    CommandList&             _command_list,
    RHIQueueBinding          _queue_binding,
    std::uint64_t            _source_order
) noexcept {
    const auto state = state_;
    if (!state || _frame.state_.get() != state.get() ||
        _frame.sealed_ || !_frame.frame_.Valid()) {
        return RenderProfileBindResult::InvalidFrame;
    }

    std::scoped_lock lock(state->mutex);
    if (!state->accepting || state->closed ||
        !state->RuntimeWritableUnlocked()) {
        if (!state->closed) {
            state->CloseUnlocked(true);
        }
        return RenderProfileBindResult::Inactive;
    }
    if (!IsManagedQueue(_queue_binding)) {
        state->MarkSourceFailureUnlocked(_frame.frame_id_);
        return RenderProfileBindResult::InvalidQueue;
    }

    GpuScopeRecorder recorder =
        _frame.frame_.CreateRecorder(_queue_binding, _source_order);
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

bool RenderProfileCapture::Seal(
    RenderProfileFrameToken& _frame
) noexcept {
    const auto state = state_;
    if (!state || _frame.state_.get() != state.get() ||
        _frame.sealed_) {
        return false;
    }

    std::scoped_lock lock(state->mutex);
    const bool sealed =
        !state->closed && _frame.frame_.Valid() &&
        state->stream.SealFrame(_frame.frame_);
    if (sealed) {
        ++state->frames_sealed;
    }
    _frame.sealed_ = true;
    _frame.frame_ = {};
    _frame.state_.reset();
    return sealed;
}

std::size_t RenderProfileCapture::DrainReadyFrames() noexcept {
    const auto state = state_;
    if (!state) {
        return 0;
    }
    std::scoped_lock lock(state->mutex);
    return state->DrainUnlocked();
}

void RenderProfileCapture::ShutdownAfterRhiDrain() noexcept {
    const auto state = state_;
    if (!state) {
        return;
    }
    std::scoped_lock lock(state->mutex);
    if (state->closed) {
        return;
    }
    state->accepting = false;
    static_cast<void>(state->DrainUnlocked());
    if (!state->closed) {
        state->closed = true;
        const GpuScopeStreamStats before_close =
            state->stream.GetStats();
        state->shutdown_abandoned_frames +=
            before_close.resident_frames;
        state->stream.Close();
        state->ClearMetadataUnlocked();
    }
}

void RenderProfileCapture::Abort() noexcept {
    const auto state = state_;
    if (!state) {
        return;
    }
    std::scoped_lock lock(state->mutex);
    state->CloseUnlocked(false);
}

RenderProfileCaptureStats
RenderProfileCapture::GetStats() const noexcept {
    RenderProfileCaptureStats result{};
    const auto state = state_;
    if (!state) {
        return result;
    }

    std::scoped_lock lock(state->mutex);
    result.accepting = state->accepting;
    result.closed = state->closed;
    result.generation = state->generation;
    result.frames_attempted = state->frames_attempted;
    result.frames_admitted = state->frames_admitted;
    result.frames_admission_rejected =
        state->frames_admission_rejected;
    result.frames_sealed = state->frames_sealed;
    result.frames_emitted = state->frames_emitted;
    result.frames_invalid = state->frames_invalid;
    result.frame_records_dropped =
        state->frame_records_dropped;
    result.sources_bound = state->sources_bound;
    result.sources_rejected = state->sources_rejected;
    result.scopes_emitted = state->scopes_emitted;
    result.scope_records_dropped =
        state->scope_records_dropped;
    result.terminal_faults = state->terminal_faults;
    result.shutdown_abandoned_frames =
        state->shutdown_abandoned_frames;
    result.stream = state->stream.GetStats();
    return result;
}

} // namespace Moer::Render
