#pragma once

#include "rhi/RHIRecordDiagnostics.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace Moer::Render {
struct VulkanResolvedBarrierBatch;

struct BarrierSemanticDiagnostics {
    uint64 digest{0};
    uint32 buffer_count{0};
    uint32 texture_count{0};
    uint32 memory_count{0};
};



// One recording's summary and the reasons it could not be fully described.
struct VulkanSerialGoldenResult {
    SerialGoldenSummary summary{};
    uint32_t unresolved_count{0};
    uint32_t opaque_count{0};
    uint64_t unresolved_command_mask{0};
    uint64_t opaque_command_mask{0};
    uint32_t unresolved_native_buffers{0};
    uint32_t unresolved_native_images{0};
    std::optional<SerialBarrierItem> first_unresolved_buffer_barrier;
};

// Profile-only diagnostics for comparing serial command recording across runs.
// Native handles are lookup keys only and never enter a digest.
class VulkanSerialGoldenTrace {
public:
    VulkanSerialGoldenTrace();
    ~VulkanSerialGoldenTrace();

    VulkanSerialGoldenTrace(const VulkanSerialGoldenTrace&)            = delete;
    VulkanSerialGoldenTrace& operator=(const VulkanSerialGoldenTrace&) = delete;

    void BeginLayer(uint32_t _layer_ordinal);
    void PrimeCommandResources(
        const Command*         _command,
        uint32_t               _original_ordinal,
        const TCachedArgArray& _cached_args
    );
    void RecordCommand(
        const Command*        _command,
        uint32_t              _original_ordinal,
        const TCachedArgArray& _cached_args
    );
    void RegisterDerivedResources(const Command* _command);
    void EndLayer();

    void SetCurrentCommand(uint32_t _original_ordinal);

    StableSubmissionToken ResolveNativeBuffer(uint64_t _native_handle);
    StableSubmissionToken ResolveNativeImage(uint64_t _native_handle);
    void AddBarrier(const SerialBarrierItem& _item);
    // Observe the same resolved batch that will be recorded; the tracker has
    // no knowledge of trace IDs, hashes, or diagnostic output.
    BarrierSemanticDiagnostics RecordBarriers(
        const VulkanResolvedBarrierBatch& _barriers,
        uint64_t                          _group_ordinal,
        const SerialQueueFamilyMap&        _queue_family_map
    );
    void RecordUnresolvedBufferBarrier(const SerialBarrierItem& _item);

    // Called only from ExecuteNow's profile-on branch, after VisitCmd returned.
    // Replays the descriptor calls that the successful command just emitted.
    void RecordDescriptorsForCommand(
        const Command*         _command,
        const TCachedArgArray& _cached_args,
        uint64_t               _relative_descriptor_begin,
        uint64_t               _actual_descriptor_bytes
    );
    void RecordQueryEvent(
        SerialQueryEvent _event,
        std::string_view _name,
        uint64_t         _pipeline_stage_mask
    );

    void MarkUnresolved();

    [[nodiscard]] VulkanSerialGoldenResult Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Moer::Render
