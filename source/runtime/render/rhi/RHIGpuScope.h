#ifndef MOER_ENGINE_RHI_GPU_SCOPE_H
#define MOER_ENGINE_RHI_GPU_SCOPE_H

#include "RenderAPI.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIQuery.h"

#include <array>
#if defined(MOER_RHI_GPU_SCOPE_TEST_HOOKS)
#include <atomic>
#endif
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Moer::Render {

namespace GpuScopeDetail {
struct FrameState;
struct RecorderState;
struct StreamState;
} // namespace GpuScopeDetail

enum class GpuScopeTerminalStatus : std::uint8_t {
    Ready = 0,
    Error,
};

struct GpuScopeStreamConfig {
    // Frames remain resident from BeginFrame until the consumer pops them.
    // Reserving that slot at BeginFrame guarantees Completion never discovers
    // a full ready queue after a frame has already been admitted.
    std::size_t max_resident_frames{8};
    std::size_t max_pending_frames{4};
    std::size_t max_resident_scopes{16384};
    std::size_t max_scopes_per_frame{4096};
    std::size_t max_sources_per_frame{1024};
    std::uint32_t max_scope_depth{64};
    std::size_t max_scope_name_bytes{256};
    std::size_t max_error_reason_bytes{512};
};

struct GpuScopeStreamStats {
    bool          accepting{false};
    std::uint64_t frames_opened{0};
    std::uint64_t frames_sealed{0};
    std::uint64_t frames_ready{0};
    std::uint64_t frames_popped{0};
    std::uint64_t frames_dropped_resident_full{0};
    std::uint64_t frames_dropped_pending_full{0};
    std::uint64_t frames_dropped_duplicate_id{0};
    std::uint64_t frames_dropped_resource_exhaustion{0};
    std::uint64_t frames_abandoned_on_close{0};

    std::uint64_t sources_opened{0};
    std::uint64_t sources_dropped_capacity{0};
    std::uint64_t sources_dropped_duplicate_order{0};
    std::uint64_t sources_dropped_after_seal{0};

    std::uint64_t scopes_attempted{0};
    std::uint64_t scopes_admitted{0};
    std::uint64_t scopes_ready{0};
    std::uint64_t scopes_error{0};
    std::uint64_t scopes_dropped_resident_full{0};
    std::uint64_t scopes_dropped_frame_full{0};
    std::uint64_t scopes_dropped_name_too_large{0};
    std::uint64_t scopes_dropped_invalid_hierarchy{0};
    std::uint64_t scopes_dropped_suppressed_subtree{0};
    std::uint64_t scopes_dropped_after_seal{0};
    std::uint64_t scopes_dropped_resource_exhaustion{0};

    std::uint64_t resident_frames{0};
    std::uint64_t resident_pending_frames{0};
    std::uint64_t resident_ready_frames{0};
    std::uint64_t resident_scopes{0};
    std::uint64_t high_water_frames{0};
    std::uint64_t high_water_pending_frames{0};
    std::uint64_t high_water_ready_frames{0};
    std::uint64_t high_water_scopes{0};
};

struct GpuScopeNode {
    std::uint64_t scope_id{0};
    std::uint64_t parent_scope_id{0};
    std::uint64_t source_order{0};
    std::uint64_t local_order{0};
    std::uint32_t depth{0};
    RHIQueueBinding queue_binding{};
    std::string     name{};

    GpuScopeTerminalStatus status{GpuScopeTerminalStatus::Error};
    std::uint64_t          query_id{0};
    std::uint64_t          begin_tick{0};
    std::uint64_t          end_tick{0};
    std::uint32_t          valid_bits{0};
    double                 tick_period_ns{0.0};
    double                 total_duration_ns{0.0};
    double                 exclusive_duration_ns{0.0};
    std::string            error_reason{};

    Array<GpuScopeNode> children{};
};

struct ResolvedGpuScopeFrame {
    std::uint64_t frame_id{0};
    bool          valid{false};
    std::uint64_t admitted_scope_count{0};
    std::uint64_t dropped_scope_count{0};
    std::uint64_t error_scope_count{0};
    std::array<Array<GpuScopeNode>, 3> queue_roots{};
};

// Copyable, internal completion ticket. It intentionally owns only the
// bounded frame slot and never captures QueryToken/QueryFuture, avoiding a
// Future -> callback -> Future reference cycle.
class RENDER_API GpuScopeCompletionTicket {
public:
    GpuScopeCompletionTicket() = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] std::uint64_t ScopeId() const noexcept;

    void Resolve(const QueryResult& _result) const noexcept;
    void Fail(std::string_view _reason) const noexcept;

private:
    GpuScopeCompletionTicket(
        std::shared_ptr<GpuScopeDetail::FrameState> _frame,
        std::uint32_t                               _slot,
        std::uint64_t                               _scope_id
    ) noexcept;

    std::shared_ptr<GpuScopeDetail::FrameState> frame_{};
    std::uint32_t                               slot_{0};
    std::uint64_t                               scope_id_{0};

    friend class GpuScopeRecorder;
    friend struct GpuScopeDetail::FrameState;
};

