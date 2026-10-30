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
#include "misc/Types.h"
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace Moer::Render {

namespace GpuScopeDetail {
struct FrameState;
struct RecorderState;
struct StreamState;
} // namespace GpuScopeDetail

enum class GpuScopeTerminalStatus : uint8 {
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
    uint32      max_scope_depth{64};
    std::size_t max_scope_name_bytes{256};
    std::size_t max_error_reason_bytes{512};
};

struct GpuScopeStreamStats {
    bool          accepting{false};
    uint64        frames_opened{0};
    uint64        frames_sealed{0};
    uint64        frames_ready{0};
    uint64        frames_popped{0};
    uint64        frames_dropped_resident_full{0};
    uint64        frames_dropped_pending_full{0};
    uint64        frames_dropped_duplicate_id{0};
    uint64        frames_dropped_resource_exhaustion{0};
    uint64        frames_abandoned_on_close{0};

    uint64 sources_opened{0};
    uint64 sources_dropped_capacity{0};
    uint64 sources_dropped_duplicate_order{0};
    uint64 sources_dropped_after_seal{0};

    uint64 scopes_attempted{0};
    uint64 scopes_admitted{0};
    uint64 scopes_ready{0};
    uint64 scopes_error{0};
    uint64 scopes_dropped_resident_full{0};
    uint64 scopes_dropped_frame_full{0};
    uint64 scopes_dropped_name_too_large{0};
    uint64 scopes_dropped_invalid_hierarchy{0};
    uint64 scopes_dropped_suppressed_subtree{0};
    uint64 scopes_dropped_after_seal{0};
    uint64 scopes_dropped_resource_exhaustion{0};

    uint64 resident_frames{0};
    uint64 resident_pending_frames{0};
    uint64 resident_ready_frames{0};
    uint64 resident_scopes{0};
    uint64 high_water_frames{0};
    uint64 high_water_pending_frames{0};
    uint64 high_water_ready_frames{0};
    uint64 high_water_scopes{0};
};

struct GpuScopeNode {
    uint64          scope_id{0};
    uint64          parent_scope_id{0};
    uint64          source_order{0};
    uint64          local_order{0};
    uint32          depth{0};
    RHIQueueBinding queue_binding{};
    std::string     name{};

    GpuScopeTerminalStatus status{GpuScopeTerminalStatus::Error};
    uint64                 query_id{0};
    uint64                 begin_tick{0};
    uint64                 end_tick{0};
    uint32                 valid_bits{0};
    double                 tick_period_ns{0.0};
    double                 total_duration_ns{0.0};
    double                 exclusive_duration_ns{0.0};
    std::string            error_reason{};

    Array<GpuScopeNode> children{};
};

struct ResolvedGpuScopeFrame {
    uint64                             frame_id{0};
    bool          valid{false};
    uint64                             admitted_scope_count{0};
    uint64                             dropped_scope_count{0};
    uint64                             error_scope_count{0};
    std::array<Array<GpuScopeNode>, 3> queue_roots{};
};

// Copyable, internal completion ticket. It intentionally owns only the
// bounded frame slot and never captures QueryToken/QueryFuture, avoiding a
// Future -> callback -> Future reference cycle.
class RENDER_API GpuScopeCompletionTicket {
public:
    GpuScopeCompletionTicket() = default;

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] uint64 ScopeId() const noexcept;

    void Resolve(const QueryResult& _result) const noexcept;
    void Fail(std::string_view _reason) const noexcept;

private:
    GpuScopeCompletionTicket(
        std::shared_ptr<GpuScopeDetail::FrameState> _frame,
        uint32                                      _slot,
        uint64                                      _scope_id
    ) noexcept;

    std::shared_ptr<GpuScopeDetail::FrameState> frame_{};
    uint32                                      slot_{0};
    uint64                                      scope_id_{0};

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
    [[nodiscard]] uint64          FrameId() const noexcept;
    [[nodiscard]] uint64          SourceOrder() const noexcept;
    [[nodiscard]] RHIQueueBinding QueueBinding() const noexcept;

private:
    explicit GpuScopeRecorder(
        std::shared_ptr<GpuScopeDetail::RecorderState> _state
    ) noexcept;

    [[nodiscard]] GpuScopeCompletionTicket TryBeginScope(
        std::string_view _name,
        uint64           _parent_scope_id,
        uint32           _depth
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
    [[nodiscard]] uint64 FrameId() const noexcept;

    [[nodiscard]] GpuScopeRecorder CreateRecorder(
        RHIQueueBinding _queue_binding,
        uint64          _source_order
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
        uint64 _frame_id
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
    std::atomic<uint32>& _entered_count,
    std::atomic_bool&    _release
) noexcept;
RENDER_API void ClearAdmissionPause() noexcept;
} // namespace GpuScopeTesting
#endif

} // namespace Moer::Render

#endif // MOER_ENGINE_RHI_GPU_SCOPE_H