// One stable topology source inside a frame. Recorders are move-only so a
// source_order cannot accidentally acquire multiple local-order counters.
class RENDER_API GpuScopeRecorder {
public:
    GpuScopeRecorder() = default;
    GpuScopeRecorder(const GpuScopeRecorder&) = delete;
    GpuScopeRecorder& operator=(const GpuScopeRecorder&) = delete;
    GpuScopeRecorder(GpuScopeRecorder&&) noexcept = default;
    GpuScopeRecorder& operator=(GpuScopeRecorder&&) noexcept = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] std::uint64_t FrameId() const noexcept;
    [[nodiscard]] std::uint64_t SourceOrder() const noexcept;
    [[nodiscard]] RHIQueueBinding QueueBinding() const noexcept;

private:
    explicit GpuScopeRecorder(
        std::shared_ptr<GpuScopeDetail::RecorderState> _state
    ) noexcept;

    [[nodiscard]] GpuScopeCompletionTicket TryBeginScope(
        std::string_view _name,
        std::uint64_t    _parent_scope_id,
        std::uint32_t    _depth
    ) const noexcept;
    void RecordSuppressedScope() const noexcept;
    [[nodiscard]] bool IsBound() const noexcept;
    [[nodiscard]] RHIQueueBinding BoundQueueBinding() const noexcept;

    std::shared_ptr<GpuScopeDetail::RecorderState> state_{};

    friend class CommandList;
    friend class GpuScopeFrameHandle;
};

class RENDER_API GpuScopeFrameHandle {
public:
    GpuScopeFrameHandle() = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] std::uint64_t FrameId() const noexcept;

    [[nodiscard]] GpuScopeRecorder CreateRecorder(
        RHIQueueBinding _queue_binding,
        std::uint64_t   _source_order
    ) const noexcept;

private:
    explicit GpuScopeFrameHandle(
        std::shared_ptr<GpuScopeDetail::FrameState> _state
    ) noexcept;

    std::shared_ptr<GpuScopeDetail::FrameState> state_{};

    friend class GpuScopeStream;
};

class RENDER_API GpuScopeStream {
public:
    explicit GpuScopeStream(
        const GpuScopeStreamConfig& _config = {}
    );
    ~GpuScopeStream();

    GpuScopeStream(const GpuScopeStream&) = delete;
    GpuScopeStream& operator=(const GpuScopeStream&) = delete;
    GpuScopeStream(GpuScopeStream&& _other) noexcept;
    GpuScopeStream& operator=(GpuScopeStream&& _other) noexcept;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] GpuScopeFrameHandle BeginFrame(
        std::uint64_t _frame_id
    ) noexcept;
    [[nodiscard]] bool SealFrame(
        const GpuScopeFrameHandle& _frame
    ) noexcept;

    // Seal is a linearizable producer boundary: admissions that started
    // before it are included, while later attempts are rejected. The frame
    // cannot become visible until all pre-Seal admissions and their queries
    // have reached a terminal state.
    // Frames are consumed in BeginFrame order, never callback arrival order.
    // Hierarchy construction and exclusive-time calculation happen here on
    // the consumer, not on Completion/rejection owners.
    [[nodiscard]] bool TryPopFrame(
        ResolvedGpuScopeFrame& _frame
    );

    [[nodiscard]] GpuScopeStreamStats GetStats() const noexcept;

    // Non-blocking terminal lifecycle boundary. It rejects new admission and
    // detaches resident frames; outstanding Query callbacks may safely finish
    // against their bounded frame state without touching this stream.
    void Close() noexcept;

private:
    std::shared_ptr<GpuScopeDetail::StreamState> state_{};
};

#if defined(MOER_RHI_GPU_SCOPE_TEST_HOOKS)
// Deterministic contract-test seam. Production builds do not declare or
// compile this gate.
namespace GpuScopeTesting {
RENDER_API void InstallAdmissionPause(
    std::atomic_uint32_t& _entered_count,
    std::atomic_bool&     _release
) noexcept;
RENDER_API void ClearAdmissionPause() noexcept;
} // namespace GpuScopeTesting
#endif

} // namespace Moer::Render

#endif // MOER_ENGINE_RHI_GPU_SCOPE_H
